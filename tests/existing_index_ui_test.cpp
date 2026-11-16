// Exercise the production dispatcher with an in-memory catalog and a hidden result window.
#include <windows.h>
#include <atomic>
#include <iostream>
#include "../src/main.cpp"

static std::atomic<int> catalogCalls{0}, flowtaryCalls{0};
static bool flowtaryAvailable = false;
static bool workshopAvailable = true;
static std::atomic<int> nameResult{-1}, pathResult{-1};
static std::atomic<bool> holdCatalog{false}, catalogEntered{false};
namespace df {
bool WindowsSearchAvailable() { return true; }
std::wstring DiagProbe() { return {}; }
bool FlowtaryIndexAvailable() { return flowtaryAvailable; }
bool FlowtaryIndexPrewarm() { return flowtaryAvailable; }
bool WorkshopCatalogAvailable() { return workshopAvailable; }
WorkshopWarmupSummary WorkshopCatalogWarmup(const std::atomic<bool>*) { return {}; }
int WorkshopCatalogQuery(const Query& q, std::vector<Hit>& out) {
    out.clear();
    if (!workshopAvailable) return -1;
    if (!q.words.empty() && q.words[0] == L"current") out.push_back({L"C:\\workshop\\current-1.2.exe", false, true});
    if (!q.words.empty() && q.words[0] == L"C:\\workshop\\*.exe")
        out.push_back({L"C:\\workshop\\Flowtary-1.2.exe", false, true});
    return (int)out.size();
}
WorkshopScan ScanWorkshopExecutables(const std::wstring&, WorkshopLimits, const std::atomic<bool>*) { WorkshopScan s; s.complete = true; return s; }
std::vector<ProgramEntry> WorkshopProgramEntries(const std::vector<std::wstring>&) { return {}; }
int FlowtaryIndexQuery(const Query&, bool, std::vector<Hit>& out) {
    ++flowtaryCalls;
    out = {{L"C:\\fixture\\current.exe", false, false},
           {L"C:\\workshop\\current-1.2.exe", false, true}};
    return (int)out.size();
}
int WindowsSearchQuery(const Query& q, bool path, std::vector<Hit>& out) {
    ++catalogCalls; catalogEntered = true;
    while (holdCatalog) Sleep(1);
    out.clear(); int result = path ? pathResult.load() : nameResult.load();
    if (result > 0) out.push_back({L"C:\\fixture\\" + q.words[0] + L".txt", false});
    return result;
}
}
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT msg, WPARAM, LPARAM lp) {
    if (msg == WM_APP_DF_READY) { HandleDfReply((DfReply*)lp); return 0; }
    return DefWindowProcW(hwnd, msg, 0, lp);
}
template<class Predicate> static void PumpUntil(Predicate done) {
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (!done() && GetTickCount64() < deadline) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); Sleep(1);
    }
    Check(done(), "dispatcher finished before deadline");
}
static void Reset(int preference, int name, int path = -1) {
    g.dfBackend = preference; g.mode = Mode::Files; g.dfQuery.words = {L"current"}; g.items.clear();
    catalogCalls = 0; catalogEntered = false; nameResult = name; pathResult = path;
}
int main() {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"Flowtary.ExistingIndexTest"; cls.lpfnWndProc = TestWindowProc;
    Check(RegisterClassW(&cls) != 0, "result window class");
    g.hwnd = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    Check(g.hwnd != nullptr, "hidden result window"); g.historyEnabled = g.learnEnabled = false; g.startingUp = false;
    flowtaryAvailable = true; Reset(0, 0); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 0 && flowtaryCalls == 1 && g.searchRoute == ft::search::Route::FlowtaryIndex && g.items.front().action == L"C:\\workshop\\current-1.2.exe", "workshop .exe priority survives IPC and UI text reranking");
    flowtaryAvailable = false; workshopAvailable = false; Reset(0, -1); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 1 && g.dfWsFailed && g.searchRoute == ft::search::Route::WindowsSearch, "automatic route uses existing Windows Search catalog when snapshot absent");
    Reset(0, 1); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 1 && !g.dfWsFailed && g.searchRoute == ft::search::Route::WindowsSearch && g.items.front().kind == Row::File, "catalog query succeeds");
    Reset(1, -1); StartDfSearch();
    Check(catalogCalls == 0 && g.searchRoute == ft::search::Route::Unavailable, "explicit Flowtary mode does not start a scan when snapshot absent");
    workshopAvailable = true; Reset(1, -1); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 0 && g.searchRoute == ft::search::Route::WorkshopCatalog && !g.items.empty(), "explicit Flowtary mode falls back to owned workshop catalog");
    Reset(1, -1); g.dfQuery.words = {L"C:\\workshop\\*.exe"}; StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(g.searchRoute == ft::search::Route::WorkshopCatalog && !g.items.empty() &&
          g.items.front().action == L"C:\\workshop\\Flowtary-1.2.exe",
          "C:\\workshop\\*.exe remains handled by Flowtary when the full snapshot is absent");
    Reset(2, 0, -1); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 2 && g.dfWsFailed, "catalog path query failure propagates");
    Reset(2, 0, 0); StartDfSearch(); PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 2 && !g.dfWsFailed && g.items.front().kind == Row::Hint, "empty catalog result remains scoped to indexed locations");
    Reset(2, 1); holdCatalog = true; g.dfQuery.words = {L"old"}; StartDfSearch(); PumpUntil([] { return catalogEntered.load(); });
    g.dfQuery.words = {L"new"}; StartDfSearch(); holdCatalog = false; PumpUntil([] { return g.dfWsDone; });
    Check(catalogCalls == 2 && g.items.front().title == L"new.txt", "superseded provider result does not replace current query");
    { std::lock_guard<std::mutex> lock(s_wsMu); s_wsQuit = true; } s_wsCv.notify_one(); DestroyWindow(g.hwnd);
    std::cout << "PASS: snapshot routing, bounded catalog fallback, no traversal backend, stale-query rejection\n";
}
