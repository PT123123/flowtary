// White-box resource regressions: child output, traversal caps, bounded deltas and mmap lifetime.
#include "../src/df_search.cpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

static std::atomic<bool> comScopesBalanced{true};

static void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static SIZE_T PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    Check(GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)), "memory counter");
    return pm.PrivateUsage;
}
static std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        if (wcscmp(argv[1], L"--emit") == 0) {
            std::string row = "C:\\fixture\\" + std::string(300, 'x') + ".txt\n";
            for (int i = 0; i < 1000000; ++i)
                if (fwrite(row.data(), 1, row.size(), stdout) != row.size()) break;
            return 0;
        }
        if (wcscmp(argv[1], L"--fragment") == 0) { fputs("C:\\final.txt", stdout); return 0; }
        if (wcscmp(argv[1], L"--fail") == 0) { fputs("C:\\partial.txt\n", stdout); return 2; }
        if (wcscmp(argv[1], L"--stall") == 0) { fputs("C:\\unfinished", stdout); fflush(stdout); Sleep(5000); return 0; }
        if (wcscmp(argv[1], L"--long") == 0) {
            std::string invalid(2 * 1024 * 1024, 'x');
            fwrite(invalid.data(), 1, invalid.size(), stdout);
            return 0;
        }
        return 2;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SIZE_T baseline = PrivateBytes(), peak = baseline;
    size_t lines = 0;
    Check(df::FdCaptureStream(self, L" --emit", 2000, [&](std::string&& row) {
        Check(row.size() > 300, "complete row");
        if (++lines % 8192 == 0) peak = (std::max)(peak, PrivateBytes());
        return true;
    }), "stream emitter succeeded");
    Check(lines == 1000000, "all million rows consumed");
    Check(peak - baseline < 32 * 1024 * 1024, "stream memory stays bounded below 32 MiB growth");
    std::cout << "stream: 1000000 rows, peak private growth " << (peak - baseline) / 1024 << " KiB\n";
    lines = 0;
    Check(df::FdCaptureStream(self, L" --fragment", 2000, [&](std::string&& row) {
        ++lines; return row == "C:\\final.txt";
    }) && lines == 1, "unterminated successful final row");
    Check(!df::FdCaptureStream(self, L" --fail", 2000, [](std::string&&) { return true; }), "nonzero child exit rejected");
    lines = 0;
    Check(!df::FdCaptureStream(self, L" --stall", 100, [&](std::string&&) { ++lines; return true; }) && lines == 0,
          "timeout rejects unfinished output");
    lines = 0;
    Check(!df::FdCaptureStream(self, L" --emit", 2000, [&](std::string&&) { return ++lines < 20; }) && lines == 20,
          "sink failure stops producer");
    Check(!df::FdCaptureStream(self, L" --long", 2000, [](std::string&&) { return true; }), "malformed line cannot grow buffer forever");
    for (int i = 0; i < 32; ++i) {
        std::thread worker([] {
            APTTYPE apartment;
            APTTYPEQUALIFIER qualifier;
            { df::ThreadComScope scope;
              if (FAILED(CoGetApartmentType(&apartment, &qualifier))) comScopesBalanced = false; }
            if (CoGetApartmentType(&apartment, &qualifier) != CO_E_NOTINITIALIZED) comScopesBalanced = false;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            { df::ThreadComScope nested; }
            if (FAILED(CoGetApartmentType(&apartment, &qualifier))) comScopesBalanced = false;
            CoUninitialize();
        });
        worker.join();
    }
    Check(comScopesBalanced, "short-lived and nested COM scopes balance before thread teardown");
    df::g_dfPinyinEnabled = true;
    df::MemPinyinStart();
    Check(df::g_py != nullptr, "real pinyin dictionary loaded");
    auto mixedPinyin = df::MemNameToPinyin(L"\u5FAE\u4FE1\U0001F445\u65E5\u62A5.txt");
    Check(mixedPinyin.find(L"weixin") != std::wstring::npos && mixedPinyin.find(L"ribao") != std::wstring::npos,
          "Chinese plus emoji converts without crashing");
    Check(ft::PinyinBmpText(std::wstring{L'a', 0xD800, L'b', 0xDC00}) == L"ab", "unpaired surrogates removed from phonetic text");

    // A change flood must invalidate the disk layer and fall back, rather than accumulate forever.
    df::DiskDeltaClearLocked();
    df::g_ovOverflow = false;
    df::g_diReady = true;
    for (int i = 0; i < 100000; ++i) {
        std::wstring p = L"c:\\flood\\" + std::to_wstring(i) + std::wstring(200, L'x');
        df::DiskDeltaApplyLocked(p, p, 0);
        Check(df::g_ovBytes <= df::kDiskDeltaMaxBytes, "delta byte budget");
        Check(df::g_ovAdd.size() + df::g_ovDel.size() <= df::kDiskDeltaMaxEntries, "delta entry budget");
    }
    Check(df::g_ovOverflow && !df::g_diReady && df::g_diNeedsRebuild, "overflow invalidates disk index");
    Check(df::g_ovBytes == 0 && df::g_ovAdd.empty() && df::g_ovDel.empty(), "overflow releases allocations");
    df::g_ovOverflow = false;
    df::DiskDeltaApplyLocked(L"c:\\old", L"C:\\old", 0);
    ULONGLONG cutoff = df::g_ovGeneration;
    df::DiskDeltaApplyLocked(L"c:\\new", L"C:\\new", 0);
    df::DiskDeltaApplyLocked(L"c:\\deleted", L"C:\\deleted", -1);
    df::DiskDeltaFoldLocked(cutoff);
    Check(!df::g_ovAdd.count(L"c:\\old") && df::g_ovAdd.count(L"c:\\new") && df::g_ovDel.count(L"c:\\deleted"),
          "fold preserves changes arriving during rebuild");
    df::DiskDeltaApplyLocked(L"c:\\new", L"C:\\new", -1);
    Check(!df::g_ovAdd.count(L"c:\\new") && df::g_ovDel.count(L"c:\\new"), "add/delete replacement");
    df::DiskDeltaFoldLocked(df::g_ovGeneration);
    Check(df::g_ovBytes == 0, "delta accounting returns to zero");

    auto fixture = std::filesystem::path(self).parent_path() / (L"search-memory-fixture-" + std::to_wstring(GetCurrentProcessId()));
    auto root = fixture / L"root";
    auto cache = fixture / L"cache";
    std::filesystem::create_directories(root / L"folder");
    std::filesystem::create_directories(cache);
    for (int i = 0; i < 8; ++i) std::ofstream(root / (L"cap-probe-" + std::to_wstring(i) + L".txt")) << "probe";
    std::ofstream(root / L"\u5FAE\u4FE1\U0001F445\u65E5\u62A5.txt") << "emoji";
    SetEnvironmentVariableW(L"FT_DF_CACHE_DIR", cache.c_str());
    SetEnvironmentVariableW(L"FT_DF_DISKINDEX_ROOTS", root.c_str());
    SetEnvironmentVariableW(L"FT_DF_DISKINDEX_MAX", L"1000");
    auto fd = df::FindFdExe();
    Check(!fd.empty(), "fd.exe available alongside tests");
    df::MemRootData part;
    Check(df::MemListRootFd(fd, root.wstring(), 3, part) && part.size() == 3, "mem cap shared across directory/file passes");
    Check(df::MemListRootFd(fd, root.wstring(), 0, part) && part.size() == 0, "zero cap never means unlimited");
    df::DiskBuildThread(nullptr);
    Check(df::g_diReady && !df::g_diLastBuildError && df::g_diRecords == 10, "complete isolated disk build with emoji filename");
    df::Query emojiQuery;
    emojiQuery.words = {L"weixinribao"};
    std::vector<df::Hit> emojiHits;
    Check(df::DiskIndexSearch(emojiQuery, false, emojiHits, 20) == 1 &&
          emojiHits[0].path.find(L"\U0001F445") != std::wstring::npos, "phonetic lookup retains original emoji path");
    std::ofstream(root / L"second-build-probe.txt") << "second";
    df::DiskBuildThread(nullptr);
    Check(df::g_diReady && !df::g_diLastBuildError && df::g_diRecords == 11, "replace an already mapped disk index");
    df::Query q;
    q.words = {L"second-build-probe"};
    std::vector<df::Hit> hits;
    Check(df::DiskIndexSearch(q, false, hits, 20) == 1, "rebuilt index is searchable");
    auto before = ReadFileText(cache / L"diskindex.dat");
    SetEnvironmentVariableW(L"FT_DF_DISKINDEX_ROOTS", (root / L"missing").c_str());
    df::DiskBuildThread(nullptr);
    Check(df::g_diLastBuildError && df::g_diReady, "failed traversal preserves usable old mapping");
    Check(before == ReadFileText(cache / L"diskindex.dat"), "failed traversal never publishes a partial index");
    auto base = df::g_diBase;
    df::DiskUnmapLocked();
    MEMORY_BASIC_INFORMATION mbi{};
    Check(VirtualQuery(base, &mbi, sizeof(mbi)) && mbi.State == MEM_FREE, "unmap releases the allocation base");
    df::g_diQuit = true;
    Sleep(100);
    std::filesystem::remove_all(fixture);
    std::cout << "PASS: stream failures, COM teardown, delta flood/generation, traversal caps, rebuild rollback, mmap release\n";
    return 0;
}
