// t3sim core: an exact, allocation-light re-implementation of the Track-3 ABIDES baseline
// (pinned f9cbe51 + 4 patches + abides_fork adapter) specialised to what the scenario agents do.
//
// Semantics are those of spec/spec_sim.py (verified exact against every public reference trace and
// ledger). Each block cites the ABIDES behaviour it reproduces. Nothing here depends on wall-clock
// time, hash ordering or threads, so a scenario+seed always yields the same bytes.
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>
#ifdef T3_PROF
#include <x86intrin.h>
#endif

#include "json.hpp"
#include "latstream.hpp"
#include "rng.hpp"

namespace t3 {

#ifdef T3_PROF
struct ProfCounters { uint64_t c[8] = {0}; uint64_t n[8] = {0}; };
inline ProfCounters g_prof;
#define T3P_BEGIN uint64_t _t3p0 = __rdtsc();
#define T3P_MARK(i) { uint64_t _t3p1 = __rdtsc(); g_prof.c[i] += _t3p1 - _t3p0; g_prof.n[i]++; _t3p0 = _t3p1; }
struct T3PScope { int i; uint64_t t0; explicit T3PScope(int k) : i(k), t0(__rdtsc()) {} ~T3PScope() { g_prof.c[i] += __rdtsc() - t0; g_prof.n[i]++; } };
#define T3P_SCOPE(i) T3PScope _t3ps(i);
#else
#define T3P_BEGIN
#define T3P_MARK(i)
#define T3P_SCOPE(i)
#endif

constexpr int64_t DATE_NS = 1612483200000000000LL;          // pd.to_datetime("20210205").value
constexpr int64_t OPEN_OFFSET_NS = 34200LL * 1000000000LL;  // 09:30:00
constexpr int64_t ORACLE_CLOSE_NS = DATE_NS + 57600LL * 1000000000LL;  // 16:00:00
constexpr int64_t DEFAULT_COMPUTATION_DELAY = 50;
constexpr int64_t NULL_I64 = INT64_MIN;

// ---- message kinds; names are the ABIDES class names written verbatim to the ledger ----------
enum Kind : uint8_t {
    K_WAKEUP = 0, K_MKT_CLOSE_PRICE_REQ, K_MKT_HOURS_REQ, K_MKT_HOURS, K_MKT_CLOSE_PRICE,
    K_MKT_CLOSED, K_QUERY_SPREAD, K_QUERY_SPREAD_RESP, K_LIMIT_ORDER, K_CANCEL_ORDER,
    K_ORDER_ACCEPTED, K_ORDER_EXECUTED, K_ORDER_CANCELLED, K_NKINDS
};
inline const char* const* kind_names() {
    static const char* const n[K_NKINDS] = {
        "AGENT_WAKEUP", "MarketClosePriceRequestMsg", "MarketHoursRequestMsg", "MarketHoursMsg",
        "MarketClosePriceMsg", "MarketClosedMsg", "QuerySpreadMsg", "QuerySpreadResponseMsg",
        "LimitOrderMsg", "CancelOrderMsg", "OrderAcceptedMsg", "OrderExecutedMsg", "OrderCancelledMsg"};
    return n;
}

// ---- trace msg_type codes ---------------------------------------------------------------------
enum TType : uint8_t { T_SUBMITTED = 0, T_ACCEPTED, T_FILLED, T_PARTIAL, T_CANCELLED, T_REPLACED, T_QUOTE, T_NTYPES };
inline const char* const* ttype_names() {
    static const char* const n[T_NTYPES] = {"ORDER_SUBMITTED", "ORDER_ACCEPTED", "ORDER_FILLED", "PARTIAL_FILL",
                                            "ORDER_CANCELLED", "ORDER_REPLACED", "QUOTE_UPDATE"};
    return n;
}

// ---- Python numeric helpers -------------------------------------------------------------------
inline int64_t py_round(double x) { return int64_t(std::nearbyint(x)); }  // round-half-even
inline int64_t py_floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
// exact comparison of a double with an int64, as Python does: returns -1/0/+1 (x vs n)
inline int cmp_fi(double x, int64_t n) {
    if (x >= 9223372036854775808.0) return 1;
    if (x < -9223372036854775808.0) return -1;
    double fl = std::floor(x);
    int64_t xi = int64_t(fl);
    if (xi < n) return -1;
    if (xi > n) return 1;
    return x > fl ? 1 : 0;
}
// A Python "time" that may be an int or a float (the oracle mixes both after a megashock).
struct PyNum {
    bool is_f = false;
    int64_t i = 0;
    double f = 0.0;
    static PyNum I(int64_t v) { PyNum p; p.i = v; return p; }
    static PyNum F(double v) { PyNum p; p.is_f = true; p.f = v; return p; }
    double d() const { return is_f ? f : double(i); }
};
inline double py_sub(const PyNum& a, const PyNum& b) {
    if (!a.is_f && !b.is_f) return double(a.i - b.i);
    return a.d() - b.d();
}
inline int py_cmp(const PyNum& a, int64_t n) { return a.is_f ? cmp_fi(a.f, n) : (a.i < n ? -1 : (a.i > n ? 1 : 0)); }

// ---- scenario configuration (all Python conversions applied once) ------------------------------
enum AType : uint8_t { A_NOISE = 1, A_MM, A_VALUE, A_MOMENTUM };

struct AgentCfg {
    AType type;
    int64_t count;
    int64_t interval;
    double size_mean = 0, size_std = 0;
    int64_t offset = 0, spread = 0, depth = 0, lvl_size = 0, thr = 0, lookback = 0;
};

enum LatModel : uint8_t { L_DET, L_LOGNORMAL, L_UNIFORM, L_PARETO };

struct Scenario {
    std::string scenario_id;
    int64_t seed = 0;
    int64_t horizon = 0;
    bool proto = false;
    int stp = 0;  // 0 none, 1 cancel_newest (any non-"cancel_oldest" policy), 2 cancel_oldest
    int64_t ack_delay = 0, ex_compute_delay = 0;
    int64_t ref_price = 100000;
    double kappa = 1.67e-16, fund_vol = 5e-5, lam = 2.77778e-18, ms_mean = 1000.0, ms_var = 50000;
    bool has_jump = false;
    int64_t jump_time = 0, jump_mag = 0;
    bool has_latency = false;
    LatModel lat = L_DET;
    double lat_mean = 0, lat_sigma = 0, lat_lo = 0, lat_hi = 1e12, lat_alpha = 1.5, lat_mu = 0;
    std::vector<AgentCfg> agents;
};

inline Scenario load_scenario(const JVal& j, const int64_t* seed_override = nullptr) {
    Scenario s;
    static const JVal kEmpty = [] { JVal v; v.kind = JVal::OBJ; return v; }();
    auto obj = [&](const JVal* p) -> const JVal& { return p ? *p : kEmpty; };
    auto num = [&](const JVal& o, const char* k, double dflt) { const JVal* v = o.get(k); return v ? v->as_float() : dflt; };
    auto inum = [&](const JVal& o, const char* k, int64_t dflt) { const JVal* v = o.get(k); return v ? v->as_int() : dflt; };
    // Python max(lo, x) on a possibly-float value, then int()
    auto imax = [&](const JVal& o, const char* k, int64_t dflt, int64_t lo) -> int64_t {
        const JVal* v = o.get(k);
        if (!v) return std::max<int64_t>(lo, dflt);
        if (v->kind == JVal::FLT) return v->f > double(lo) ? int64_t(v->f) : lo;
        return std::max<int64_t>(lo, v->as_int());
    };

    const JVal* sid = j.get("scenario_id");
    s.scenario_id = sid && sid->kind == JVal::STR ? sid->s : "";
    s.seed = seed_override ? *seed_override : j.get("seed")->as_int();
    s.horizon = j.get("horizon_ns")->as_int();

    const JVal& ex = obj(j.get("exchange_config"));
    s.proto = ex.get("protocol_enforcement") ? ex.get("protocol_enforcement")->truthy() : false;
    if (s.proto && ex.get("stp_policy") && ex.get("stp_policy")->truthy()) {
        const JVal* sp = ex.get("stp_policy");
        s.stp = (sp->kind == JVal::STR && sp->s == "cancel_oldest") ? 2 : 1;
    }
    s.ack_delay = s.proto ? inum(ex, "ack_delay_ns", 0) : 0;
    s.ex_compute_delay = s.proto ? inum(ex, "compute_delay_ns", 0) : 0;

    const JVal& op = obj(obj(j.get("oracle_config")).get("params"));
    s.ref_price = inum(op, "initial_price", 100000);
    double kps = num(op, "kappa", 0.0);
    s.kappa = kps > 0 ? kps / 1e9 : 1.67e-16;
    s.fund_vol = num(op, "sigma", 5e-5);
    double jr = num(op, "jump_intensity", 0.0);
    s.lam = jr > 0 ? jr / 1e9 : 2.77778e-18;
    double js = num(op, "jump_sigma", 0.0);
    s.ms_mean = js != 0.0 ? js : 1000.0;
    s.ms_var = 50000;
    const JVal* sj = op.get("scheduled_jump");
    if (sj && sj->truthy()) {
        s.has_jump = true;
        s.jump_time = sj->get("time_ns")->as_int();
        s.jump_mag = sj->get("magnitude")->as_int();
    }

    const JVal* lc = j.get("latency_config");
    if (lc && lc->truthy()) {
        s.has_latency = true;
        const JVal& lp = obj(lc->get("params"));
        std::string m = lc->get("model") && lc->get("model")->kind == JVal::STR ? lc->get("model")->s : "deterministic";
        s.lat = m == "log_normal" ? L_LOGNORMAL : m == "uniform" ? L_UNIFORM : m == "pareto" ? L_PARETO : L_DET;
        s.lat_mean = num(lp, "mean_ns", 0.0);
        s.lat_sigma = num(lp, "sigma", 0.0);
        s.lat_lo = num(lp, "min_ns", 0.0);
        s.lat_hi = num(lp, "max_ns", 1e12);
        s.lat_alpha = num(lp, "alpha", 1.5);
        s.lat_mu = s.lat_mean > 0 ? std::log(s.lat_mean) : 0.0;
    }

    for (const JVal& ac : j.get("agent_configs")->arr) {
        AgentCfg c{};
        const std::string& t = ac.get("agent_type")->s;
        const JVal& p = obj(ac.get("params"));
        if (t == "NoiseTrader") c.type = A_NOISE;
        else if (t == "MarketMaker") c.type = A_MM;
        else if (t == "ValueTrader") c.type = A_VALUE;
        else if (t == "MomentumTrader") c.type = A_MOMENTUM;
        else throw std::runtime_error("unsupported agent_type: " + t);
        c.count = ac.get("count")->as_int();
        int64_t interval;
        if (p.has("rebalance_interval_ns")) interval = p.get("rebalance_interval_ns")->as_int();
        else {
            double hz = num(p, "arrival_rate_hz", 1.0);
            interval = hz > 0 ? int64_t(1e9 / hz) : int64_t(1e9);
        }
        c.interval = std::max<int64_t>(1, interval);
        switch (c.type) {
            case A_NOISE:
                c.size_mean = num(p, "order_size_mean", 10);
                c.size_std = num(p, "order_size_std", 2);
                c.offset = inum(p, "price_offset_ticks", 5);
                break;
            case A_MM:
                c.spread = imax(p, "spread_ticks", 2, 2);
                c.depth = imax(p, "depth_levels", 3, 1);
                c.lvl_size = imax(p, "size_per_level", 10, 1);
                break;
            case A_VALUE:
                c.size_mean = num(p, "order_size_mean", 25);
                c.thr = inum(p, "threshold_ticks", 2);
                break;
            case A_MOMENTUM:
                c.size_mean = num(p, "order_size_mean", 15);
                c.thr = inum(p, "threshold_ticks", 2);
                c.lookback = imax(p, "lookback", 5, 1);
                break;
        }
        s.agents.push_back(c);
    }
    return s;
}

// ---- streaming output sink (fixed-size columnar chunks) ----------------------------------------
constexpr size_t CHUNK = 4096;
struct TraceChunk {
    size_t n = 0;
    int64_t t[CHUNK], price[CHUNK], size[CHUNK], order_id[CHUNK];
    int32_t agent[CHUNK];
    uint8_t side[CHUNK];  // 0 BID, 1 ASK
    uint8_t type[CHUNK];  // msg_type code (appended to Sink::types by the encoder, in order)
};
struct LedgerChunk {
    size_t n = 0;
    int64_t seq0 = 0;
    int64_t t_recv[CHUNK], t_send[CHUNK], latency[CHUNK], msg_id[CHUNK], order_id[CHUNK], causal[CHUNK];  // NULL_I64 = null
    int32_t src[CHUNK], dst[CHUNK];
    uint8_t kind[CHUNK];
};
// The simulator fills `tc`/`lc`; when one is full it calls flush_*(), which must install a fresh
// chunk. msg_type codes for the whole trace stay in `types` (ORDER_FILLED is decided at the end).
struct Sink {
    TraceChunk* tc = nullptr;
    LedgerChunk* lc = nullptr;
    std::vector<uint8_t> types;  // complete only after the encoder has drained every chunk
    int64_t n_trace = 0, n_ledger = 0;
    virtual void flush_trace() = 0;
    virtual void flush_ledger() = 0;
    virtual ~Sink() = default;
};

// ---- simulator ---------------------------------------------------------------------------------
class Sim {
   public:
    Sim(const Scenario& sc, Sink& sink, bool async_latency = false) : sc_(sc), sink_(sink), async_lat_(async_latency) { setup(); }

    void run();
    int64_t n_orders() const { return next_order_id_; }
    // Row index of each order's final execution (-1 if none): those rows are ORDER_FILLED.
    const std::vector<int64_t>& last_exec_rows() const { return last_exec_row_; }

   private:
    // one in-flight delivery
    struct Slot {
        int64_t t_send, t_recv, causal;
        int64_t order_id, qty, price;  // order snapshot (price = limit, or fill price for executions)
        int64_t bid, ask;              // spread response (0 == None, Python-falsy)
        int32_t agent;
        uint8_t kind, is_bid, closed;
    };
    struct HE {  // heap entry, ordered like ABIDES' (time, (sender, recipient, message))
        int64_t t;
        uint64_t sr;  // sender << 32 | recipient
        int64_t mid;
        uint32_t slot;
    };
    static bool less(const HE& a, const HE& b) {
        if (a.t != b.t) return a.t < b.t;
        if (a.sr != b.sr) return a.sr < b.sr;
        return a.mid < b.mid;
    }
    struct Agent {
        AType type;
        int32_t id;
        int64_t interval;
        MT19937 rs;
        bool awaiting = false, has_hours = false, closed = false, first_wake = true;
        int64_t bid = 0, ask = 0;
        const AgentCfg* cfg;
        // MarketMaker open-order dict (insertion order == ascending order_id)
        struct Open { int64_t id, qty, price; uint8_t is_bid; };
        std::vector<Open> open;
        // MomentumTrader mid history (Python list semantics)
        std::vector<double> hist;
        size_t hist_head = 0;
    };
    // order book
    struct Node { int64_t order_id, qty; int32_t agent, next, prev; };
    struct Level { int64_t price, total; int32_t head, tail; };

    const Scenario& sc_;
    Sink& sink_;
    bool async_lat_;
    std::unique_ptr<LatencyStream> lat_;
    std::vector<double> line_x_;  // fallback line-distance latency model
    int64_t mkt_open_ = 0, mkt_close_ = 0, start_ = 0, stop_ = 0;

    // RNG
    MT19937 global_, oracle_rs_;

    // kernel
    std::vector<HE> heap_, wheap_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_slots_;
    int64_t cur_ = 0;
    int64_t next_mid_ = 1;
    int64_t next_order_id_ = 0;
    int64_t causal_ = NULL_I64;
    std::vector<int64_t> busy_, cdelay_;

    std::vector<Agent> agents_;  // index 0 unused (exchange)
    std::vector<int32_t> close_subs_;

    // book: levels sorted with the BEST level at the back
    std::vector<Level> bids_, asks_;
    std::vector<Node> nodes_;
    std::vector<int32_t> free_nodes_;
    std::vector<int32_t> node_of_;  // order_id -> node index (-1 if not resting)

    // oracle
    PyNum or_t_;
    int64_t or_v_ = 0;
    double mst_ = 0, msv_ = 0;
    bool jump_consumed_ = false;
    double theta2_over_2g_ = 0;

    // trace grouping (rows at one timestamp are flushed in (t, order_id) order)
    struct OEv { int64_t order_id, price, size; int32_t agent; uint8_t type, side; };
    std::vector<OEv> g_orders_;
    int64_t g_t_ = NULL_I64;
    int g_nq_ = 0;
    uint8_t g_qside_[2];
    int64_t g_qprice_[2], g_qsize_[2];
    bool g_qseen_[2];
    std::vector<int64_t> last_exec_row_;

    void setup();
    // kernel
    inline uint32_t alloc_slot() {
        if (!free_slots_.empty()) { uint32_t s = free_slots_.back(); free_slots_.pop_back(); return s; }
        slots_.emplace_back();
        return uint32_t(slots_.size() - 1);
    }
    // Two 4-ary min-heaps — wakeups and in-flight messages — merged by comparing tops. The pair
    // yields exactly the single-queue order (a total order on (time, sender, recipient, id)), but
    // most pops (messages) hit a small, shallow heap.
    static inline void heap_push(std::vector<HE>& h, const HE& e) {
        h.push_back(e);
        size_t i = h.size() - 1;
        while (i > 0) {
            size_t p = (i - 1) >> 2;
            if (!less(e, h[p])) break;
            h[i] = h[p];
            i = p;
        }
        h[i] = e;
    }
    static inline HE heap_pop(std::vector<HE>& h) {
        HE top = h[0];
        HE last = h.back();
        h.pop_back();
        size_t n = h.size();
        if (n) {
            size_t i = 0;
            for (;;) {
                size_t c = 4 * i + 1;
                if (c >= n) break;
                size_t best = c;
                size_t e = std::min(c + 4, n);
                for (size_t k = c + 1; k < e; k++)
                    if (less(h[k], h[best])) best = k;
                if (!less(h[best], last)) break;
                h[i] = h[best];
                i = best;
            }
            h[i] = last;
        }
        return top;
    }
    inline void push(const HE& e, bool wake) { heap_push(wake ? wheap_ : heap_, e); }
    inline bool queue_empty() const { return heap_.empty() && wheap_.empty(); }
    inline HE pop(bool& wake) {
        wake = !wheap_.empty() && (heap_.empty() || less(wheap_[0], heap_[0]));
        return heap_pop(wake ? wheap_ : heap_);
    }
    inline int64_t latency(int32_t s, int32_t r);
    inline void set_wakeup(int32_t aid, int64_t t);
    inline Slot& send(int32_t s, int32_t r, uint8_t kind, int64_t mid, int64_t delay = 0);
    inline Slot& send_new(int32_t s, int32_t r, uint8_t kind, int64_t delay = 0) { return send(s, r, kind, next_mid_++, delay); }
    inline Slot& ex_send(int32_t r, uint8_t kind) {
        int64_t d = (kind == K_ORDER_ACCEPTED || kind == K_ORDER_CANCELLED || kind == K_ORDER_EXECUTED) ? sc_.ack_delay : 0;
        return send_new(0, r, kind, d);
    }
    // exchange
    void ex_wakeup(int64_t t);
    void ex_receive(int64_t t, int32_t s, const Slot& m);
    void handle_limit(int32_t agent, int64_t oid, bool is_bid, int64_t qty, int64_t price);
    bool cancel(int32_t agent, int64_t oid, bool is_bid, int64_t price);
    void enter(int32_t agent, int64_t oid, bool is_bid, int64_t qty, int64_t price);
    void remove_node(std::vector<Level>& book, size_t li, int32_t ni);
    size_t find_level(const std::vector<Level>& book, bool is_bid, int64_t price, bool& found) const;
    void log_quotes();
    // agents
    void ag_wakeup(Agent& a, int64_t t);
    void ag_receive(Agent& a, int64_t t, const Slot& m);
    void act(Agent& a, int64_t t);
    void place(Agent& a, int64_t t, int64_t qty, bool is_bid, int64_t price);
    // oracle
    int64_t oracle_compute(PyNum ts, double v_adj, PyNum pt, int64_t pv);
    int64_t oracle_advance(int64_t t);
    int64_t oracle_observe(int64_t t, MT19937& rs);
    // ledger
    inline void led_row(int64_t t_recv, int64_t t_send, int32_t s, int32_t r, int64_t mid, uint8_t kind,
                        int64_t oid, int64_t causal) {
        LedgerChunk& c = *sink_.lc;
        size_t k = c.n;
        c.t_recv[k] = t_recv;
        c.t_send[k] = t_send;
        c.latency[k] = t_send == NULL_I64 ? 0 : t_recv - t_send;
        c.src[k] = s;
        c.dst[k] = r;
        c.msg_id[k] = mid;
        c.kind[k] = kind;
        c.order_id[k] = oid;
        c.causal[k] = causal;
        c.n = k + 1;
        sink_.n_ledger++;
        if (c.n == CHUNK) sink_.flush_ledger();
    }
    inline void tr_row(int64_t t, int32_t agent, uint8_t type, uint8_t side, int64_t price, int64_t size, int64_t oid) {
        TraceChunk& c = *sink_.tc;
        size_t k = c.n;
        c.t[k] = t;
        c.agent[k] = agent;
        c.side[k] = side;
        c.price[k] = price;
        c.size[k] = size;
        c.order_id[k] = oid;
        c.type[k] = type;
        c.n = k + 1;
        sink_.n_trace++;
        if (c.n == CHUNK) sink_.flush_trace();
    }
    // trace
    inline void emit_order(int64_t t, int64_t oid, int32_t agent, uint8_t type, bool is_bid, int64_t price, int64_t size) {
        if (t != g_t_) { flush_group(); g_t_ = t; }
        g_orders_.push_back(OEv{oid, price, size, agent, type, uint8_t(is_bid ? 0 : 1)});
    }
    inline void emit_quote(int64_t t, bool is_bid, int64_t price, int64_t size) {
        if (t != g_t_) { flush_group(); g_t_ = t; }
        int sd = is_bid ? 0 : 1;
        if (!g_qseen_[sd]) { g_qseen_[sd] = true; g_qside_[g_nq_++] = uint8_t(sd); }
        g_qprice_[sd] = price;
        g_qsize_[sd] = size;
    }
    void flush_group();
    void finish_trace();
};

// ============================== implementation ==============================

inline void Sim::setup() {
    mkt_open_ = DATE_NS + OPEN_OFFSET_NS;
    mkt_close_ = mkt_open_ + sc_.horizon;
    start_ = DATE_NS;
    stop_ = mkt_close_ + 1000000000LL;

    // config.build_config: global seed, then RandomStates drawn in a fixed order
    global_.seed(uint32_t(sc_.seed));
    oracle_rs_.seed(global_.seed_draw());
    // SparseMeanRevertingOracle.__init__
    or_t_ = PyNum::I(mkt_open_);
    or_v_ = sc_.ref_price;
    double ms_dt = global_.exponential(1.0 / sc_.lam);
    mst_ = double(mkt_open_) + ms_dt;
    double msv = oracle_rs_.normal(sc_.ms_mean, std::sqrt(sc_.ms_var));
    msv_ = oracle_rs_.randint(0, 2) == 0 ? msv : -msv;
    // CPython evaluates theta**2 with libm pow(); call it through a volatile pointer so the
    // compiler cannot fold it into theta*theta.
    static double (*volatile libm_pow)(double, double) = std::pow;
    theta2_over_2g_ = libm_pow(sc_.fund_vol, 2.0) / (2 * sc_.kappa);
    (void)global_.seed_draw();  // ExchangeAgent random_state (never used)

    agents_.resize(1);
    int32_t next_id = 1;
    size_t n_agents = 1;
    for (auto& c : sc_.agents) n_agents += size_t(std::max<int64_t>(0, c.count));
    agents_.reserve(n_agents);
    for (auto& c : sc_.agents) {
        for (int64_t k = 0; k < c.count; k++) {
            agents_.emplace_back();
            Agent& a = agents_.back();
            a.type = c.type;
            a.id = next_id++;
            a.interval = c.interval;
            a.cfg = &c;
            a.rs.seed(global_.seed_draw());
            if (c.type == A_MOMENTUM) a.hist.reserve(size_t(c.lookback) + 2);
        }
    }
    if (!sc_.has_latency) {
        // utils.generate_latency_model(n, "deterministic"): agents uniform on a NYC-Seattle line,
        // latency = int(pairwise distance / light speed in m/ns); no per-message randomness.
        MT19937 lrs(global_.seed_draw());
        line_x_.resize(agents_.size());
        for (auto& x : line_x_) x = lrs.uniform(0.0, 3866660.0);
    } else {
        LatParams lp{uint8_t(sc_.lat == L_LOGNORMAL ? 1 : sc_.lat == L_UNIFORM ? 2 : sc_.lat == L_PARETO ? 3 : 0),
                     sc_.lat_mean, sc_.lat_sigma, sc_.lat_lo, sc_.lat_hi, sc_.lat_alpha, sc_.lat_mu};
        lat_.reset(new LatencyStream(global_.seed_draw(), lp, async_lat_));
    }
    (void)global_.seed_draw();  // kernel random_state (never used)

    size_t n = agents_.size();
    busy_.assign(n, start_);
    cdelay_.assign(n, DEFAULT_COMPUTATION_DELAY);
    heap_.reserve(4 * n + 64);
    wheap_.reserve(n + 8);
    slots_.reserve(4 * n + 64);
    g_orders_.reserve(256);
    g_qseen_[0] = g_qseen_[1] = false;
}

inline int64_t Sim::latency(int32_t s, int32_t r) {
    if (s == r) return 0;  // ScenarioLatencyModel: no draw for self-messages
    if (!lat_) return int64_t(std::fabs(line_x_[size_t(s)] - line_x_[size_t(r)]) / 299792458e-9);
    return lat_->next();
}

inline void Sim::set_wakeup(int32_t aid, int64_t t) {
    if (cur_ && t < cur_) throw std::runtime_error("set_wakeup() called with requested time not in future");
    uint32_t si = alloc_slot();
    Slot& sl = slots_[si];
    sl.kind = K_WAKEUP;
    push(HE{t, (uint64_t(uint32_t(aid)) << 32) | uint32_t(aid), next_mid_++, si}, true);
}

inline Sim::Slot& Sim::send(int32_t s, int32_t r, uint8_t kind, int64_t mid, int64_t delay) {
    T3P_SCOPE(5)
    int64_t sent = cur_ + cdelay_[s] + delay;
    int64_t at = sent + latency(s, r);
    uint32_t si = alloc_slot();
    Slot& sl = slots_[si];
    sl.kind = kind;
    sl.t_send = sent;
    sl.t_recv = at;
    sl.causal = causal_;
    sl.order_id = NULL_I64;
    push(HE{at, (uint64_t(uint32_t(s)) << 32) | uint32_t(r), mid, si}, false);
    return sl;
}

inline void Sim::run() {
    // Kernel.initialize: exchange.kernel_initializing -> wakeup at mkt_close; then every
    // agent's kernel_starting -> wakeup at start_time (agent id order).
    set_wakeup(0, mkt_close_);
    for (size_t a = 0; a < agents_.size(); a++) set_wakeup(int32_t(a), start_);
    cur_ = start_;
    while (!queue_empty() && cur_ <= stop_) {
        T3P_BEGIN
        bool wake;
        HE e = pop(wake);
        cur_ = e.t;
        int32_t r = int32_t(e.sr & 0xffffffffu);
        if (busy_[r] > cur_) {  // recipient still "thinking": requeue at its free time
            e.t = busy_[r];
            push(e, wake);
            continue;
        }
        busy_[r] = cur_;
        Slot m = slots_[e.slot];
        free_slots_.push_back(e.slot);
        T3P_MARK(0)
        if (m.kind == K_WAKEUP) {
            causal_ = e.mid;
            led_row(cur_, NULL_I64, r, r, e.mid, K_WAKEUP, NULL_I64, NULL_I64);
            T3P_MARK(1)
            if (r == 0) ex_wakeup(cur_);
            else ag_wakeup(agents_[r], cur_);
            busy_[r] += cdelay_[r];
            T3P_MARK(2)
        } else {
            busy_[r] += cdelay_[r];
            causal_ = e.mid;
            int32_t s = int32_t(e.sr >> 32);
            led_row(m.t_recv, m.t_send, s, r, e.mid, m.kind, m.order_id, m.causal);
            T3P_MARK(1)
            if (r == 0) { ex_receive(cur_, s, m); T3P_MARK(3) }
            else { ag_receive(agents_[r], cur_, m); T3P_MARK(4) }
        }
    }
    finish_trace();
#ifdef T3_PROF
    const char* names[] = {"pop+slot", "ledger", "wakeup", "exchange", "agent_recv", "[send]", "[flush_grp]", "[act]"};
    for (int i = 0; i < 8; i++)
        std::fprintf(stderr, "  %-11s n=%9lu  cyc/call=%7.1f  total_Mcyc=%8.1f\n", names[i], (unsigned long)g_prof.n[i],
                     g_prof.n[i] ? double(g_prof.c[i]) / double(g_prof.n[i]) : 0.0, double(g_prof.c[i]) / 1e6);
#endif
}

// ------------------------------ exchange ------------------------------
inline void Sim::ex_wakeup(int64_t t) {
    if (t >= mkt_close_) {
        int64_t mid = next_mid_++;  // ONE MarketClosePriceMsg object sent to every subscriber
        for (int32_t a : close_subs_) send(0, a, K_MKT_CLOSE_PRICE, mid);
    }
}

inline void Sim::ex_receive(int64_t t, int32_t s, const Slot& m) {
    cdelay_[0] = sc_.ex_compute_delay;
    uint8_t k = m.kind;
    if (t > mkt_close_) {
        if (k != K_QUERY_SPREAD) {  // OrderMsg -> MarketClosedMsg; other non-query -> MarketClosedMsg
            ex_send(s, K_MKT_CLOSED);
            return;
        }
    }
    switch (k) {
        case K_MKT_HOURS_REQ:
            cdelay_[0] = 0;
            ex_send(s, K_MKT_HOURS);
            break;
        case K_MKT_CLOSE_PRICE_REQ:
            close_subs_.push_back(s);
            break;
        case K_QUERY_SPREAD: {
            int64_t bid = bids_.empty() ? 0 : bids_.back().price;
            int64_t ask = asks_.empty() ? 0 : asks_.back().price;
            Slot& o = ex_send(s, K_QUERY_SPREAD_RESP);
            o.bid = bid;
            o.ask = ask;
            o.closed = t > mkt_close_;
            break;
        }
        case K_LIMIT_ORDER:
            handle_limit(m.agent, m.order_id, m.is_bid, m.qty, m.price);
            break;
        case K_CANCEL_ORDER:
            cancel(m.agent, m.order_id, m.is_bid, m.price);
            break;
        default:
            break;
    }
}

inline size_t Sim::find_level(const std::vector<Level>& book, bool is_bid, int64_t price, bool& found) const {
    // bids ascending (best = highest at back); asks descending (best = lowest at back)
    size_t lo = 0, hi = book.size();
    if (is_bid) {
        while (lo < hi) { size_t m = (lo + hi) >> 1; if (book[m].price < price) lo = m + 1; else hi = m; }
    } else {
        while (lo < hi) { size_t m = (lo + hi) >> 1; if (book[m].price > price) lo = m + 1; else hi = m; }
    }
    found = lo < book.size() && book[lo].price == price;
    return lo;
}

inline void Sim::remove_node(std::vector<Level>& book, size_t li, int32_t ni) {
    Level& L = book[li];
    Node& n = nodes_[ni];
    if (n.prev >= 0) nodes_[n.prev].next = n.next; else L.head = n.next;
    if (n.next >= 0) nodes_[n.next].prev = n.prev; else L.tail = n.prev;
    L.total -= n.qty;
    node_of_[size_t(n.order_id)] = -1;
    free_nodes_.push_back(ni);
    if (L.head < 0) book.erase(book.begin() + ptrdiff_t(li));
}

inline void Sim::enter(int32_t agent, int64_t oid, bool is_bid, int64_t qty, int64_t price) {
    std::vector<Level>& book = is_bid ? bids_ : asks_;
    int32_t ni;
    if (!free_nodes_.empty()) { ni = free_nodes_.back(); free_nodes_.pop_back(); }
    else { nodes_.emplace_back(); ni = int32_t(nodes_.size() - 1); }
    Node& n = nodes_[ni];
    n.order_id = oid; n.qty = qty; n.agent = agent; n.next = -1;
    if (size_t(oid) >= node_of_.size()) node_of_.resize(std::max<size_t>(size_t(oid) + 1, node_of_.size() * 2), -1);
    node_of_[size_t(oid)] = ni;
    bool found;
    size_t li = find_level(book, is_bid, price, found);
    if (found) {
        Level& L = book[li];
        n.prev = L.tail;
        nodes_[L.tail].next = ni;
        L.tail = ni;
        L.total += qty;
    } else {
        n.prev = -1;
        book.insert(book.begin() + ptrdiff_t(li), Level{price, qty, ni, ni});
    }
}

inline bool Sim::cancel(int32_t agent, int64_t oid, bool is_bid, int64_t price) {
    std::vector<Level>& book = is_bid ? bids_ : asks_;
    if (book.empty()) return false;
    bool found;
    size_t li = find_level(book, is_bid, price, found);
    if (!found) return false;
    if (oid < 0 || size_t(oid) >= node_of_.size()) return false;
    int32_t ni = node_of_[size_t(oid)];
    if (ni < 0) return false;
    int64_t qty = nodes_[ni].qty;
    int32_t owner = nodes_[ni].agent;
    remove_node(book, li, ni);
    // OrderCancelledMsg(cancelled_order) goes to the *requesting* order's agent
    Slot& o = ex_send(agent, K_ORDER_CANCELLED);
    o.order_id = oid; o.agent = owner; o.is_bid = is_bid; o.qty = qty; o.price = price;
    return true;
}

inline void Sim::handle_limit(int32_t agent, int64_t oid, bool is_bid, int64_t qty, int64_t price) {
    if (qty <= 0 || price < 0) return;  // discarded with a warning; no quotes logged
    std::vector<Level>& opp = is_bid ? asks_ : bids_;
    for (;;) {
        bool match = !opp.empty() && (is_bid ? price >= opp.back().price : price <= opp.back().price);
        if (sc_.stp && match) {
            Level& B = opp.back();
            Node& head = nodes_[B.head];
            if (head.agent == agent) {
                if (sc_.stp == 2 && cancel(head.agent, head.order_id, !is_bid, B.price)) continue;
                if (sc_.stp != 2) {
                    Slot& o = ex_send(agent, K_ORDER_CANCELLED);
                    o.order_id = oid; o.agent = agent; o.is_bid = is_bid; o.qty = qty; o.price = price;
                    break;
                }
            }
        }
        if (!match) {
            enter(agent, oid, is_bid, qty, price);
            Slot& o = ex_send(agent, K_ORDER_ACCEPTED);
            o.order_id = oid; o.agent = agent; o.is_bid = is_bid; o.qty = qty; o.price = price;
            break;
        }
        // execute_order against the head of the best opposite level
        size_t li = opp.size() - 1;
        Level& B = opp[li];
        int32_t hi = B.head;
        Node& head = nodes_[hi];
        int64_t fill_price = B.price;
        int64_t mq;
        int32_t m_agent = head.agent;
        int64_t m_oid = head.order_id;
        if (qty >= head.qty) {
            mq = head.qty;
            remove_node(opp, li, hi);
        } else {
            mq = qty;
            head.qty -= mq;
            B.total -= mq;
        }
        qty -= mq;
        Slot& a = ex_send(m_agent, K_ORDER_EXECUTED);
        a.order_id = m_oid; a.agent = m_agent; a.is_bid = !is_bid; a.qty = mq; a.price = fill_price;
        Slot& b = ex_send(agent, K_ORDER_EXECUTED);
        b.order_id = oid; b.agent = agent; b.is_bid = is_bid; b.qty = mq; b.price = fill_price;
        if (qty <= 0) break;
    }
    log_quotes();
}

inline void Sim::log_quotes() {
    if (!bids_.empty()) emit_quote(cur_, true, bids_.back().price, bids_.back().total);
    if (!asks_.empty()) emit_quote(cur_, false, asks_.back().price, asks_.back().total);
}

// ------------------------------ agents ------------------------------
inline void Sim::ag_wakeup(Agent& a, int64_t t) {
    if (a.first_wake) {
        a.first_wake = false;
        send_new(a.id, 0, K_MKT_CLOSE_PRICE_REQ);
    }
    if (!a.has_hours) send_new(a.id, 0, K_MKT_HOURS_REQ);
    if (!a.has_hours || a.closed) return;  // mkt_open/mkt_close are never 0 once known
    set_wakeup(a.id, t + a.interval);
    send_new(a.id, 0, K_QUERY_SPREAD);
    a.awaiting = true;
}

inline void Sim::ag_receive(Agent& a, int64_t t, const Slot& m) {
    bool had = a.has_hours;
    switch (m.kind) {
        case K_MKT_HOURS: a.has_hours = true; break;
        case K_MKT_CLOSED: a.closed = true; break;
        case K_ORDER_EXECUTED:
            emit_order(t, m.order_id, a.id, T_PARTIAL, m.is_bid, m.price, m.qty);
            if (a.type == A_MM) {
                for (size_t i = 0; i < a.open.size(); i++)
                    if (a.open[i].id == m.order_id) {
                        if (m.qty >= a.open[i].qty) a.open.erase(a.open.begin() + ptrdiff_t(i));
                        else a.open[i].qty -= m.qty;
                        break;
                    }
            }
            break;
        case K_ORDER_ACCEPTED:
            emit_order(t, m.order_id, a.id, T_ACCEPTED, m.is_bid, m.price, m.qty);
            break;
        case K_ORDER_CANCELLED:
            emit_order(t, m.order_id, a.id, T_CANCELLED, m.is_bid, m.price, m.qty);
            if (a.type == A_MM) {
                for (size_t i = 0; i < a.open.size(); i++)
                    if (a.open[i].id == m.order_id) { a.open.erase(a.open.begin() + ptrdiff_t(i)); break; }
            }
            break;
        case K_QUERY_SPREAD_RESP:
            if (m.closed) a.closed = true;
            a.bid = m.bid;
            a.ask = m.ask;
            break;
        default:
            break;
    }
    if (!had && a.has_hours) set_wakeup(a.id, mkt_open_ + 0);
    if (a.awaiting && m.kind == K_QUERY_SPREAD_RESP) {
        if (!a.closed) act(a, t);
        a.awaiting = false;
    }
}

inline void Sim::place(Agent& a, int64_t t, int64_t qty, bool is_bid, int64_t price) {
    int64_t oid = next_order_id_++;
    if (a.type == A_MM) a.open.push_back(Agent::Open{oid, qty, price, uint8_t(is_bid)});
    Slot& o = send_new(a.id, 0, K_LIMIT_ORDER);
    o.order_id = oid; o.agent = a.id; o.is_bid = is_bid; o.qty = qty; o.price = price;
    emit_order(t, oid, a.id, T_SUBMITTED, is_bid, price, qty);
}

inline void Sim::act(Agent& a, int64_t t) {
    T3P_SCOPE(7)
    const AgentCfg& c = *a.cfg;
    int64_t bid = a.bid, ask = a.ask;
    switch (a.type) {
        case A_NOISE: {
            int64_t size = std::max<int64_t>(1, py_round(a.rs.normal(c.size_mean, c.size_std)));
            bool buy = a.rs.randint(0, 2) != 0;
            int64_t off = a.rs.randint(0, c.offset + 1);
            if (buy) {
                int64_t anchor = ask ? ask : (bid ? bid : sc_.ref_price);
                place(a, t, size, true, anchor + off);
            } else {
                int64_t anchor = bid ? bid : (ask ? ask : sc_.ref_price);
                place(a, t, size, false, anchor - off);
            }
            break;
        }
        case A_MM: {
            int64_t mid = (bid && ask) ? py_floordiv(bid + ask, 2) : sc_.ref_price;
            for (const auto& o : a.open) {  // cancel_all_orders: one CancelOrderMsg per open order
                Slot& s = send_new(a.id, 0, K_CANCEL_ORDER);
                s.order_id = o.id; s.agent = a.id; s.is_bid = o.is_bid; s.qty = o.qty; s.price = o.price;
            }
            int64_t half = c.spread / 2;
            for (int64_t lvl = 0; lvl < c.depth; lvl++) {
                place(a, t, c.lvl_size, true, mid - half - lvl);
                place(a, t, c.lvl_size, false, mid + half + lvl);
            }
            break;
        }
        case A_VALUE: {
            double mid;
            if (bid && ask) mid = double(bid + ask) / 2.0;
            else if (bid) mid = double(bid);
            else if (ask) mid = double(ask);
            else return;
            int64_t fundamental = oracle_observe(t, a.rs);
            int64_t size = std::max<int64_t>(1, py_round(c.size_mean));
            if (mid < double(fundamental - c.thr) && ask) place(a, t, size, true, ask);
            else if (mid > double(fundamental + c.thr) && bid) place(a, t, size, false, bid);
            break;
        }
        case A_MOMENTUM: {
            double mid;
            if (bid && ask) mid = double(bid + ask) / 2.0;
            else if (bid) mid = double(bid);
            else if (ask) mid = double(ask);
            else return;
            // list.append / pop(0) semantics with a sliding head index
            a.hist.push_back(mid);
            if (a.hist.size() - a.hist_head > size_t(c.lookback) + 1) a.hist_head++;
            if (a.hist_head > 4096) { a.hist.erase(a.hist.begin(), a.hist.begin() + ptrdiff_t(a.hist_head)); a.hist_head = 0; }
            if (a.hist.size() - a.hist_head <= size_t(c.lookback)) return;
            double past = a.hist[a.hist_head];
            int64_t size = std::max<int64_t>(1, py_round(c.size_mean));
            if (mid > past + double(c.thr) && ask) place(a, t, size, true, ask);
            else if (mid < past - double(c.thr) && bid) place(a, t, size, false, bid);
            break;
        }
    }
}

// ------------------------------ oracle ------------------------------
inline int64_t Sim::oracle_compute(PyNum ts, double v_adj, PyNum pt, int64_t pv) {
    double d = py_sub(ts, pt);
    const int64_t mu = sc_.ref_price;
    const double gamma = sc_.kappa;
    double loc = double(mu) + double(pv - mu) * std::exp(-gamma * d);
    double scale = std::sqrt(theta2_over_2g_ * (1 - std::exp(-2 * gamma * d)));
    double v = oracle_rs_.normal(loc, scale);
    v += v_adj;
    int64_t vi = v > 0 ? py_round(v) : 0;
    if (sc_.has_jump && !jump_consumed_ && py_cmp(ts, mkt_open_ + sc_.jump_time) >= 0) {
        vi = std::max<int64_t>(0, vi + sc_.jump_mag);
        jump_consumed_ = true;
    }
    or_t_ = ts;
    or_v_ = vi;
    return vi;
}

inline int64_t Sim::oracle_advance(int64_t t) {
    PyNum pt = or_t_;
    int64_t pv = or_v_;
    if (py_cmp(pt, t) >= 0) return pv;  // current_time <= pt
    while (cmp_fi(mst_, t) < 0) {
        int64_t v = oracle_compute(PyNum::F(mst_), msv_, pt, pv);
        pt = PyNum::F(mst_);
        pv = v;
        mst_ = mst_ + std::trunc(global_.exponential(1.0 / sc_.lam));
        double msv = oracle_rs_.normal(sc_.ms_mean, std::sqrt(sc_.ms_var));
        msv_ = oracle_rs_.randint(0, 2) == 0 ? msv : -msv;
    }
    return oracle_compute(PyNum::I(t), 0.0, pt, pv);
}

inline int64_t Sim::oracle_observe(int64_t t, MT19937& rs) {
    int64_t r = oracle_advance(t >= ORACLE_CLOSE_NS ? ORACLE_CLOSE_NS - 1 : t);
    return py_round(rs.normal(double(r), std::sqrt(1000.0)));
}

// ------------------------------ trace assembly ------------------------------
inline void Sim::flush_group() {
    T3P_SCOPE(6)
    if (g_t_ == NULL_I64) return;
    for (int q = 0; q < g_nq_; q++) {
        int sd = g_qside_[q];
        tr_row(g_t_, 0, T_QUOTE, uint8_t(sd), g_qprice_[sd], g_qsize_[sd], -1);
    }
    g_nq_ = 0;
    g_qseen_[0] = g_qseen_[1] = false;
    // stable sort by order_id (groups are tiny: insertion sort)
    auto& g = g_orders_;
    for (size_t i = 1; i < g.size(); i++) {
        OEv x = g[i];
        size_t j = i;
        while (j > 0 && g[j - 1].order_id > x.order_id) { g[j] = g[j - 1]; j--; }
        g[j] = x;
    }
    for (const OEv& e : g) {
        if (e.type == T_PARTIAL) {
            if (size_t(e.order_id) >= last_exec_row_.size())
                last_exec_row_.resize(std::max<size_t>(size_t(e.order_id) + 1, last_exec_row_.size() * 2), -1);
            last_exec_row_[size_t(e.order_id)] = sink_.n_trace;
        }
        tr_row(g_t_, e.agent, e.type, e.side, e.price, e.size, e.order_id);
    }
    g.clear();
}

inline void Sim::finish_trace() {
    flush_group();
    g_t_ = NULL_I64;
}

}  // namespace t3
