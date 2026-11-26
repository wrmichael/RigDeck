#include "version.h"
#include <cstdio>
#include <cstring>

namespace ver {

V parse(const std::string& s) {
    V v;
    int n = 0;
    if (sscanf(s.c_str(), "%d.%d.%d%n", &v.a, &v.b, &v.c, &n) != 3 || v.a < 0 || v.b < 0 || v.c < 0) return v;
    std::string rest = s.substr((size_t)n);
    if (!rest.empty() && rest != "-beta") return v;          // unknown suffix: not comparable
    v.beta = !rest.empty();
    v.ok = true;
    return v;
}

int cmp(const V& x, const V& y) {
    if (x.a != y.a) return x.a < y.a ? -1 : 1;
    if (x.b != y.b) return x.b < y.b ? -1 : 1;
    if (x.c != y.c) return x.c < y.c ? -1 : 1;
    if (x.beta != y.beta) return x.beta ? -1 : 1;
    return 0;
}

bool offer(const std::string& running, const std::string& stable, const std::string& beta, bool betaNotice,
           std::string& version, std::string& channel) {
    V r = parse(running), s = parse(stable), b = parse(beta);
    if (!r.ok) return false;                                  // "dev" builds and the like: never nag
    bool haveS = s.ok && !s.beta && cmp(s, r) > 0;
    bool haveB = betaNotice && b.ok && cmp(b, r) > 0 && (!s.ok || cmp(b, s) > 0);
    if (haveB) { version = beta; channel = "beta"; return true; }
    if (haveS) { version = stable; channel = "stable"; return true; }
    return false;
}

}  // namespace ver
