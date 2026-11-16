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

// This Flowtary-owned rule is deliberately scoped to C:\workshop.
bool WorkshopCatalogAvailable();
WorkshopWarmupSummary WorkshopCatalogWarmup(const std::atomic<bool>* cancelled = nullptr);
WorkshopScan ScanWorkshopExecutables(const std::wstring& root, WorkshopLimits limits = {},
                                     const std::atomic<bool>* cancelled = nullptr);
// Files (query.isFile=true) are Flowtary's C:\workshop\*.exe catalog. Folders
// (query.isFile=false, d-search) are the immediate child directories of every
// local hot root: C:\workshop plus %USERPROFILE%\{Desktop,project,WorkBuddy}.
// Both are served from memory; neither enumerates the directory tree per query.
int WorkshopCatalogQuery(const Query& query, std::vector<Hit>& out);
// Pure matcher for .exe descendants of C:\workshop (kept for regression tests).
bool WorkshopQueryMatchesPath(const Query& query, const std::wstring& path);
// Pure matcher for an immediate child directory of any local hot root (d-search).
bool WorkshopFolderMatchesPath(const Query& query, const std::wstring& path);

// Program-mode entries distilled from the C:\workshop exe tree. One row per exe
// name: when several release directories coexist (Flowtary-1.1.34 … -1.1.37),
// only the newest official release survives. The name strips the trailing
// version so "flowtary-1.1.37.10.exe" is searchable as "flowtary"; auxiliary
// exes (indexer/settings/…) keep their full stem under their own row.
struct ProgramEntry { std::wstring name, path; };
std::vector<ProgramEntry> WorkshopProgramEntries(const std::vector<std::wstring>& exePaths);
} // namespace df
