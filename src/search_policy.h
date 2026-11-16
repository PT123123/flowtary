#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
namespace ft::search {
// Registry values 0 and 1 preserve existing preferences; 2 selects Windows Search only.
enum class Route { Unavailable, WindowsSearch, WindowsSearchOnly, FlowtaryIndex, WorkshopCatalog };
inline Route SelectRoute(int preference, bool flowtaryAvailable, bool workshopAvailable = false) {
    if (preference == 2) return Route::WindowsSearchOnly;
    if (preference == 1) return flowtaryAvailable ? Route::FlowtaryIndex
        : (workshopAvailable ? Route::WorkshopCatalog : Route::Unavailable);
    if (flowtaryAvailable) return Route::FlowtaryIndex;
    return Route::WindowsSearch;
}
// A slash in an f/d query means the user supplied a path fragment. The Flowtary
// path posting can search it directly; sending it through filename postings first
// wastes a query and can never match a full path such as "C:\\workshop\\tool".
inline bool HasExplicitPath(const std::vector<std::wstring>& words) {
    for (const auto& word : words)
        if (word.find_first_of(L"\\/") != std::wstring::npos) return true;
    return false;
}

// How a stored self-learned term relates to the current f/d query term.
// Lower rank wins. Bidirectional prefixes let a short learned term (e.g.
// "life-reco") still surface when the user types the full name ("life-record"),
// and vice versa. A one-character learned term is only accepted on exact match
// to avoid it prefix-matching every longer query.
//   -1 no match | 0 exact | 1 learned starts with query | 2 query starts with learned
inline int LearnedTermMatchRank(const std::wstring& learned, const std::wstring& term) {
    if (term.empty() || learned.empty()) return -1;
    if (learned == term) return 0;
    if (learned.size() > term.size() &&
        learned.compare(0, term.size(), term) == 0) return 1;
    if (term.size() > learned.size() && learned.size() >= 2 &&
        term.compare(0, learned.size(), learned) == 0) return 2;
    return -1;
}

// When the program catalog scan is cut short by its budget, a frequently used
// program can fall outside the enumerated set even though click weights/learned
// entries still point at it. Fold the per-term counts into a per-path total,
// keep only paths used at least minWeight times, and return them hottest first.
// Ties are broken by path so the result is deterministic. The caller converts
// surviving paths into catalog entries (and re-checks that the file exists).
inline std::vector<std::pair<std::wstring, int>> TopWeightedPaths(
    const std::vector<std::pair<std::wstring, int>>& candidates,
    int minWeight = 2, size_t limit = 64) {
    std::unordered_map<std::wstring, int> totals;
    for (const auto& kv : candidates) totals[kv.first] += kv.second;
    std::vector<std::pair<std::wstring, int>> out;
    out.reserve(totals.size());
    for (const auto& kv : totals)
        if (kv.second >= minWeight) out.emplace_back(kv.first, kv.second);
    std::sort(out.begin(), out.end(),
        [](const std::pair<std::wstring, int>& a, const std::pair<std::wstring, int>& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
    if (out.size() > limit) out.resize(limit);
    return out;
}
} // namespace ft::search
