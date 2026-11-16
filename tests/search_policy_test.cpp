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

    // Catalog-truncation fallback: fold per-term weights into per-path totals,
    // keep only genuinely frequent paths, hottest first.
    using ft::search::TopWeightedPaths;
    {
        auto hot = TopWeightedPaths({
            {L"C:\\a\\valorant.lnk", 3}, {L"C:\\a\\valorant.lnk", 4},   // 7 across two terms
            {L"C:\\a\\once.exe", 1},                                    // clicked once: noise
            {L"C:\\a\\editor.exe", 2},
        });
        Check(hot.size() == 2 && hot[0].first == L"C:\\a\\valorant.lnk" &&
              hot[0].second == 7 && hot[1].first == L"C:\\a\\editor.exe" &&
              hot[1].second == 2, "per-term weights fold into one hot path");
    }
    {
        auto none = TopWeightedPaths({{L"C:\\a\\once.exe", 1}});
        Check(none.empty(), "single click is not frequent enough to restore");
    }
    {
        auto capped = TopWeightedPaths({
            {L"C:\\a\\p1.exe", 9}, {L"C:\\a\\p2.exe", 8}, {L"C:\\a\\p3.exe", 7},
        }, 2, 2);
        Check(capped.size() == 2 && capped[0].first == L"C:\\a\\p1.exe" &&
              capped[1].first == L"C:\\a\\p2.exe", "restore list respects the cap");
    }
    {
        auto tied = TopWeightedPaths({{L"C:\\b\\z.exe", 5}, {L"C:\\a\\a.exe", 5}});
        Check(tied.size() == 2 && tied[0].first == L"C:\\a\\a.exe" &&
              tied[1].first == L"C:\\b\\z.exe", "equal weights sort by path");
    }

    if (g_fail) { std::printf("\n%d FAILED\n", g_fail); return 1; }
    std::printf("\nall passed\n");
    return 0;
}
