// LatencyStream: the latency RandomState is consumed once per inter-agent message, in send order,
// and each draw is independent of market state. The sequence of realised integer latencies is
// therefore fixed by the seed alone, so it can be generated ahead of use — on a producer thread
// (async) or in tight inline blocks — without changing a single value or its order.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "rng.hpp"

namespace t3 {

struct LatParams {
    uint8_t model;  // 0 det, 1 lognormal, 2 uniform, 3 pareto (matches sim.hpp LatModel)
    double mean, sigma, lo, hi, alpha, mu;
};

inline int64_t lat_draw(MT19937& rs, const LatParams& p) {
    double v;
    switch (p.model) {
        case 1: v = rs.lognormal(p.mu, p.sigma); break;
        case 2: v = rs.uniform(p.lo, p.hi); break;
        case 3: {
            double base = p.lo > 0 ? p.lo : 1.0;
            v = base * (1.0 + rs.pareto(p.alpha));
            break;
        }
        default: v = p.mean;
    }
    // np.clip -> _NPY_MIN(_NPY_MAX(v, lo), hi) with NaN propagation; then int(round(float(.)))
    double c = std::isnan(v) ? v : (v > p.lo ? v : p.lo);
    c = std::isnan(c) ? c : (c < p.hi ? c : p.hi);
    return int64_t(std::nearbyint(c));
}

class LatencyStream {
   public:
    static constexpr size_t BLOCK = 8192, NBLK = 8;  // ring of 8 blocks
    // Block g holds the next bsize(g) draws: small first blocks keep start-up cheap for tiny
    // scenarios; consecutive blocks are consecutive draws, so the stream is unchanged.
    static constexpr size_t bsize(uint64_t g) { return g >= 5 ? BLOCK : (size_t(256) << g); }

    LatencyStream(uint32_t seed, const LatParams& p, bool async) : p_(p), async_(async) {
        rs_.seed(seed);
        ring_.reset(new int64_t[BLOCK * NBLK]);
        for (auto& r : ready_) r.store(0, std::memory_order_relaxed);
        if (p_.model == 0) {  // deterministic: constant, no RNG involved at all
            det_ = lat_draw(rs_, p_);
            return;
        }
        if (async_) {
            th_ = std::thread([this] { produce(); });
            while (ready_[0].load(std::memory_order_acquire) != 1) std::this_thread::yield();
        } else {
            fill(0, 0);
        }
        cur_ = ring_.get();
        end_ = cur_ + bsize(0);
    }
    ~LatencyStream() {
        stop_.store(true, std::memory_order_release);
        if (th_.joinable()) th_.join();
    }
    inline int64_t next() {
        if (p_.model == 0) return det_;
        if (cur_ == end_) advance();
        return *cur_++;
    }

   private:
    LatParams p_;
    bool async_;
    MT19937 rs_;
    int64_t det_ = 0;
    std::unique_ptr<int64_t[]> ring_;
    // ready_[b] = (generation number of the block currently stored in slot b) + 1
    std::atomic<uint64_t> ready_[NBLK];
    std::atomic<uint64_t> consumed_{0};  // number of blocks fully consumed
    std::atomic<bool> stop_{false};
    std::thread th_;
    int64_t* cur_ = nullptr;
    int64_t* end_ = nullptr;
    uint64_t gen_ = 0;  // generation the consumer is reading

    void fill(size_t slot, uint64_t gen) {
        int64_t* b = ring_.get() + slot * BLOCK;
        const size_t n = bsize(gen);
        for (size_t k = 0; k < n; k++) b[k] = lat_draw(rs_, p_);
        ready_[slot].store(gen + 1, std::memory_order_release);
    }
    void produce() {
        for (uint64_t g = 0;; g++) {
            // wait until the slot is free: the consumer must be within NBLK-1 blocks
            while (g >= consumed_.load(std::memory_order_acquire) + NBLK) {
                if (stop_.load(std::memory_order_acquire)) return;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (stop_.load(std::memory_order_acquire)) return;
            fill(size_t(g % NBLK), g);
        }
    }
    void advance() {
        gen_++;
        consumed_.store(gen_, std::memory_order_release);
        size_t slot = size_t(gen_ % NBLK);
        if (!async_) fill(slot, gen_);
        else
            while (ready_[slot].load(std::memory_order_acquire) != gen_ + 1) std::this_thread::yield();
        cur_ = ring_.get() + slot * BLOCK;
        end_ = cur_ + bsize(gen_);
    }
};

}  // namespace t3
