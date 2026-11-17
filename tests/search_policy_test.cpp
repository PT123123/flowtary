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

    // 应用身份归一化：同一应用的不同版本目录归到同一个 key，升级不丢历史使用。
    using ft::search::AppIdentityKey;
    {
        auto k1 = AppIdentityKey(L"C:\\workshop\\taskinformer-0.1.11\\taskinformer.exe");
        auto k2 = AppIdentityKey(L"C:\\workshop\\taskinformer-0.1.48\\taskinformer.exe");
        Check(!k1.empty() && k1 == k2, "version dirs collapse to one app identity");
    }
    {
        auto ka = AppIdentityKey(L"C:\\workshop\\aura-1.2.25\\aura.exe");
        auto kb = AppIdentityKey(L"C:\\workshop\\taskinformer-0.1.11\\taskinformer.exe");
        Check(ka != kb, "different apps keep different identities");
    }
    {
        // 无版本号的普通程序：身份=小写全路径，大小写不敏感但路径不同则不同
        auto k1 = AppIdentityKey(L"C:\\Apps\\Trae\\Trae.exe");
        auto k2 = AppIdentityKey(L"c:\\apps\\trae\\trae.exe");
        auto k3 = AppIdentityKey(L"C:\\Apps\\Trae CN\\TraeCN.exe");
        Check(k1 == k2 && k1 != k3, "non-versioned programs use lowercased full path");
    }

    // 最近使用加分：越近越高，时间未知(<=0)不加分，30 天以上不加分。
    using ft::search::RecencyBonus;
    const long long NOW = 1000000000LL;
    const long long DAY = 86400;
    {
        Check(RecencyBonus(0, NOW) == 0, "unknown last time gets no bonus");
        Check(RecencyBonus(NOW, NOW) == 8, "used today gets highest bonus");
        Check(RecencyBonus(NOW - 2 * DAY, NOW) == 5, "used within 3 days");
        Check(RecencyBonus(NOW - 6 * DAY, NOW) == 3, "used within 7 days");
        Check(RecencyBonus(NOW - 20 * DAY, NOW) == 1, "used within 30 days");
        Check(RecencyBonus(NOW - 90 * DAY, NOW) == 0, "used long ago gets no bonus");
        Check(RecencyBonus(NOW + DAY, NOW) == 8, "future timestamp clamped to today");
    }

    // 综合分：次数仍是主因，最近使用只温和加权；taskinformer 场景——
    // 版本分散导致每版 4 次，聚合后 5 版=20 次，压过单次常驻程序 23 次中近期用过的那一档。
    using ft::search::UsageScore;
    {
        Check(UsageScore(4, 0, NOW) == 4, "no recency info: score is pure count");
        Check(UsageScore(4, NOW, NOW) == 12, "count plus today bonus");
        Check(UsageScore(20, NOW - DAY, NOW) > UsageScore(23, 0, NOW),
              "aggregated recent usage outranks a higher static count");
    }

    if (g_fail) { std::printf("\n%d FAILED\n", g_fail); return 1; }
    std::printf("\nall passed\n");
    return 0;
}
