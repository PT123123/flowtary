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

    // d-search: immediate application folders under C:\workshop (seconds-out source).
    df::Query folder;
    folder.isFile = false;
    folder.words = {L"taskinformer"};
    Check(df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\taskinformer-0.1.41"),
          "folder query matches an immediate release directory by name");
    Check(df::WorkshopFolderMatchesPath(folder, L"C:/WORKSHOP/TASKINFORMER-0.1.41"),
          "folder matching is case-insensitive and normalizes separators");
    Check(!df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\foo\\taskinformer-0.1.41"),
          "folder query rejects non-immediate (nested) directories");
    Check(!df::WorkshopFolderMatchesPath(folder, L"C:\\workshop-old\\taskinformer-0.1.41"),
          "folder query rejects sibling path prefixes");
    Check(!df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\taskinformer-0.1.41\\x.exe"),
          "folder query rejects files");
    folder.words = {L"alpha", L"beta"};
    Check(df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\alpha-beta-1.0") &&
          !df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\alpha-1.0"),
          "folder query ANDs all words");
    folder.excludes = {L"beta"};
    Check(!df::WorkshopFolderMatchesPath(folder, L"C:\\workshop\\alpha-beta-1.0"),
          "folder query applies excludes");
    Check(!df::WorkshopFolderMatchesPath(query /* isFile=true */, L"C:\\workshop\\taskinformer-0.1.41"),
          "file query never matches folders");

    // d-search: personal project roots (Desktop / project / WorkBuddy) are hot too.
    wchar_t profile[MAX_PATH]{};
    const DWORD profileLen = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    Check(profileLen > 0 && profileLen < MAX_PATH, "USERPROFILE available for hot-root tests");
    const std::wstring desktop = std::wstring(profile) + L"\\Desktop";
    df::Query personal;
    personal.isFile = false;
    personal.words = {L"aw-flutterui"};
    Check(df::WorkshopFolderMatchesPath(personal, desktop + L"\\aw-flutterui"),
          "folder query matches an immediate Desktop child");
    Check(!df::WorkshopFolderMatchesPath(personal, desktop + L"\\parent\\aw-flutterui"),
          "folder query rejects nested Desktop directories");
    Check(!df::WorkshopFolderMatchesPath(personal, std::wstring(profile) + L"\\DesktopX\\aw-flutterui"),
          "folder query rejects sibling root prefixes");
    personal.words = {L"project"};
    Check(df::WorkshopFolderMatchesPath(personal, std::wstring(profile) + L"\\project\\my-project"),
          "folder query matches an immediate project child");
    personal.words = {L"aw"};
    personal.excludes = {L"android"};
    Check(!df::WorkshopFolderMatchesPath(personal, desktop + L"\\aw-android-native"),
          "folder query applies excludes to personal-root paths");

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

    // Program mode: workshop exes become launcher rows. Same exe name across
    // release dirs collapses to the newest official build; auxiliary exes keep
    // their full stem; the displayed name strips a trailing version.
    const std::vector<std::wstring> exePaths = {
        L"C:\\workshop\\Flowtary-1.1.34.10\\flowtary-1.1.34.10.exe",
        L"C:\\workshop\\Flowtary-1.1.36.10\\flowtary-1.1.36.10.exe",
        L"C:\\workshop\\Flowtary-1.1.37.10\\flowtary-1.1.37.10.exe",
        L"C:\\workshop\\Flowtary-1.1.37.10\\flowtary-indexer.exe",
        L"C:\\workshop\\Flowtary-1.1.36.10\\flowtary-indexer.exe",
        L"C:\\workshop\\taskinformer-0.1.44\\taskinformer.exe",
        L"C:\\workshop\\taskinformer-0.1.45\\taskinformer.exe",
        L"C:\\workshop\\taskinformer-0.1.45\\peview.exe",
    };
    auto programs = df::WorkshopProgramEntries(exePaths);
    Check(programs.size() == 4, "workshop programs collapse to one row per exe name");
    auto find = [&programs](const std::wstring& name) -> const df::ProgramEntry* {
        for (const auto& p : programs) if (p.name == name) return &p;
        return nullptr;
    };
    Check(find(L"flowtary") && find(L"flowtary")->path.find(L"1.1.37.10") != std::wstring::npos,
          "newest official release wins, and the name drops its version");
    Check(find(L"flowtary-indexer") && find(L"flowtary-indexer")->path.find(L"1.1.37.10") != std::wstring::npos,
          "auxiliary exe is its own row, also deduped to newest version");
    Check(find(L"taskinformer") && find(L"taskinformer")->path.find(L"0.1.45") != std::wstring::npos,
          "version-less exe name dedupes by its release directory version");
    Check(find(L"peview") != nullptr, "other exes in a release dir remain searchable");
    Check(df::WorkshopProgramEntries({L"C:\\workshop\\a.txt", L"C:\\workshop\\noext"}).empty(),
          "non-exe paths never become program rows");
    std::cout << "PASS: workshop exe program rows, version dedupe, and clean names\n";
}
