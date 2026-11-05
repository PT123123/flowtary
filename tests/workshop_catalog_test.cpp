#include "workshop_catalog.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    df::Query query;
    query.words = {L"C:\\workshop\\*.exe"};
    Check(df::WorkshopQueryMatchesPath(query, L"C:\\workshop\\Flowtary-1.2.exe"),
          "custom root wildcard matches only the owned exe catalog");
    Check(!df::WorkshopQueryMatchesPath(query, L"C:\\workshop-old\\Flowtary-1.2.exe"),
          "custom root wildcard rejects sibling path prefixes");
    Check(!df::WorkshopQueryMatchesPath(query, L"C:\\workshop\\Flowtary-1.2.txt"),
          "custom root wildcard requires an executable");
    query.words = {L"C:/WORKSHOP/*.EXE"};
    Check(df::WorkshopQueryMatchesPath(query, L"C:\\workshop\\Flowtary-1.2.exe"),
          "wildcard path matching is case-insensitive and normalizes separators");
    query.words = {L"C:\\workshop\\*.txt"};
    Check(!df::WorkshopQueryMatchesPath(query, L"C:\\workshop\\Flowtary-1.2.exe"),
          "unsupported extension wildcard fails closed");
    query.words = {L"C:\\workshop\\*.exe"}; query.excludes = {L"Flowtary"};
    Check(!df::WorkshopQueryMatchesPath(query, L"C:\\workshop\\Flowtary-1.2.exe"),
          "excludes are applied to the complete custom path");

    wchar_t temp[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "temporary directory available");
    std::wstring root = std::wstring(temp) + L"FlowtaryWorkshopTest-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root + L"\\nested");
    { HANDLE file = CreateFileW((root + L"\\FlowtaryFixture.exe").c_str(), GENERIC_WRITE, 0, nullptr,
          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); Check(file != INVALID_HANDLE_VALUE, "create fixture exe"); CloseHandle(file); }
    { HANDLE file = CreateFileW((root + L"\\nested\\NestedFixture.EXE").c_str(), GENERIC_WRITE, 0, nullptr,
          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); Check(file != INVALID_HANDLE_VALUE, "create nested exe"); CloseHandle(file); }
    { HANDLE file = CreateFileW((root + L"\\ignored.txt").c_str(), GENERIC_WRITE, 0, nullptr,
          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); Check(file != INVALID_HANDLE_VALUE, "create non-exe"); CloseHandle(file); }

    df::WorkshopLimits limits;
    limits.maxMilliseconds = 5000;
    auto scan = df::ScanWorkshopExecutables(root, limits);
    Check(scan.complete && scan.error == ERROR_SUCCESS, "bounded traversal completes");
    Check(scan.directories == 2 && scan.paths.size() == 2, "only exe entries in real directories are captured");

    limits.maxDirectories = 1;
    auto capped = df::ScanWorkshopExecutables(root, limits);
    Check(!capped.complete && capped.directories == 1, "directory cap returns incomplete snapshot");
    std::atomic<bool> cancelled{true};
    auto stopped = df::ScanWorkshopExecutables(root, {}, &cancelled);
    Check(!stopped.complete && stopped.error == ERROR_CANCELLED, "startup prewarm honors cancellation before traversal");
    std::filesystem::remove_all(root);
    std::cout << "PASS: workshop traversal, filtering, and resource limits\n";
}
