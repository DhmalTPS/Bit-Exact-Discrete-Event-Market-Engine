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
        for (int i = 0; i < kPool; i++) { tfree_.push_back(new TraceChunk); lfree_.push_back(new LedgerChunk); }
        types.reserve(1 << 16);
        tc = take(tfree_);
        lc = take(lfree_);
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
        tc = take(tfree_);
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
        lc = take(lfree_);
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
    static constexpr int kPool = 4;
    out::Worker tw_, lw_;
    std::vector<out::ColumnChunk> tcols_, lcols_;
    std::mutex pm_;
    std::condition_variable pcv_;
    std::vector<TraceChunk*> tfree_;
    std::vector<LedgerChunk*> lfree_;
    int64_t seqbuf_[CHUNK];
    template <class T> T* take(std::vector<T*>& pool) {
        std::unique_lock<std::mutex> lk(pm_);
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
    Sim sim(sc, *sink);
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

