"""One-off refactor: replace main.cpp's whole-run encode path with the streaming sink."""
p = 'engine/src/main.cpp'
s = open(p, encoding='utf8').read()
a = s.index('struct Encoded {')
b = s.index('static int cgroup_cpus()')
new = open('engine/tools/stream_sink.inc', encoding='utf8').read()
s = s[:a] + new + s[b:]
s = s.replace('#include "parquet.hpp"\n', '#include "output.hpp"\n')
s = s.replace('RunResult r = run_one(config, fs::path(out), has_seed ? &seed : nullptr);',
              'RunResult r = run_one(config, fs::path(out), has_seed ? &seed : nullptr, true);')
s = s.replace('res[i] = run_one(subs[i].string(), fs::path(odir) / subs[i].stem() / "trace.parquet", nullptr);',
              'res[i] = run_one(subs[i].string(), fs::path(odir) / subs[i].stem() / "trace.parquet", nullptr, async_subs);')
s = s.replace('    int nw = std::max(1, std::min<int>(int(subs.size()), cgroup_cpus()));',
              '    int ncpu = cgroup_cpus();\n    int nw = std::max(1, std::min<int>(int(subs.size()), ncpu));\n'
              '    // encoder threads only when the pool leaves idle cores\n    bool async_subs = int(subs.size()) * 3 <= ncpu;')
open(p, 'w', encoding='utf8', newline='\n').write(s)
print('patched')
