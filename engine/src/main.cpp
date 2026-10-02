// t3sim â€” Track-3 submission binary. Verbs (dispatched on argv[0] basename or a leading verb):
//   simulate       --config /input/scenario.json --out /output/trace.parquet [--seed N]
//   simulate-batch --batch-dir /input/scenarios  --out-dir /output
#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "output.hpp"
#include "sha256.hpp"
#include "sim.hpp"

namespace fs = std::filesystem;
using namespace t3;

static const char* kTracePandas =
    R"({"index_columns": [], "column_indexes": [], "columns": [{"name": "t_ns", "field_name": "t_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "agent_id", "field_name": "agent_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "msg_type", "field_name": "msg_type", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "side", "field_name": "side", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "price", "field_name": "price", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "size", "field_name": "size", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "order_id", "field_name": "order_id", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}], "creator": {"library": "pyarrow", "version": "15.0.2"}, "pandas_version": "1.5.3"})";
static const char* kLedgerPandas =
    R"({"index_columns": [], "column_indexes": [], "columns": [{"name": "seq", "field_name": "seq", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "t_recv_ns", "field_name": "t_recv_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "t_send_ns", "field_name": "t_send_ns", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}, {"name": "latency_ns", "field_name": "latency_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "src_id", "field_name": "src_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "dst_id", "field_name": "dst_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "message_id", "field_name": "message_id", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "msg_type", "field_name": "msg_type", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "order_id", "field_name": "order_id", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}, {"name": "causal_parent", "field_name": "causal_parent", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}], "creator": {"library": "pyarrow", "version": "15.0.2"}, "pandas_version": "1.5.3"})";

static double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string s;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    s.resize(size_t(n));
    if (n > 0 && std::fread(&s[0], 1, size_t(n), f) != size_t(n)) { std::fclose(f); throw std::runtime_error("short read " + path); }
    std::fclose(f);
    return s;
}

static void write_file(const fs::path& path, const void* data, size_t n) {
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path.string());
    if (n && std::fwrite(data, 1, n, f) != n) { std::fclose(f); throw std::runtime_error("short write " + path.string()); }
    std::fclose(f);
}

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

static std::string fmt_double(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.17g", v);
    std::string s(b);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

static int64_t peak_rss_bytes() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return int64_t(ru.ru_maxrss) * 1024;
}

// Streaming sink: full chunks are encoded into per-column Parquet pages by one worker per table
// (inline when `async` is false, e.g. inside a saturated batch pool).
class StreamSink : public Sink {
   public:
    explicit StreamSink(bool async) : tw_(async), lw_(async) {
        using out::ColSpec;
        auto spec = [](const char* n, out::PType t, bool opt = false) { ColSpec c; c.name = n; c.ptype = t; c.optional = opt; return c; };
        tcols_.resize(7);
        tcols_[0].spec = spec("t_ns", out::INT64);
        tcols_[1].spec = spec("agent_id", out::INT32);
        tcols_[2].spec = spec("msg_type", out::BYTE_ARRAY);
        for (int k = 0; k < T_NTYPES; k++) tcols_[2].spec.dict.push_back(ttype_names()[k]);
        tcols_[3].spec = spec("side", out::BYTE_ARRAY);
        tcols_[3].spec.dict = {"BID", "ASK"};
        tcols_[4].spec = spec("price", out::INT64);
        tcols_[5].spec = spec("size", out::INT64);
        tcols_[6].spec = spec("order_id", out::INT64);
        lcols_.resize(10);
        lcols_[0].spec = spec("seq", out::INT64);
        lcols_[1].spec = spec("t_recv_ns", out::INT64);
        lcols_[2].spec = spec("t_send_ns", out::INT64, true);
        lcols_[3].spec = spec("latency_ns", out::INT64);
        lcols_[4].spec = spec("src_id", out::INT32);
        lcols_[5].spec = spec("dst_id", out::INT32);
        lcols_[6].spec = spec("message_id", out::INT64);
        lcols_[7].spec = spec("msg_type", out::BYTE_ARRAY);
        for (int k = 0; k < K_NKINDS; k++) lcols_[7].spec.dict.push_back(kind_names()[k]);
        lcols_[8].spec = spec("order_id", out::INT64, true);
        lcols_[9].spec = spec("causal_parent", out::INT64, true);
        for (auto& c : tcols_) c.begin();
        for (auto& c : lcols_) c.begin();
        for (int i = 0; i < 2; i++) { tfree_.push_back(new TraceChunk); lfree_.push_back(new LedgerChunk); }
        tn_ = ln_ = 2;
        types.reserve(1 << 16);
        tc = take(tfree_, tn_);
        lc = take(lfree_, ln_);
        lc->seq0 = 0;
    }
    ~StreamSink() override {
        tw_.finish();
        lw_.finish();
        for (auto* p : tfree_) delete p;
        for (auto* p : lfree_) delete p;
        delete tc;
        delete lc;
    }
    void flush_trace() override {
        TraceChunk* c = tc;
        tw_.submit([this, c] {
            size_t n = c->n;
            tcols_[0].page_i64(c->t, n);
            tcols_[1].page_i32(c->agent, n);
            tcols_[3].page_codes(c->side, n);
            tcols_[4].page_i64(c->price, n);
            tcols_[5].page_i64(c->size, n);
            tcols_[6].page_i64(c->order_id, n);
            c->n = 0;
            give(tfree_, c);
        });
        tc = take(tfree_, tn_);
    }
    void flush_ledger() override {
        LedgerChunk* c = lc;
        int64_t next_seq = c->seq0 + int64_t(c->n);
        lw_.submit([this, c] {
            size_t n = c->n;
            for (size_t k = 0; k < n; k++) seqbuf_[k] = c->seq0 + int64_t(k);
            lcols_[0].page_i64(seqbuf_, n);
            lcols_[1].page_i64(c->t_recv, n);
            lcols_[2].page_i64(c->t_send, n);
            lcols_[3].page_i64(c->latency, n);
            lcols_[4].page_i32(c->src, n);
            lcols_[5].page_i32(c->dst, n);
            lcols_[6].page_i64(c->msg_id, n);
            lcols_[7].page_codes(c->kind, n);
            lcols_[8].page_i64(c->order_id, n);
            lcols_[9].page_i64(c->causal, n);
            c->n = 0;
            give(lfree_, c);
        });
        lc = take(lfree_, ln_);
        lc->seq0 = next_seq;
    }
    // Drain everything and build both file images.
    void finish(out::FileImage& trace_img, out::FileImage& ledger_img) {
        if (tc->n) flush_trace();
        if (lc->n) flush_ledger();
        tw_.finish();
        lw_.finish();
        size_t n = types.size();
        for (size_t i = 0; i < n; i += CHUNK) tcols_[2].page_codes(types.data() + i, std::min(CHUNK, n - i));
        out::assemble(trace_img, tcols_, int64_t(n), kTracePandas);
        out::assemble(ledger_img, lcols_, n_ledger, kLedgerPandas);
    }

   private:
    static constexpr int kMaxPool = 64;  // chunks are allocated lazily, only when encoding lags
    int tn_ = 0, ln_ = 0;
    out::Worker tw_, lw_;
    std::vector<out::ColumnChunk> tcols_, lcols_;
    std::mutex pm_;
    std::condition_variable pcv_;
    std::vector<TraceChunk*> tfree_;
    std::vector<LedgerChunk*> lfree_;
    int64_t seqbuf_[CHUNK];
    template <class T> T* take(std::vector<T*>& pool, int& allocated) {
        std::unique_lock<std::mutex> lk(pm_);
        if (pool.empty() && allocated < kMaxPool) { allocated++; return new T; }
        pcv_.wait(lk, [&] { return !pool.empty(); });
        T* p = pool.back();
        pool.pop_back();
        return p;
    }
    template <class T> void give(std::vector<T*>& pool, T* p) {
        std::unique_lock<std::mutex> lk(pm_);
        pool.push_back(p);
        pcv_.notify_all();
    }
};

struct RunResult {
    std::string scenario_id;
    int64_t seed = 0;
    size_t n_events = 0, n_messages = 0;
    double wall = 0;
    std::string sha;
};

// Run one scenario and write trace.parquet (at out_trace), message_trace.parquet and events.json
// next to it. wall_clock_sec covers everything from reading the config to the files on disk.
static RunResult run_one(const std::string& config_path, const fs::path& out_trace, const int64_t* seed_override,
                         bool async) {
    double t_start = now_sec();
    JVal j = parse_json(read_file(config_path));
    Scenario sc = load_scenario(j, seed_override);
    fs::path dir = out_trace.parent_path();
    if (!dir.empty()) fs::create_directories(dir);
    double t_parse = now_sec();
    auto sink = std::make_unique<StreamSink>(async);
    Sim sim(sc, *sink, async);
    sim.run();
    double t_sim = now_sec();
    out::FileImage timg, limg;
    sink->finish(timg, limg);
    double t_enc = now_sec();
    RunResult r;
    r.scenario_id = sc.scenario_id;
    r.seed = sc.seed;
    r.n_events = sink->types.size();
    r.n_messages = size_t(sink->n_ledger);
    r.sha = timg.sha256();
    double t_sha = now_sec();
    timg.write(out_trace.string());
    limg.write((dir / "message_trace.parquet").string());
    size_t tbytes = 0, lbytes = 0;
    for (auto& s : timg.segs) tbytes += s.second;
    for (auto& s : limg.segs) lbytes += s.second;
    r.wall = now_sec() - t_start;
    if (r.wall <= 0) r.wall = 1e-9;
    if (std::getenv("T3_PROFILE"))
        std::fprintf(stderr, "profile rows=%zu msgs=%zu parse=%.4f sim=%.4f encode=%.4f sha=%.4f write=%.4f bytes=%zu+%zu\n",
                     r.n_events, r.n_messages, t_parse - t_start, t_sim - t_parse, t_enc - t_sim, t_sha - t_enc,
                     now_sec() - t_sha, tbytes, lbytes);
    std::ostringstream ev;
    ev << "{\n"
       << "  \"scenario_id\": \"" << json_escape(r.scenario_id) << "\",\n"
       << "  \"seed\": " << r.seed << ",\n"
       << "  \"n_events\": " << r.n_events << ",\n"
       << "  \"wall_clock_sec\": " << fmt_double(r.wall) << ",\n"
       << "  \"events_per_sec\": " << fmt_double(double(r.n_events) / r.wall) << ",\n"
       << "  \"trace_sha256\": \"" << r.sha << "\",\n"
       << "  \"n_messages\": " << r.n_messages << ",\n"
       << "  \"peak_memory_bytes\": " << peak_rss_bytes() << ",\n"
       << "  \"gpu_seconds\": 0.0\n"
       << "}\n";
    std::string evs = ev.str();
    write_file(dir / "events.json", evs.data(), evs.size());
    return r;
}

static int cgroup_cpus() {
    // cgroup v2 quota ("max 100000" or "400000 100000"), then v1, then hardware threads
    try {
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string q, p;
        if (f >> q >> p && q != "max") {
            double c = std::stod(q) / std::stod(p);
            if (c >= 1) return int(c + 0.5);
        }
    } catch (...) {}
    try {
        std::ifstream fq("/sys/fs/cgroup/cpu/cpu.cfs_quota_us"), fp("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        long q = -1, p = 0;
        if ((fq >> q) && (fp >> p) && q > 0 && p > 0 && q / p >= 1) return int(q / p);
    } catch (...) {}
    unsigned h = std::thread::hardware_concurrency();
    return h ? int(h) : 1;
}

static int cmd_simulate(int argc, char** argv) {
    std::string config, out;
    bool has_seed = false;
    int64_t seed = 0;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--seed" && i + 1 < argc) { seed = std::stoll(argv[++i]); has_seed = true; }
        else if (a.rfind("--config=", 0) == 0) config = a.substr(9);
        else if (a.rfind("--out=", 0) == 0) out = a.substr(6);
    }
    if (config.empty() || out.empty()) {
        std::fprintf(stderr, "usage: simulate --config <scenario.json> --out <trace.parquet> [--seed N]\n");
        return 2;
    }
    RunResult r = run_one(config, fs::path(out), has_seed ? &seed : nullptr, true);
    std::printf("{\"scenario_id\": \"%s\", \"n_events\": %zu, \"wall_clock_sec\": %s, \"events_per_sec\": %s}\n",
                json_escape(r.scenario_id).c_str(), r.n_events, fmt_double(r.wall).c_str(),
                fmt_double(double(r.n_events) / r.wall).c_str());
    return 0;
}

static int cmd_batch(int argc, char** argv) {
    std::string bdir, odir;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--batch-dir" && i + 1 < argc) bdir = argv[++i];
        else if (a == "--out-dir" && i + 1 < argc) odir = argv[++i];
    }
    if (bdir.empty() || odir.empty()) {
        std::fprintf(stderr, "usage: simulate-batch --batch-dir <dir> --out-dir <dir>\n");
        return 2;
    }
    double t0 = now_sec();
    std::vector<fs::path> subs;
    for (auto& e : fs::directory_iterator(bdir))
        if (e.is_regular_file() && e.path().extension() == ".json") subs.push_back(e.path());
    std::sort(subs.begin(), subs.end());
    if (subs.empty()) { std::fprintf(stderr, "simulate-batch: no sub-scenarios (*.json) in %s\n", bdir.c_str()); return 2; }

    // largest-first scheduling on min(#subs, cgroup CPUs) workers; outputs are per-sub and
    // independent of scheduling (each Sim owns all of its state).
    std::vector<double> cost(subs.size(), 0);
    for (size_t i = 0; i < subs.size(); i++) {
        try {
            JVal j = parse_json(read_file(subs[i].string()));
            Scenario sc = load_scenario(j);
            double c = 0;
            for (auto& a : sc.agents) c += double(a.count) * double(sc.horizon) / double(a.interval) * (a.type == A_MM ? 2.0 * a.depth : 1.0);
            cost[i] = c;
        } catch (...) { cost[i] = 0; }
    }
    std::vector<size_t> order(subs.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cost[a] > cost[b]; });
    std::vector<RunResult> res(subs.size());
    std::vector<std::string> err(subs.size());
    std::atomic<size_t> next{0};
    int ncpu = cgroup_cpus();
    int nw = std::max(1, std::min<int>(int(subs.size()), ncpu));
    // encoder threads only when the pool leaves idle cores
    bool async_subs = int(subs.size()) * 3 <= ncpu;
    auto worker = [&]() {
        for (;;) {
            size_t k = next.fetch_add(1);
            if (k >= order.size()) return;
            size_t i = order[k];
            try {
                res[i] = run_one(subs[i].string(), fs::path(odir) / subs[i].stem() / "trace.parquet", nullptr, async_subs);
            } catch (const std::exception& ex) {
                err[i] = ex.what();
            }
        }
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < nw; w++) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    for (size_t i = 0; i < subs.size(); i++)
        if (!err[i].empty()) { std::fprintf(stderr, "simulate-batch: %s: %s\n", subs[i].string().c_str(), err[i].c_str()); return 1; }
    double wall = now_sec() - t0;
    if (wall <= 0) wall = 1e-9;
    size_t total = 0;
    for (auto& r : res) total += r.n_events;
    std::ostringstream b;
    b << "{\n  \"n_scenarios\": " << subs.size() << ",\n  \"total_events\": " << total
      << ",\n  \"wall_clock_sec\": " << fmt_double(wall)
      << ",\n  \"events_per_sec\": " << fmt_double(double(total) / wall)
      << ",\n  \"peak_memory_bytes\": " << peak_rss_bytes() << ",\n  \"gpu_seconds\": 0.0,\n  \"per_scenario\": [";
    for (size_t i = 0; i < subs.size(); i++) {
        b << (i ? "," : "") << "\n    {\"sub\": \"" << json_escape(subs[i].stem().string()) << "\", \"n_events\": " << res[i].n_events
          << ", \"trace_sha256\": \"" << res[i].sha << "\"}";
    }
    b << "\n  ]\n}\n";
    std::string bs = b.str();
    fs::create_directories(odir);
    write_file(fs::path(odir) / "batch_events.json", bs.data(), bs.size());
    std::printf("{\"n_scenarios\": %zu, \"total_events\": %zu, \"wall_clock_sec\": %s}\n", subs.size(), total, fmt_double(wall).c_str());
    return 0;
}

int main(int argc, char** argv) {
    try {
        std::string prog = fs::path(argv[0]).filename().string();
        int i = 1;
        std::string verb;
        if (argc > 1 && (std::string(argv[1]) == "simulate" || std::string(argv[1]) == "simulate-batch")) { verb = argv[1]; i = 2; }
        else if (prog == "simulate-batch") verb = "simulate-batch";
        else verb = "simulate";
        if (argc > i && (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h")) {
            std::printf("t3sim: simulate --config <scenario.json> --out <trace.parquet> | simulate-batch --batch-dir <dir> --out-dir <dir>\n");
            return 0;
        }
        if (verb == "simulate-batch") return cmd_batch(argc - i, argv + i);
        return cmd_simulate(argc - i, argv + i);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "t3sim: error: %s\n", e.what());
        return 1;
    }
}
