#pragma once
#include <string>
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
} // namespace ft::search
