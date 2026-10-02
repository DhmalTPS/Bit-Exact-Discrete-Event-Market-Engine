// SHA-256 with an x86 SHA-NI fast path (runtime-detected) and a portable fallback.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace t3 {

namespace sha_detail {
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void blocks_portable(uint32_t st[8], const uint8_t* p, size_t nblocks) {
    for (; nblocks--; p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }
}

#if defined(__x86_64__)
__attribute__((target("sha,sse4.1"))) inline void blocks_shani(uint32_t st[8], const uint8_t* p, size_t nblocks) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i TMP = _mm_loadu_si128((const __m128i*)&st[0]);
    __m128i STATE1 = _mm_loadu_si128((const __m128i*)&st[4]);
    TMP = _mm_shuffle_epi32(TMP, 0xB1);
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);
    __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);
    for (; nblocks--; p += 64) {
        __m128i ABEF_SAVE = STATE0, CDGH_SAVE = STATE1, MSG, MSG0, MSG1, MSG2, MSG3;
        MSG0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 0)), MASK);
        MSG1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 16)), MASK);
        MSG2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 32)), MASK);
        MSG3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 48)), MASK);
#define T3_RND(Mk, ki)                                                                  \
    MSG = _mm_add_epi32(Mk, _mm_loadu_si128((const __m128i*)&K[ki]));                   \
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);                                \
    MSG = _mm_shuffle_epi32(MSG, 0x0E);                                                 \
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        // rounds 0-15
        T3_RND(MSG0, 0)
        T3_RND(MSG1, 4)
        MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);
        T3_RND(MSG2, 8)
        MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);
        // rounds 12-63 (canonical order): run rounds with W=M[i], finalise M[i+1] from M[i] and
        // M[i-1], then start M[i-1]'s next schedule word with msg1.
        __m128i* M[4] = {&MSG0, &MSG1, &MSG2, &MSG3};
        for (int r = 12; r < 64; r += 4) {
            int i = (r / 4) & 3;
            __m128i& Cur = *M[i];
            __m128i& Nxt = *M[(i + 1) & 3];
            __m128i& Prv = *M[(i + 3) & 3];
            T3_RND(Cur, r)
            if (r < 60) {
                Nxt = _mm_add_epi32(Nxt, _mm_alignr_epi8(Cur, Prv, 4));
                Nxt = _mm_sha256msg2_epu32(Nxt, Cur);
                Prv = _mm_sha256msg1_epu32(Prv, Cur);
            }
        }
#undef T3_RND
        STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
        STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);
    }
    TMP = _mm_shuffle_epi32(STATE0, 0x1B);
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);
    _mm_storeu_si128((__m128i*)&st[0], STATE0);
    _mm_storeu_si128((__m128i*)&st[4], STATE1);
}
inline bool have_shani() {
    unsigned a, b, c, d;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    bool sha = (b >> 29) & 1;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    bool sse41 = (c >> 19) & 1, ssse3 = (c >> 9) & 1;
    return sha && sse41 && ssse3;
}
#endif
}  // namespace sha_detail

inline std::string sha256_hex(const uint8_t* data, size_t n, bool allow_hw = true) {
    using namespace sha_detail;
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    void (*fn)(uint32_t*, const uint8_t*, size_t) = blocks_portable;
#if defined(__x86_64__)
    static const bool hw = have_shani();
    if (hw && allow_hw) fn = blocks_shani;
#endif
    size_t full = n / 64;
    fn(st, data, full);
    uint8_t tail[128] = {0};
    size_t rem = n - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tl = (rem + 9 <= 64) ? 64 : 128;
    uint64_t bits = uint64_t(n) * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = uint8_t(bits >> (8 * i));
    fn(st, tail, tl / 64);
    char hex[65];
    for (int i = 0; i < 8; i++) std::snprintf(hex + 8 * i, 9, "%08x", st[i]);
    return std::string(hex, 64);
}

}  // namespace t3
