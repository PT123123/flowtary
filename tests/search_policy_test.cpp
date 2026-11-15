// Pure f/d query policy assertions (ctest target search_policy).
// Covers bidirectional self-learned term matching so a short learned term still
// surfaces when the user types the full/ longer name.
#include "search_policy.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_fail;
}

int main() {
    using ft::search::LearnedTermMatchRank;
    // Exact
    Check(LearnedTermMatchRank(L"training", L"training") == 0, "exact match ranks 0");
    // Forward: learned term is longer and starts with the query (legacy behavior)
    Check(LearnedTermMatchRank(L"training", L"train") == 1, "learned startswith query ranks 1");
    Check(LearnedTermMatchRank(L"talk2agent", L"talk") == 1, "forward prefix on talk2agent");
    // Reverse: query is longer and starts with the learned term (the fix)
    Check(LearnedTermMatchRank(L"life-reco", L"life-record") == 2, "query startswith learned ranks 2");
    Check(LearnedTermMatchRank(L"aw-fl", L"aw-flutterui") == 2, "reverse prefix on aw-fl*");
    Check(LearnedTermMatchRank(L"key", L"keystores") == 2, "reverse prefix on key/keystores");
    // Sibling names that only share a short stem are not prefix-related.
    Check(LearnedTermMatchRank(L"aw-an", L"aw-flutterui") == -1, "sibling names do not prefix-match");
    // Exact wins over prefixes
    Check(LearnedTermMatchRank(L"kmusic", L"kmusic") == 0 &&
          LearnedTermMatchRank(L"kmusic", L"kmu") == 1, "exact outranks forward prefix");
    // No match
    Check(LearnedTermMatchRank(L"housing", L"house") == -1, "non-prefix does not match");
    Check(LearnedTermMatchRank(L"crawlbase", L"crawler") == -1, "partial word does not match");
    Check(LearnedTermMatchRank(L"activitywatch", L"flutter") == -1, "unrelated term does not match");
    // One-character learned term: only exact, never a reverse prefix
    Check(LearnedTermMatchRank(L"k", L"kmusic") == -1, "single-char learned term not used as prefix");
    Check(LearnedTermMatchRank(L"k", L"k") == 0, "single-char learned term matches exactly");
    // Empty inputs
    Check(LearnedTermMatchRank(L"", L"foo") == -1, "empty learned term does not match");
    Check(LearnedTermMatchRank(L"foo", L"") == -1, "empty query does not match");
    // Ranking order: exact < forward < reverse
    Check(LearnedTermMatchRank(L"work", L"work") < LearnedTermMatchRank(L"workshop", L"work") &&
          LearnedTermMatchRank(L"workshop", L"work") < LearnedTermMatchRank(L"work", L"workshop"),
          "rank ordering exact < forward < reverse");

    if (g_fail) { std::printf("\n%d FAILED\n", g_fail); return 1; }
    std::printf("\nall passed\n");
    return 0;
}
