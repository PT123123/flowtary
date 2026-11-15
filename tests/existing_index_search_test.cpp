// No live filesystem traversal or search service is used by these regressions.
#include "../src/df_search.cpp"
#include "search_policy.h"
#include <iostream>
#include <thread>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    using namespace ft::search;
    Check(SelectRoute(0, true) == Route::FlowtaryIndex, "automatic prefers an available Flowtary snapshot");
    Check(SelectRoute(0, false) == Route::WindowsSearch, "automatic uses SystemIndex until a Flowtary snapshot exists");
    Check(SelectRoute(1, true) == Route::FlowtaryIndex, "explicit Flowtary snapshot");
    Check(SelectRoute(1, false) == Route::Unavailable, "missing explicit Flowtary snapshot reports unavailable");
    Check(SelectRoute(2, true) == Route::WindowsSearchOnly && SelectRoute(2, false) == Route::WindowsSearchOnly,
          "explicit SystemIndex remains selected");
    Check(HasExplicitPath({L"C:\\workshop", L"tool.exe"}), "drive path selects path postings");
    Check(HasExplicitPath({L"folder/subfolder"}), "slash path selects path postings");
    Check(!HasExplicitPath({L"workshop", L"tool.exe"}), "ordinary terms stay on filename postings");

    df::Query q;
    q.words = {L"fixture"};
    std::vector<df::Hit> hits;
    int calls = 0;
    auto current = [] { return true; };
    Check(df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool path, auto& out) {
        ++calls;
        if (path) out.push_back({L"C:\\fixture\\name.txt", false});
        return path ? 1 : 0;
    }, current) == 1 && calls == 2 && hits.size() == 1, "empty name query retries path within catalog");
    calls = 0;
    Check(df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool, auto&) {
        ++calls; return -1;
    }, current) == -1 && calls == 1 && hits.empty(), "unavailable provider stops without retry or traversal");
    calls = 0;
    Check(df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool path, auto&) {
        ++calls; return path ? -1 : 0;
    }, current) == -1 && calls == 2, "path query failure is reported rather than masked as no results");
    calls = 0;
    Check(df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool, auto& out) {
        ++calls; out.push_back({L"C:\\fixture.txt", false}); return 1;
    }, current) == 1 && calls == 1, "name hits require only one query");
    calls = 0;
    df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool, auto&) {
        ++calls; return 0;
    }, [] { return false; });
    Check(calls == 0 && hits.empty(), "superseded queued work never contacts provider");
    bool stillCurrent = true;
    df::QueryWithPathFallback(q, hits, [&](const df::Query&, bool, auto&) {
        ++calls; stillCurrent = false; return 0;
    }, [&] { return stillCurrent; });
    Check(calls == 1, "superseded name query cannot schedule a path query");

    Check(df::SqlEscape(L"a'b") == L"a''b", "catalog query quotes apostrophes");
    Check(df::ItemUrlToPath(L"file:///C:/fixture/a%20b.txt") == L"C:\\fixture\\a b.txt", "catalog URL decoding");
    // Win10/11 目录里文件夹的 System.ItemType 实测是 "Directory"；只认 "Folder"
    // 会把全部文件夹判成文件，d 搜索整片漏掉（2026-11-15 justbrowse 事故）。
    Check(df::IsFolderItemType(L"Directory"), "catalog Directory entries are folders");
    Check(df::IsFolderItemType(L"Folder"), "legacy Folder spelling remains a folder");
    Check(df::IsFolderItemType(L"directory") && df::IsFolderItemType(L"FOLDER"),
          "folder type comparison ignores case");
    Check(!df::IsFolderItemType(L".zip") && !df::IsFolderItemType(L".lnk") &&
          !df::IsFolderItemType(L"File folder"), "extension and other type values stay files");
    Check(!df::IsFolderItemType(L"") && !df::IsFolderItemType(nullptr),
          "missing ItemType stays a file");
    for (int i = 0; i < 32; ++i) {
        std::thread worker([] {
            APTTYPE apartment;
            APTTYPEQUALIFIER qualifier;
            APTTYPE originalApartment{};
            APTTYPEQUALIFIER originalQualifier{};
            HRESULT original = CoGetApartmentType(&originalApartment, &originalQualifier);
            { df::WsCache cache;
              Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)), "cache owns an apartment"); }
            HRESULT after = CoGetApartmentType(&apartment, &qualifier);
            Check(after == original && (FAILED(after) ||
                  (apartment == originalApartment && qualifier == originalQualifier)),
                  "cache releases COM apartment when its thread scope ends");
        });
        worker.join();
    }
    std::cout << "PASS: existing-index routing, bounded catalog fallback, folder type classification, cancellation, COM lifetime\n";
}
