// RigDeck version numbers ("1.1.6", "1.1.6-beta") and the "update available" decision.
#pragma once
#include <string>

namespace ver {

struct V {
    int a = 0, b = 0, c = 0;
    bool beta = false, ok = false;
};
V parse(const std::string& s);
// <0 a older than b, 0 same, >0 newer. For the same numbers a Stable release is newer than its beta.
int cmp(const V& x, const V& y);

// Which release to offer, given what runs and what the RigDeck site has ("" = not known). A Stable release
// is offered when newer than what runs; a Beta only when betaNotice is on and it is newer than both what
// runs and the Stable release on offer. Returns false when there is nothing to offer.
bool offer(const std::string& running, const std::string& stable, const std::string& beta, bool betaNotice,
           std::string& version, std::string& channel);

}  // namespace ver
