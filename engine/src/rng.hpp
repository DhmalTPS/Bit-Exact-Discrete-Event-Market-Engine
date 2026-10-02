// Bit-exact port of numpy's legacy np.random.RandomState (MT19937 + legacy distributions).
// Every routine mirrors numpy/random/src/{mt19937,legacy,distributions} so a seed yields the same
// stream of draws as the ABIDES baseline. libm calls (exp/log/expm1/sqrt) go to the platform libm;
// on glibc these are the very functions numpy and CPython call.
#pragma once
#include <cmath>
#include <cstdint>

namespace t3 {

struct MT19937 {
    static constexpr int N = 624, M = 397;
    uint32_t key[N];
    int pos = N;
    bool has_gauss = false;
    double gauss_cache = 0.0;

    MT19937() { seed(5489u); }
    explicit MT19937(uint32_t s) { seed(s); }

    // mt19937_seed (legacy integer seeding); also resets the cached gaussian.
    void seed(uint32_t s) {
        for (int i = 0; i < N; i++) {
            key[i] = s;
            s = 1812433253u * (s ^ (s >> 30)) + uint32_t(i + 1);
        }
        pos = N;
        has_gauss = false;
        gauss_cache = 0.0;
    }

    void twist() {
        constexpr uint32_t A = 0x9908b0dfu, UP = 0x80000000u, LO = 0x7fffffffu;
        int i = 0;
        uint32_t y;
        for (; i < N - M; i++) {
            y = (key[i] & UP) | (key[i + 1] & LO);
            key[i] = key[i + M] ^ (y >> 1) ^ (-(y & 1u) & A);
        }
        for (; i < N - 1; i++) {
            y = (key[i] & UP) | (key[i + 1] & LO);
            key[i] = key[i + (M - N)] ^ (y >> 1) ^ (-(y & 1u) & A);
        }
        y = (key[N - 1] & UP) | (key[0] & LO);
        key[N - 1] = key[M - 1] ^ (y >> 1) ^ (-(y & 1u) & A);
        pos = 0;
    }

    inline uint32_t next32() {
        if (pos == N) twist();
        uint32_t y = key[pos++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }

    // random_standard_uniform / legacy_double: 53-bit double in [0, 1).
    inline double next_double() {
        int32_t a = int32_t(next32() >> 5), b = int32_t(next32() >> 6);
        return (a * 67108864.0 + b) / 9007199254740992.0;
    }

    // legacy_gauss: Marsaglia polar method with one cached value.
    inline double gauss() {
        if (has_gauss) {
            has_gauss = false;
            double t = gauss_cache;
            gauss_cache = 0.0;
            return t;
        }
        double x1, x2, r2;
        do {
            x1 = 2.0 * next_double() - 1.0;
            x2 = 2.0 * next_double() - 1.0;
            r2 = x1 * x1 + x2 * x2;
        } while (r2 >= 1.0 || r2 == 0.0);
        double f = std::sqrt(-2.0 * std::log(r2) / r2);
        gauss_cache = f * x1;
        has_gauss = true;
        return f * x2;
    }

    inline double normal(double loc, double scale) { return loc + scale * gauss(); }
    inline double lognormal(double mean, double sigma) { return std::exp(normal(mean, sigma)); }
    inline double standard_exponential() { return -std::log(1.0 - next_double()); }
    inline double exponential(double scale) { return scale * standard_exponential(); }
    // legacy_pareto keeps exp(x)-1 (the new Generator uses expm1; the two differ in the last ulps).
    inline double pareto(double a) { return std::exp(standard_exponential() / a) - 1; }
    inline double uniform(double lo, double hi) { return lo + (hi - lo) * next_double(); }

    // RandomState.randint(low, high) for the default int64 dtype (legacy masked rejection).
    inline int64_t randint(int64_t low, int64_t high) {
        uint64_t rng = uint64_t(high - low - 1);
        if (rng == 0) return low;
        if (rng == 0xFFFFFFFFull) return low + int64_t(next32());
        if (rng < 0xFFFFFFFFull) {
            uint32_t mask = uint32_t(rng);
            mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4; mask |= mask >> 8; mask |= mask >> 16;
            uint32_t v;
            while ((v = (next32() & mask)) > uint32_t(rng)) {}
            return low + int64_t(v);
        }
        // 64-bit masked path (never reached by the baseline's draws, kept for completeness).
        uint64_t mask = rng;
        mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4; mask |= mask >> 8; mask |= mask >> 16; mask |= mask >> 32;
        uint64_t v;
        do {
            v = (uint64_t(next32()) << 32) | next32();
            v &= mask;
        } while (v > rng);
        return low + int64_t(v);
    }

    // np.random.randint(low=0, high=2**32, dtype="uint64"): the full-range uint32 fast path.
    inline uint32_t seed_draw() { return next32(); }
};

}  // namespace t3
