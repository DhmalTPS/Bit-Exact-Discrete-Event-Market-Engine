// Streaming columnar output: the simulator appends rows into fixed-size chunks; full chunks are
// encoded into Parquet data pages (one page per chunk per column) — on a background thread when
// requested — so encoding overlaps the simulation and memory stays bounded by the encoded size.
//
// File layout: "PAR1", one row group whose column chunks are [optional dictionary page, data
// pages...] in schema order, Thrift FileMetaData footer carrying the reference `pandas` metadata.
// Integers: DELTA_BINARY_PACKED; strings: RLE_DICTIONARY; nullable: RLE/bit-packed def levels.
// Pages use the SNAPPY codec framed as one literal (valid Snappy; no compression work).
#pragma once
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sha256.hpp"

namespace t3 {
namespace out {

using Buf = std::vector<uint8_t>;
constexpr int64_t NUL = INT64_MIN;

inline void put_uleb(Buf& b, uint64_t v) {
    while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; }
    b.push_back(uint8_t(v));
}
inline uint8_t* put_uleb(uint8_t* p, uint64_t v) {
    while (v >= 0x80) { *p++ = uint8_t(v | 0x80); v >>= 7; }
    *p++ = uint8_t(v);
    return p;
}
inline uint64_t zz(int64_t v) { return (uint64_t(v) << 1) ^ uint64_t(v >> 63); }

// ---------------- Thrift compact protocol ----------------
class Thrift {
   public:
    explicit Thrift(Buf& b) : b_(b) {}
    enum T : uint8_t { BOOL_T = 1, BOOL_F = 2, I32 = 5, I64 = 6, BIN = 8, LIST = 9, STRUCT = 12 };
    void field(int id, uint8_t type) {
        int d = id - last_[depth_];
        if (d > 0 && d <= 15) b_.push_back(uint8_t((d << 4) | type));
        else { b_.push_back(type); put_uleb(b_, zz(id)); }
        last_[depth_] = id;
    }
    void i32(int id, int32_t v) { field(id, I32); put_uleb(b_, zz(v)); }
    void i64(int id, int64_t v) { field(id, I64); put_uleb(b_, zz(v)); }
    void str(int id, const std::string& s) { field(id, BIN); put_uleb(b_, s.size()); b_.insert(b_.end(), s.begin(), s.end()); }
    void begin_struct(int id) { field(id, STRUCT); last_[++depth_] = 0; }
    void begin_elem() { last_[++depth_] = 0; }
    void end_struct() { b_.push_back(0); depth_--; }
    void list(int id, uint8_t et, size_t n) {
        field(id, LIST);
        if (n < 15) b_.push_back(uint8_t((n << 4) | et));
        else { b_.push_back(uint8_t(0xF0 | et)); put_uleb(b_, n); }
    }
    void elem_i32(int32_t v) { put_uleb(b_, zz(v)); }
    void elem_str(const std::string& s) { put_uleb(b_, s.size()); b_.insert(b_.end(), s.begin(), s.end()); }
    void stop() { b_.push_back(0); }

   private:
    Buf& b_;
    int last_[16] = {0};
    int depth_ = 0;
};

// ---------------- value encoders ----------------
// Pack 32 uint64 values of width w (1..64) LSB-first; returns bytes written (= 4*w).
inline size_t pack32(uint8_t* p, const uint64_t* v, unsigned w) {
    uint64_t acc = 0;
    unsigned bits = 0;
    uint8_t* q = p;
    for (int k = 0; k < 32; k++) {
        uint64_t x = v[k];
        acc |= x << bits;
        unsigned nb = bits + w;
        if (nb >= 64) {
            std::memcpy(q, &acc, 8);
            q += 8;
            nb -= 64;
            acc = nb ? (x >> (w - nb)) : 0;
        }
        bits = nb;
    }
    // remaining bits (32*w is a multiple of 32 -> 0 or 32 bits left)
    if (bits) { std::memcpy(q, &acc, bits / 8); q += bits / 8; }
    return size_t(q - p);
}

// DELTA_BINARY_PACKED (block 128 = 4 miniblocks x 32). Appends to `o`.
inline void delta_encode(Buf& o, const int64_t* v, size_t n) {
    size_t start = o.size();
    o.resize(start + 32 + (n / 128 + 1) * (10 + 4 + 128 * 8));
    uint8_t* p = o.data() + start;
    p = put_uleb(p, 128);
    p = put_uleb(p, 4);
    p = put_uleb(p, n);
    p = put_uleb(p, zz(n ? v[0] : 0));
    uint64_t d[128];
    for (size_t i = 1; i < n;) {
        size_t cnt = std::min<size_t>(128, n - i);
        int64_t mind = INT64_MAX;
        for (size_t k = 0; k < cnt; k++) {
            int64_t x = int64_t(uint64_t(v[i + k]) - uint64_t(v[i + k - 1]));
            d[k] = uint64_t(x);
            mind = x < mind ? x : mind;
        }
        for (size_t k = cnt; k < 128; k++) d[k] = uint64_t(mind);
        p = put_uleb(p, zz(mind));
        uint8_t* wp = p;
        p += 4;
        for (int m = 0; m < 4; m++) {
            uint64_t* blk = d + 32 * m;
            if (size_t(32 * m) >= cnt) { wp[m] = 0; continue; }
            uint64_t orr = 0;
            for (int k = 0; k < 32; k++) { blk[k] -= uint64_t(mind); orr |= blk[k]; }
            unsigned w = orr ? unsigned(64 - __builtin_clzll(orr)) : 0;
            wp[m] = uint8_t(w);
            if (w) p += pack32(p, blk, w);
        }
        i += cnt;
    }
    o.resize(size_t(p - o.data()));
}

// RLE/bit-packing hybrid: a single bit-packed run (w <= 8).
inline void bitpack_run(Buf& o, const uint8_t* v, size_t n, unsigned w) {
    size_t groups = (n + 7) / 8;
    put_uleb(o, (uint64_t(groups) << 1) | 1);
    size_t base = o.size();
    o.resize(base + groups * w + 8);
    uint8_t* p = o.data() + base;
    uint64_t acc = 0;
    unsigned bits = 0;
    for (size_t k = 0; k < groups * 8; k++) {
        acc |= uint64_t(k < n ? v[k] : 0) << bits;
        bits += w;
        if (bits >= 56) {
            unsigned nbytes = bits / 8;
            std::memcpy(p, &acc, 8);
            p += nbytes;
            acc >>= nbytes * 8;
            bits -= nbytes * 8;
        }
    }
    while (bits > 0) { *p++ = uint8_t(acc); acc >>= 8; bits = bits > 8 ? bits - 8 : 0; }
    o.resize(base + groups * w);
}

// ---------------- column chunk / file ----------------
enum PType : int32_t { INT32 = 1, INT64 = 2, BYTE_ARRAY = 6 };

struct ColSpec {
    std::string name;
    PType ptype;
    bool optional = false;
    std::vector<std::string> dict;
};

class ColumnChunk {
   public:
    ColSpec spec;
    Buf data;            // encoded pages
    int64_t dict_size = -1;  // bytes of the dictionary page (if any) at the start of data
    int64_t nvalues = 0;

    void begin() {
        if (spec.ptype != BYTE_ARRAY) return;
        scratch_.clear();
        for (auto& s : spec.dict) {
            uint32_t L = uint32_t(s.size());
            scratch_.insert(scratch_.end(), (uint8_t*)&L, (uint8_t*)&L + 4);
            scratch_.insert(scratch_.end(), s.begin(), s.end());
        }
        emit(2, int32_t(spec.dict.size()), 0);
        dict_size = int64_t(data.size());
    }
    void page_i64(const int64_t* v, size_t n) {
        scratch_.clear();
        if (spec.optional) {
            def_.resize(n);
            vals_.clear();
            for (size_t k = 0; k < n; k++) {
                bool nn = v[k] != NUL;
                def_[k] = nn;
                if (nn) vals_.push_back(v[k]);
            }
            scratch_.resize(4);
            bitpack_run(scratch_, def_.data(), n, 1);
            uint32_t L = uint32_t(scratch_.size() - 4);
            std::memcpy(scratch_.data(), &L, 4);
            delta_encode(scratch_, vals_.data(), vals_.size());
        } else {
            delta_encode(scratch_, v, n);
        }
        emit(0, int32_t(n), 5);
        nvalues += int64_t(n);
    }
    void page_i32(const int32_t* v, size_t n) {
        tmp_.assign(v, v + n);
        page_i64(tmp_.data(), n);
    }
    void page_codes(const uint8_t* c, size_t n) {
        scratch_.clear();
        unsigned w = 1;
        while ((1u << w) < spec.dict.size()) w++;
        scratch_.push_back(uint8_t(w));
        bitpack_run(scratch_, c, n, w);
        emit(0, int32_t(n), 8);
        nvalues += int64_t(n);
    }

   private:
    Buf scratch_;
    std::vector<uint8_t> def_;
    std::vector<int64_t> vals_, tmp_;

    void emit(int type, int32_t num_values, int encoding) {
        size_t n = scratch_.size();
        uint8_t snap[16];
        uint8_t* s = put_uleb(snap, n);
        if (n) {
            if (n <= 60) *s++ = uint8_t((n - 1) << 2);
            else {
                uint32_t L = uint32_t(n - 1);
                *s++ = uint8_t(63 << 2);
                std::memcpy(s, &L, 4);
                s += 4;
            }
        }
        size_t sn = size_t(s - snap);
        Buf h;
        Thrift t(h);
        t.i32(1, type);
        t.i32(2, int32_t(n));
        t.i32(3, int32_t(n + sn));
        if (type == 2) {
            t.begin_struct(7);
            t.i32(1, num_values);
            t.i32(2, 0);
            t.end_struct();
        } else {
            t.begin_struct(5);
            t.i32(1, num_values);
            t.i32(2, encoding);
            t.i32(3, 3);
            t.i32(4, 3);
            t.end_struct();
        }
        t.stop();
        data.insert(data.end(), h.begin(), h.end());
        data.insert(data.end(), snap, snap + sn);
        data.insert(data.end(), scratch_.begin(), scratch_.end());
    }
};

// Assemble the file from column chunks; returns the byte segments in file order (header,
// chunk data..., footer) which the caller hashes and writes without concatenating.
struct FileImage {
    Buf head, foot;
    std::vector<std::pair<const uint8_t*, size_t>> segs;
    std::string sha256() const {
        // contiguous hash needs one buffer; segments are hashed via a staging copy of small size
        size_t total = 0;
        for (auto& s : segs) total += s.second;
        Buf all;
        all.reserve(total);
        for (auto& s : segs) all.insert(all.end(), s.first, s.first + s.second);
        return sha256_hex(all.data(), all.size());
    }
    void write(const std::string& path) const {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) throw std::runtime_error("cannot write " + path);
        std::vector<iovec> iov;
        for (auto& s : segs) if (s.second) iov.push_back(iovec{(void*)s.first, s.second});
        size_t i = 0;
        while (i < iov.size()) {
            int cnt = int(std::min<size_t>(iov.size() - i, 1024));
            ssize_t w = ::writev(fd, iov.data() + i, cnt);
            if (w < 0) { ::close(fd); throw std::runtime_error("write failed " + path); }
            size_t ww = size_t(w);
            while (ww && i < iov.size()) {
                if (ww >= iov[i].iov_len) { ww -= iov[i].iov_len; i++; }
                else { iov[i].iov_base = (uint8_t*)iov[i].iov_base + ww; iov[i].iov_len -= ww; ww = 0; }
            }
        }
        ::close(fd);
    }
};

inline void assemble(FileImage& img, std::vector<ColumnChunk>& cols, int64_t nrows, const std::string& pandas_meta) {
    img.head.assign({'P', 'A', 'R', '1'});
    img.segs.clear();
    img.segs.push_back({img.head.data(), img.head.size()});
    int64_t off = 4;
    std::vector<int64_t> offs;
    for (auto& c : cols) {
        offs.push_back(off);
        img.segs.push_back({c.data.data(), c.data.size()});
        off += int64_t(c.data.size());
    }
    Buf& fm = img.foot;
    fm.clear();
    Thrift t(fm);
    t.i32(1, 1);
    t.list(2, Thrift::STRUCT, cols.size() + 1);
    t.begin_elem();
    t.str(4, "schema");
    t.i32(5, int32_t(cols.size()));
    t.end_struct();
    for (auto& c : cols) {
        t.begin_elem();
        t.i32(1, c.spec.ptype);
        t.i32(3, c.spec.optional ? 1 : 0);
        t.str(4, c.spec.name);
        if (c.spec.ptype == BYTE_ARRAY) {
            t.i32(6, 0);
            t.begin_struct(10);
            t.begin_struct(1);
            t.end_struct();
            t.end_struct();
        }
        t.end_struct();
    }
    t.i64(3, nrows);
    t.list(4, Thrift::STRUCT, 1);
    t.begin_elem();
    t.list(1, Thrift::STRUCT, cols.size());
    int64_t total = 0;
    for (size_t k = 0; k < cols.size(); k++) {
        auto& c = cols[k];
        int64_t sz = int64_t(c.data.size());
        total += sz;
        t.begin_elem();
        t.i64(2, offs[k]);
        t.begin_struct(3);
        t.i32(1, c.spec.ptype);
        std::vector<int32_t> enc = c.spec.ptype == BYTE_ARRAY ? std::vector<int32_t>{0, 3, 8}
                                   : c.spec.optional ? std::vector<int32_t>{3, 5} : std::vector<int32_t>{5};
        t.list(2, Thrift::I32, enc.size());
        for (int32_t e : enc) t.elem_i32(e);
        t.list(3, Thrift::BIN, 1);
        t.elem_str(c.spec.name);
        t.i32(4, 1);
        t.i64(5, nrows);
        t.i64(6, sz);
        t.i64(7, sz);
        t.i64(9, offs[k] + (c.dict_size > 0 ? c.dict_size : 0));
        if (c.dict_size > 0) t.i64(11, offs[k]);
        t.end_struct();
        t.end_struct();
    }
    t.i64(2, total);
    t.i64(3, nrows);
    t.end_struct();
    t.list(5, Thrift::STRUCT, 1);
    t.begin_elem();
    t.str(1, "pandas");
    t.str(2, pandas_meta);
    t.end_struct();
    t.str(6, "t3sim parquet writer 2.0");
    t.stop();
    uint32_t L = uint32_t(fm.size());
    const uint8_t* lp = (const uint8_t*)&L;
    fm.insert(fm.end(), lp, lp + 4);
    fm.insert(fm.end(), {'P', 'A', 'R', '1'});
    img.segs.push_back({fm.data(), fm.size()});
}

// ---------------- background encoder ----------------
// Runs jobs (closures) in submission order on one worker thread, or inline when disabled.
class Worker {
   public:
    explicit Worker(bool async) : async_(async) {
        if (async_) th_ = std::thread([this] { loop(); });
    }
    ~Worker() { finish(); }
    void submit(std::function<void()> f) {
        if (!async_) { f(); return; }
        std::unique_lock<std::mutex> lk(m_);
        q_.push_back(std::move(f));
        cv_.notify_one();
    }
    // block until at most `n` jobs are pending (back-pressure keeps chunk pools bounded)
    void wait_pending(size_t n) {
        if (!async_) return;
        std::unique_lock<std::mutex> lk(m_);
        done_cv_.wait(lk, [&] { return q_.size() + busy_ <= n; });
    }
    void finish() {
        if (!async_ || !th_.joinable()) return;
        {
            std::unique_lock<std::mutex> lk(m_);
            stop_ = true;
            cv_.notify_one();
        }
        th_.join();
    }

   private:
    bool async_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<std::function<void()>> q_;
    size_t busy_ = 0;
    bool stop_ = false;
    void loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;
                f = std::move(q_.front());
                q_.pop_front();
                busy_ = 1;
            }
            f();
            {
                std::unique_lock<std::mutex> lk(m_);
                busy_ = 0;
                done_cv_.notify_all();
            }
        }
    }
};

}  // namespace out
}  // namespace t3
