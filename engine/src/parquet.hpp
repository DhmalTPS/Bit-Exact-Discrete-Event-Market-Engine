// Minimal, dependency-free Parquet writer for the two Track-3 output tables.
//
// One row group, one data page per column. Integer columns use DELTA_BINARY_PACKED, string columns
// RLE_DICTIONARY, nullable columns carry RLE/bit-packed definition levels. Pages use the SNAPPY codec
// framed as a single literal (valid Snappy, zero compression work). The footer carries the same
// `pandas` schema metadata the reference files carry, so pd.read_parquet restores identical dtypes
// (in particular nullable Int64, which must not round-trip through float64).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace t3 {
namespace pq {

using Buf = std::vector<uint8_t>;

inline void put_uleb(Buf& b, uint64_t v) {
    while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; }
    b.push_back(uint8_t(v));
}
inline uint64_t zz(int64_t v) { return (uint64_t(v) << 1) ^ uint64_t(v >> 63); }

// ---------------- Thrift compact protocol ----------------
class Thrift {
   public:
    explicit Thrift(Buf& b) : b_(b) {}
    enum T : uint8_t { BOOL_T = 1, BOOL_F = 2, BYTE = 3, I16 = 4, I32 = 5, I64 = 6, DBL = 7, BIN = 8, LIST = 9, STRUCT = 12 };
    void field(int id, uint8_t type) {
        int d = id - last_[depth_];
        if (d > 0 && d <= 15) b_.push_back(uint8_t((d << 4) | type));
        else { b_.push_back(type); put_uleb(b_, zz(id)); }
        last_[depth_] = id;
    }
    void i32(int id, int32_t v) { field(id, I32); put_uleb(b_, zz(v)); }
    void i64(int id, int64_t v) { field(id, I64); put_uleb(b_, zz(v)); }
    void boolean(int id, bool v) { field(id, v ? BOOL_T : BOOL_F); }
    void str(int id, const std::string& s) { field(id, BIN); put_uleb(b_, s.size()); b_.insert(b_.end(), s.begin(), s.end()); }
    void begin_struct(int id) { field(id, STRUCT); last_[++depth_] = 0; }
    void begin_struct_elem() { last_[++depth_] = 0; }  // struct inside a list
    void end_struct() { b_.push_back(0); depth_--; }
    void list(int id, uint8_t elem_type, size_t n) {
        field(id, LIST);
        if (n < 15) b_.push_back(uint8_t((n << 4) | elem_type));
        else { b_.push_back(uint8_t(0xF0 | elem_type)); put_uleb(b_, n); }
    }
    void list_i32_elem(int32_t v) { put_uleb(b_, zz(v)); }
    void list_str_elem(const std::string& s) { put_uleb(b_, s.size()); b_.insert(b_.end(), s.begin(), s.end()); }
    void stop() { b_.push_back(0); }

   private:
    Buf& b_;
    int last_[16] = {0};
    int depth_ = 0;
};

// ---------------- encoders ----------------
// DELTA_BINARY_PACKED: block 128 values = 4 miniblocks of 32.
inline void delta_encode(Buf& out, const int64_t* v, size_t n) {
    constexpr size_t BLOCK = 128, MINI = 32, NMINI = 4;
    put_uleb(out, BLOCK);
    put_uleb(out, NMINI);
    put_uleb(out, n);
    put_uleb(out, zz(n ? v[0] : 0));
    if (n <= 1) return;
    uint64_t deltas[BLOCK];
    size_t i = 1;
    while (i < n) {
        size_t cnt = std::min(BLOCK, n - i);
        int64_t mind = INT64_MAX;
        for (size_t k = 0; k < cnt; k++) {
            int64_t d = int64_t(uint64_t(v[i + k]) - uint64_t(v[i + k - 1]));
            deltas[k] = uint64_t(d);
            if (d < mind) mind = d;
        }
        put_uleb(out, zz(mind));
        uint8_t widths[NMINI] = {0, 0, 0, 0};
        for (size_t m = 0; m < NMINI; m++) {
            size_t s = m * MINI;
            if (s >= cnt) break;
            uint64_t mx = 0;
            for (size_t k = s; k < std::min(cnt, s + MINI); k++) {
                deltas[k] -= uint64_t(mind);
                mx |= deltas[k];
            }
            widths[m] = mx ? uint8_t(64 - __builtin_clzll(mx)) : 0;
        }
        for (size_t m = 0; m < NMINI; m++) out.push_back(widths[m]);
        for (size_t m = 0; m < NMINI; m++) {
            size_t s = m * MINI;
            if (s >= cnt) break;
            unsigned w = widths[m];
            if (!w) continue;
            size_t base = out.size();
            out.resize(base + 4 * w);  // 32 values * w bits / 8
            uint8_t* p = out.data() + base;
            std::memset(p, 0, 4 * w);
            unsigned __int128 acc = 0;
            unsigned bits = 0;
            size_t o = 0;
            for (size_t k = 0; k < MINI; k++) {
                uint64_t x = (s + k < cnt) ? deltas[s + k] : 0;
                acc |= (unsigned __int128)x << bits;
                bits += w;
                while (bits >= 8) { p[o++] = uint8_t(acc); acc >>= 8; bits -= 8; }
            }
        }
        i += cnt;
    }
}

// RLE/bit-packing hybrid, one bit-packed run covering all values (bit width w <= 8).
inline void bitpack_run(Buf& out, const uint8_t* v, size_t n, unsigned w) {
    size_t groups = (n + 7) / 8;
    put_uleb(out, (uint64_t(groups) << 1) | 1);
    size_t base = out.size();
    out.resize(base + groups * w);
    uint8_t* p = out.data() + base;
    uint64_t acc = 0;
    unsigned bits = 0;
    size_t o = 0;
    for (size_t k = 0; k < groups * 8; k++) {
        acc |= uint64_t(k < n ? v[k] : 0) << bits;
        bits += w;
        while (bits >= 8) { p[o++] = uint8_t(acc); acc >>= 8; bits -= 8; }
    }
}

// ---------------- table writer ----------------
enum PType : int32_t { INT32 = 1, INT64 = 2, BYTE_ARRAY = 6 };

struct Column {
    std::string name;
    PType ptype;
    bool optional = false;
    // exactly one data source:
    const int64_t* i64 = nullptr;
    const int32_t* i32 = nullptr;
    const uint8_t* codes = nullptr;              // dictionary codes
    std::vector<std::string> dict;               // dictionary values
    int64_t null_value = INT64_MIN;              // sentinel for optional int64
};

class Writer {
   public:
    explicit Writer(Buf& out) : out_(out) {}

    void write(std::vector<Column>& cols, size_t nrows, const std::string& pandas_meta) {
        out_.clear();
        out_.reserve(64 + nrows * cols.size() * 3);
        put("PAR1", 4);
        std::vector<ChunkMeta> metas;
        for (auto& c : cols) metas.push_back(write_column(c, nrows));
        // FileMetaData
        Buf fm;
        Thrift t(fm);
        t.i32(1, 1);  // version
        t.list(2, Thrift::STRUCT, cols.size() + 1);
        t.begin_struct_elem();  // root
        t.str(4, "schema");
        t.i32(5, int32_t(cols.size()));
        t.end_struct();
        for (auto& c : cols) {
            t.begin_struct_elem();
            t.i32(1, c.ptype);
            t.i32(3, c.optional ? 1 : 0);
            t.str(4, c.name);
            if (c.ptype == BYTE_ARRAY) {
                t.i32(6, 0);  // ConvertedType UTF8
                t.begin_struct(10);  // LogicalType
                t.begin_struct(1);   // STRING
                t.end_struct();
                t.end_struct();
            }
            t.end_struct();
        }
        t.i64(3, int64_t(nrows));
        t.list(4, Thrift::STRUCT, 1);
        t.begin_struct_elem();  // RowGroup
        t.list(1, Thrift::STRUCT, cols.size());
        int64_t total = 0;
        for (size_t k = 0; k < cols.size(); k++) {
            const ChunkMeta& m = metas[k];
            total += m.size;
            t.begin_struct_elem();  // ColumnChunk
            t.i64(2, m.offset);
            t.begin_struct(3);  // ColumnMetaData
            t.i32(1, cols[k].ptype);
            std::vector<int32_t> enc;
            if (cols[k].ptype == BYTE_ARRAY) enc = {0, 3, 8};  // PLAIN (dict page), RLE (levels), RLE_DICTIONARY
            else enc = cols[k].optional ? std::vector<int32_t>{3, 5} : std::vector<int32_t>{5};
            t.list(2, Thrift::I32, enc.size());
            for (int32_t e : enc) t.list_i32_elem(e);
            t.list(3, Thrift::BIN, 1);
            t.list_str_elem(cols[k].name);
            t.i32(4, 1);  // SNAPPY
            t.i64(5, int64_t(nrows));
            t.i64(6, m.size);
            t.i64(7, m.size);
            t.i64(9, m.data_off);
            if (m.dict_off >= 0) t.i64(11, m.dict_off);
            t.end_struct();
            t.end_struct();
        }
        t.i64(2, total);
        t.i64(3, int64_t(nrows));
        t.end_struct();
        t.list(5, Thrift::STRUCT, 1);
        t.begin_struct_elem();
        t.str(1, "pandas");
        t.str(2, pandas_meta);
        t.end_struct();
        t.str(6, "t3sim parquet writer 1.0");
        t.stop();
        out_.insert(out_.end(), fm.begin(), fm.end());
        uint32_t L = uint32_t(fm.size());
        put(&L, 4);
        put("PAR1", 4);
    }

   private:
    struct ChunkMeta { int64_t offset, data_off, dict_off, size; };
    Buf& out_;
    Buf page_;

    void put(const void* p, size_t n) { const uint8_t* c = (const uint8_t*)p; out_.insert(out_.end(), c, c + n); }

    // page header + snappy single-literal framing around page_
    void emit_page(int type, int32_t num_values, int encoding, bool dict) {
        size_t n = page_.size();
        Buf snap;
        put_uleb(snap, n);
        if (n) {
            if (n <= 60) snap.push_back(uint8_t((n - 1) << 2));
            else {
                uint32_t L = uint32_t(n - 1);
                snap.push_back(uint8_t(63 << 2));
                snap.push_back(uint8_t(L)); snap.push_back(uint8_t(L >> 8));
                snap.push_back(uint8_t(L >> 16)); snap.push_back(uint8_t(L >> 24));
            }
        }
        Buf h;
        Thrift t(h);
        t.i32(1, type);
        t.i32(2, int32_t(n));
        t.i32(3, int32_t(n + snap.size()));
        if (dict) {
            t.begin_struct(7);
            t.i32(1, num_values);
            t.i32(2, 0);  // PLAIN
            t.end_struct();
        } else {
            t.begin_struct(5);
            t.i32(1, num_values);
            t.i32(2, encoding);
            t.i32(3, 3);  // RLE
            t.i32(4, 3);  // RLE
            t.end_struct();
        }
        t.stop();
        put(h.data(), h.size());
        put(snap.data(), snap.size());
        put(page_.data(), n);
    }

    ChunkMeta write_column(const Column& c, size_t n) {
        ChunkMeta m{int64_t(out_.size()), 0, -1, 0};
        if (c.ptype == BYTE_ARRAY) {
            page_.clear();
            for (auto& s : c.dict) {
                uint32_t L = uint32_t(s.size());
                page_.insert(page_.end(), (uint8_t*)&L, (uint8_t*)&L + 4);
                page_.insert(page_.end(), s.begin(), s.end());
            }
            m.dict_off = int64_t(out_.size());
            emit_page(2, int32_t(c.dict.size()), 0, true);
            m.data_off = int64_t(out_.size());
            page_.clear();
            unsigned w = 1;
            while ((1u << w) < c.dict.size()) w++;
            page_.push_back(uint8_t(w));
            bitpack_run(page_, c.codes, n, w);
            emit_page(0, int32_t(n), 8, false);
        } else {
            m.data_off = int64_t(out_.size());
            page_.clear();
            if (c.optional) {
                std::vector<uint8_t> def(n);
                std::vector<int64_t> vals;
                vals.reserve(n);
                for (size_t k = 0; k < n; k++) {
                    bool nn = c.i64[k] != c.null_value;
                    def[k] = nn;
                    if (nn) vals.push_back(c.i64[k]);
                }
                Buf lv;
                bitpack_run(lv, def.data(), n, 1);
                uint32_t L = uint32_t(lv.size());
                page_.insert(page_.end(), (uint8_t*)&L, (uint8_t*)&L + 4);
                page_.insert(page_.end(), lv.begin(), lv.end());
                delta_encode(page_, vals.data(), vals.size());
            } else if (c.i64) {
                delta_encode(page_, c.i64, n);
            } else {
                std::vector<int64_t> tmp(c.i32, c.i32 + n);
                delta_encode(page_, tmp.data(), n);
            }
            emit_page(0, int32_t(n), 5, false);
        }
        m.size = int64_t(out_.size()) - m.offset;
        return m;
    }
};

}  // namespace pq
}  // namespace t3
