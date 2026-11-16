#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "workshop_catalog.h"

#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace df {
namespace {
constexpr wchar_t kWorkshopRoot[] = L"C:\\workshop";
constexpr ULONGLONG kRefreshMilliseconds = 30000;

// Local hot roots whose immediate child directories are served from memory for
// d-search. C:\workshop is Flowtary's own app catalog; the profile roots are the
// user's common project homes. Nothing here is enumerated recursively, so this
// stays cheap (one FindFirstFile pass per root). Windows Search remains the
// fallback for anything not directly under one of these roots.
std::vector<std::wstring> HotFolderRoots() {
    std::vector<std::wstring> roots;
    roots.push_back(kWorkshopRoot);
    wchar_t profile[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        static const wchar_t* kids[] = {L"Desktop", L"project", L"WorkBuddy"};
        for (const wchar_t* kid : kids) roots.push_back(std::wstring(profile) + L"\\" + kid);
    }
    return roots;
}

struct FindHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
    FindHandle() = default;
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
};
struct PendingDirectory { std::wstring path; unsigned depth = 0; };
struct CachedPath { std::wstring path, foldedPath; };
using CatalogSnapshot = std::vector<CachedPath>;
struct CatalogData {
    CatalogSnapshot files;   // .exe descendants (f-search)
    CatalogSnapshot folders; // immediate child directories (d-search)
};
struct Cache {
    std::mutex mutex;
    std::mutex refreshMutex;
    std::shared_ptr<const CatalogData> data;
    WorkshopWarmupSummary summary;
    ULONGLONG attemptedAt = 0;
};
Cache& SharedCache() { static Cache value; return value; }

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}
bool IsExe(const wchar_t* name) {
    const size_t length = wcslen(name);
    return length >= 4 && _wcsicmp(name + length - 4, L".exe") == 0;
}
bool StartsWithI(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && CompareStringOrdinal(value.data(), (int)prefix.size(),
        prefix.data(), (int)prefix.size(), TRUE) == CSTR_EQUAL;
}
std::wstring NormalizeSeparators(std::wstring value) {
    for (auto& ch : value) if (ch == L'/') ch = L'\\';
    return value;
}
bool TermMatchesPath(const std::wstring& rawTerm, const std::wstring& foldedPath) {
    std::wstring term = Lower(NormalizeSeparators(rawTerm));
    const size_t separator = term.find_last_of(L'\\');
    if (separator != std::wstring::npos && separator + 3 < term.size() &&
        term[separator + 1] == L'*' && term[separator + 2] == L'.') {
        const std::wstring extension = term.substr(separator + 3);
        if (extension.empty() || extension.find_first_of(L"\\/*?\"<>|:") != std::wstring::npos ||
            extension != L"exe") return false;
        const std::wstring prefix = term.substr(0, separator + 1);
        return foldedPath.size() >= prefix.size() && foldedPath.compare(0, prefix.size(), prefix) == 0;
    }
    return foldedPath.find(term) != std::wstring::npos;
}
bool IsReparseDirectory(const std::wstring& path, DWORD& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { error = GetLastError(); return true; }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { error = ERROR_DIRECTORY; return true; }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = ERROR_REPARSE_TAG_INVALID; return true; }
    error = ERROR_SUCCESS;
    return false;
}

bool RefreshCache(Cache& cache, const std::atomic<bool>* cancelled, bool force,
                  WorkshopWarmupSummary* summary);

bool Snapshot(std::shared_ptr<const CatalogData>& data, bool refreshIfStale) {
    Cache& cache = SharedCache();
    bool refreshNeeded = false;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        data = cache.data;
        refreshNeeded = data && refreshIfStale && GetTickCount64() - cache.attemptedAt >= kRefreshMilliseconds;
    }
    if (refreshNeeded) {
        try { std::thread([&cache] { RefreshCache(cache, nullptr, true, nullptr); }).detach(); }
        catch (...) { /* Keep serving the immutable cached snapshot if scheduling fails. */ }
        return true;
    }
    if (data) return true;
    if (!RefreshCache(cache, nullptr, false, nullptr)) return false;
    std::lock_guard<std::mutex> lock(cache.mutex);
    data = cache.data;
    return !!data;
}

// Immediate child directories of the workshop root. One FindFirstFile pass;
// each is an application release folder such as "taskinformer-0.1.41".
static std::vector<std::wstring> ScanWorkshopFolders(const std::wstring& root) {
    std::vector<std::wstring> folders;
    const std::wstring pattern = root + L"\\*";
    WIN32_FIND_DATAW data{};
    FindHandle find;
    find.value = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                  FindExSearchNameMatch, nullptr, 0);
    if (find.value == INVALID_HANDLE_VALUE) return folders;
    do {
        if (data.cFileName[0] == L'.') continue;   // "." and ".."
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        folders.push_back(root + L"\\" + data.cFileName);
    } while (FindNextFileW(find.value, &data));
    return folders;
}

bool RefreshCache(Cache& cache, const std::atomic<bool>* cancelled, bool force,
                  WorkshopWarmupSummary* summary = nullptr) {
    std::lock_guard<std::mutex> refresh(cache.refreshMutex);
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const ULONGLONG now = GetTickCount64();
        if (cache.data && !force) { if (summary) *summary = cache.summary; return true; }
        if (cache.attemptedAt && now - cache.attemptedAt < kRefreshMilliseconds) {
            if (summary) *summary = cache.summary;
            return !!cache.data;
        }
        cache.attemptedAt = now;
    }
    WorkshopScan scanned = ScanWorkshopExecutables(kWorkshopRoot, {}, cancelled);
    WorkshopWarmupSummary result{scanned.paths.size(), scanned.directories, scanned.elapsedMilliseconds,
                                 scanned.complete, scanned.error};
    if (!scanned.complete) {
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.summary = result;
        if (summary) *summary = result;
        return false;
    }
    auto next = std::make_shared<CatalogData>();
    next->files.reserve(scanned.paths.size());
    for (auto& path : scanned.paths) next->files.push_back({std::move(path), {}});
    for (auto& entry : next->files) entry.foldedPath = Lower(entry.path);
    for (const auto& root : HotFolderRoots()) {
        for (auto& path : ScanWorkshopFolders(root)) {
            CachedPath entry;
            entry.foldedPath = Lower(path);
            entry.path = std::move(path);
            next->folders.push_back(std::move(entry));
        }
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.data = std::move(next);
    cache.summary = result;
    if (summary) *summary = result;
    return true;
}

} // namespace

bool WorkshopCatalogAvailable() {
    for (const auto& root : HotFolderRoots()) {
        const DWORD attributes = GetFileAttributesW(root.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    }
    return false;
}

WorkshopWarmupSummary WorkshopCatalogWarmup(const std::atomic<bool>* cancelled) {
    WorkshopWarmupSummary summary;
    RefreshCache(SharedCache(), cancelled, false, &summary);
    return summary;
}

WorkshopScan ScanWorkshopExecutables(const std::wstring& root, WorkshopLimits limits,
                                     const std::atomic<bool>* cancelled) {
    WorkshopScan result;
    const ULONGLONG started = GetTickCount64();
    if (IsReparseDirectory(root, result.error)) {
        result.elapsedMilliseconds = GetTickCount64() - started;
        return result;
    }
    std::vector<PendingDirectory> pending;
    pending.push_back({root, 0});
    result.complete = true;
    while (!pending.empty()) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            result.complete = false; result.error = ERROR_CANCELLED; break;
        }
        if (GetTickCount64() - started >= limits.maxMilliseconds ||
            result.directories >= limits.maxDirectories || result.paths.size() >= limits.maxEntries) {
            result.complete = false;
            break;
        }
        PendingDirectory directory = std::move(pending.back());
        pending.pop_back();
        ++result.directories;
        WIN32_FIND_DATAW data{};
        FindHandle find;
        find.value = FindFirstFileExW((directory.path + L"\\*").c_str(), FindExInfoBasic,
                                      &data, FindExSearchNameMatch, nullptr, 0);
        if (find.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                result.error = error;
                result.complete = false;
                break;
            }
            continue;
        }
        for (;;) {
            if (cancelled && cancelled->load(std::memory_order_acquire)) {
                result.complete = false; result.error = ERROR_CANCELLED; break;
            }
            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..") {
                const std::wstring fullPath = directory.path + L"\\" + name;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        directory.depth < limits.maxDepth) {
                        pending.push_back({fullPath, directory.depth + 1});
                    } else if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        result.complete = false;
                    }
                } else if (IsExe(name.c_str())) {
                    if (result.paths.size() >= limits.maxEntries) {
                        result.complete = false;
                        break;
                    }
                    result.paths.push_back(fullPath);
                }
            }
            if (!FindNextFileW(find.value, &data)) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) { result.error = error; result.complete = false; }
                break;
            }
            if (!result.complete || GetTickCount64() - started >= limits.maxMilliseconds) {
                result.complete = false;
                break;
            }
        }
        // The per-directory search handle is closed before the next directory.
        if (!result.complete) break;
    }
    if (!pending.empty()) result.complete = false;
    result.elapsedMilliseconds = GetTickCount64() - started;
    return result;
}

int WorkshopCatalogQuery(const Query& query, std::vector<Hit>& out) {
    out.clear();
    if (query.words.empty()) return 0;
    std::shared_ptr<const CatalogData> data;
    if (!Snapshot(data, true)) return -1;
    if (query.isFile) {
        for (const auto& entry : data->files) {
            if (WorkshopQueryMatchesPath(query, entry.path)) out.push_back({entry.path, false, true});
        }
    } else {
        for (const auto& entry : data->folders) {
            if (WorkshopFolderMatchesPath(query, entry.path)) out.push_back({entry.path, true, true});
        }
    }
    return static_cast<int>(out.size());
}

bool WorkshopQueryMatchesPath(const Query& query, const std::wstring& rawPath) {
    if (!query.isFile || query.words.empty()) return false;
    const std::wstring path = Lower(NormalizeSeparators(rawPath));
    const std::wstring root = Lower(kWorkshopRoot);
    if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0 ||
        path[root.size()] != L'\\' || !IsExe(path.c_str())) return false;

    for (const auto& word : query.words) {
        if (!word.empty() && !TermMatchesPath(word, path)) return false;
    }
    for (const auto& exclude : query.excludes) {
        if (!exclude.empty() && path.find(Lower(NormalizeSeparators(exclude))) != std::wstring::npos) return false;
    }
    return true;
}

bool WorkshopFolderMatchesPath(const Query& query, const std::wstring& rawPath) {
    if (query.isFile || query.words.empty()) return false;
    const std::wstring path = Lower(NormalizeSeparators(rawPath));
    // Must be an immediate child of one of the hot roots: no separator after the
    // root's leaf. Try each root; the entry's real parent matches exactly one.
    std::wstring leaf;
    bool immediateChild = false;
    for (const auto& rawRoot : HotFolderRoots()) {
        const std::wstring root = Lower(NormalizeSeparators(rawRoot));
        if (path.size() > root.size() + 1 && path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == L'\\' &&
            path.find(L'\\', root.size() + 1) == std::wstring::npos) {
            leaf = path.substr(root.size() + 1);
            immediateChild = true;
            break;
        }
    }
    if (!immediateChild) return false;

    for (const auto& word : query.words) {
        if (word.empty()) continue;
        const std::wstring term = Lower(NormalizeSeparators(word));
        if (leaf.find(term) == std::wstring::npos) return false;
    }
    for (const auto& exclude : query.excludes) {
        if (!exclude.empty() && path.find(Lower(NormalizeSeparators(exclude))) != std::wstring::npos) return false;
    }
    return true;
}
} // namespace df
