#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "app_ver.h"
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

// 最近使用加分：用得越近，加分越高。时间未知（last<=0，例如旧的 3 列 weights.dat
// 读入的数据）不加分，让老数据平滑退回「纯次数」排序，不会因为升级而行为突变。
// 按天分档，避开浮点与时钟漂移。
inline int RecencyBonus(long long lastUnix, long long nowUnix) {
    if (lastUnix <= 0 || nowUnix <= 0) return 0;
    long long d = nowUnix - lastUnix;
    if (d < 0) d = 0;                 // 时钟回拨/未来时间按「今天」处理
    const long long DAY = 86400;
    if (d <= DAY) return 8;
    if (d <= 3 * DAY) return 5;
    if (d <= 7 * DAY) return 3;
    if (d <= 30 * DAY) return 1;
    return 0;
}

// 综合使用分 = 跨版本累计次数 + 最近使用加分。次数仍是主因，近期使用只是温和加权，
// 避免「偶然点过一次的新程序」冲到长期高频项前面。
inline int UsageScore(int totalCount, long long lastUnix, long long nowUnix) {
    const int c = totalCount > 0 ? totalCount : 0;
    return c + RecencyBonus(lastUnix, nowUnix);
}

// 应用身份键：把同一应用的不同版本目录（...\taskinformer-0.1.11\taskinformer.exe 与
// ...\taskinformer-0.1.48\taskinformer.exe）归一到同一个 key，让升级不丢历史使用。
// 复用 app_ver.h 的分组键（父目录+去版本号的应用名，不同应用互不压制）；
// 无版本号的普通程序回退到小写完整路径，等价旧行为（不同路径各自独立）。
inline std::wstring AppIdentityKey(const std::wstring& path) {
    if (path.empty()) return path;
    const appver::Info v = appver::Parse(path);
    if (v.has && !v.app.empty()) return v.app;
    return appver::Lower(path);
}
} // namespace ft::search
