// Minimal JSON DOM for scenario files. Numbers remember whether they were written as integers so
// Python-style conversions (int(x) truncates a float, float(x) widens an int) can be mirrored exactly.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace t3 {

struct JVal {
    enum Kind { NUL, BOOL, INT, FLT, STR, ARR, OBJ } kind = NUL;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* k) const {
        if (kind != OBJ) return nullptr;
        for (auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool has(const char* k) const { return get(k) != nullptr; }
    bool is_num() const { return kind == INT || kind == FLT; }
    // Python truthiness
    bool truthy() const {
        switch (kind) {
            case NUL: return false;
            case BOOL: return b;
            case INT: return i != 0;
            case FLT: return f != 0.0;
            case STR: return !s.empty();
            case ARR: return !arr.empty();
            case OBJ: return !obj.empty();
        }
        return false;
    }
    double as_float() const {
        if (kind == INT) return double(i);
        if (kind == FLT) return f;
        if (kind == BOOL) return b ? 1.0 : 0.0;
        if (kind == STR) return std::strtod(s.c_str(), nullptr);
        throw std::runtime_error("not a number");
    }
    int64_t as_int() const {  // Python int(): truncation toward zero for floats
        if (kind == INT) return i;
        if (kind == FLT) return int64_t(f);
        if (kind == BOOL) return b ? 1 : 0;
        if (kind == STR) return std::strtoll(s.c_str(), nullptr, 10);
        throw std::runtime_error("not an integer");
    }
};

class JParser {
   public:
    explicit JParser(const std::string& t) : p(t.c_str()), e(t.c_str() + t.size()) {}
    JVal parse() {
        JVal v = value();
        ws();
        return v;
    }

   private:
    const char* p;
    const char* e;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    [[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("json: ") + m); }
    JVal value() {
        ws();
        if (p >= e) fail("eof");
        JVal v;
        char c = *p;
        if (c == '{') {
            v.kind = JVal::OBJ;
            p++;
            ws();
            if (*p == '}') { p++; return v; }
            for (;;) {
                ws();
                std::string k = str();
                ws();
                if (*p != ':') fail("expected :");
                p++;
                v.obj.emplace_back(std::move(k), value());
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; break; }
                fail("expected , or }");
            }
        } else if (c == '[') {
            v.kind = JVal::ARR;
            p++;
            ws();
            if (*p == ']') { p++; return v; }
            for (;;) {
                v.arr.push_back(value());
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; break; }
                fail("expected , or ]");
            }
        } else if (c == '"') {
            v.kind = JVal::STR;
            v.s = str();
        } else if (!std::strncmp(p, "true", 4)) {
            v.kind = JVal::BOOL; v.b = true; p += 4;
        } else if (!std::strncmp(p, "false", 5)) {
            v.kind = JVal::BOOL; v.b = false; p += 5;
        } else if (!std::strncmp(p, "null", 4)) {
            v.kind = JVal::NUL; p += 4;
        } else if (!std::strncmp(p, "NaN", 3)) {
            v.kind = JVal::FLT; v.f = std::nan(""); p += 3;
        } else {
            const char* s0 = p;
            bool flt = false;
            if (*p == '-' || *p == '+') p++;
            while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
                if (*p == '.' || *p == 'e' || *p == 'E') flt = true;
                p++;
            }
            std::string num(s0, p);
            if (num.empty()) fail("bad token");
            if (flt) {
                v.kind = JVal::FLT;
                v.f = std::strtod(num.c_str(), nullptr);
            } else {
                errno = 0;
                long long x = std::strtoll(num.c_str(), nullptr, 10);
                if (errno == ERANGE) {
                    v.kind = JVal::FLT;
                    v.f = std::strtod(num.c_str(), nullptr);
                } else {
                    v.kind = JVal::INT;
                    v.i = x;
                }
            }
        }
        return v;
    }
    std::string str() {
        if (*p != '"') fail("expected string");
        p++;
        std::string out;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                p++;
                char c = *p++;
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = std::strtoul(std::string(p, p + 4).c_str(), nullptr, 16);
                        p += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += c;
                }
            } else {
                out += *p++;
            }
        }
        if (p >= e) fail("unterminated string");
        p++;
        return out;
    }
};

inline JVal parse_json(const std::string& text) { return JParser(text).parse(); }

}  // namespace t3
