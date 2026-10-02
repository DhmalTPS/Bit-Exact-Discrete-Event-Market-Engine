#include "../src/rng.hpp"
#include <cstdio>
using namespace t3;
int main() {
    unsigned seeds[] = {0u, 1u, 42u, 1001u, 2147483647u, 4294967295u};
    for (unsigned s : seeds) {
        MT19937 g(s);
        for (int i = 0; i < 3; i++) printf("u32 %u %u\n", s, g.seed_draw());
        for (int i = 0; i < 5; i++) printf("nrm %u %.17g\n", s, g.normal(10.0, 2.0));
        for (int i = 0; i < 5; i++) printf("lgn %u %.17g\n", s, g.lognormal(6.2146080984221914, 0.5));
        for (int i = 0; i < 5; i++) printf("par %u %.17g\n", s, g.pareto(1.5));
        for (int i = 0; i < 5; i++) printf("uni %u %.17g\n", s, g.uniform(100.0, 2000.0));
        for (int i = 0; i < 5; i++) printf("exp %u %.17g\n", s, g.exponential(5e8));
        for (int i = 0; i < 8; i++) printf("ri2 %u %lld\n", s, (long long)g.randint(0, 2));
        for (int i = 0; i < 8; i++) printf("ri6 %u %lld\n", s, (long long)g.randint(0, 6));
        for (int i = 0; i < 4; i++) printf("ri1 %u %lld\n", s, (long long)g.randint(0, 1));
        for (int i = 0; i < 3; i++) printf("nrm %u %.17g\n", s, g.normal(0.0, 0.0));
    }
}
