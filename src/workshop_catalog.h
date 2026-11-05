#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include "df_search.h"
#include <string>
#include <vector>

namespace df {
struct WorkshopLimits {
    size_t maxEntries = 20000;
    size_t maxDirectories = 4096;
    unsigned maxDepth = 12;
    ULONGLONG maxMilliseconds = 1500;
};
struct WorkshopScan {
    std::vector<std::wstring> paths;
    size_t directories = 0;
    ULONGLONG elapsedMilliseconds = 0;
    bool complete = false;
    DWORD error = ERROR_SUCCESS;
};
struct WorkshopWarmupSummary {
    size_t entries = 0;
    size_t directories = 0;
    ULONGLONG elapsedMilliseconds = 0;
    bool complete = false;
    DWORD error = ERROR_SUCCESS;
};

// This Flowtary-owned rule is deliberately scoped to C:\workshop\*.exe.
bool WorkshopCatalogAvailable();
WorkshopWarmupSummary WorkshopCatalogWarmup(const std::atomic<bool>* cancelled = nullptr);
WorkshopScan ScanWorkshopExecutables(const std::wstring& root, WorkshopLimits limits = {},
                                     const std::atomic<bool>* cancelled = nullptr);
int WorkshopCatalogQuery(const Query& query, std::vector<Hit>& out);
// Pure matcher used by the catalog and its regression tests. Matches only
// canonical descendants of C:\workshop with a final .exe component.
bool WorkshopQueryMatchesPath(const Query& query, const std::wstring& path);
} // namespace df
