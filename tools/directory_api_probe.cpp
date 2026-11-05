// Bounded, user-mode probe for the directory-batched FileId API.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <queue>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    HANDLE get() const { return value; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};

struct DirectoryHandle {
    Handle handle;
    size_t* active;
    size_t* peak;
    DirectoryHandle(HANDLE raw, size_t& count, size_t& high) : handle(raw), active(&count), peak(&high) {
        ++*active;
        *peak = std::max(*peak, *active);
    }
    ~DirectoryHandle() { if (active) --*active; }
    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;
};

struct Work { std::filesystem::path path; uint64_t id; unsigned depth; };
struct Totals {
    DWORD pid = 0;
    uint64_t entries = 0, directories = 0, files = 0, exe_files = 0;
    uint64_t batches = 0, api_us = 0, reparse_dirs = 0;
    uint64_t opened = 0, id_mismatches = 0, passes = 0;
    uint64_t workload_start_unix_ms = 0, workload_end_unix_ms = 0;
    uint64_t phase1_passes = 0, phase2_passes = 0;
    uint64_t phase1_end_unix_ms = 0, pause_start_unix_ms = 0, pause_end_unix_ms = 0, phase2_start_unix_ms = 0;
    size_t active = 0, peak = 0;
    DWORD process_handles_start = 0, process_handles_end = 0;
    SIZE_T private_start = 0, private_end = 0;
    LARGE_INTEGER frequency{};
};

uint64_t UnixMillis() {
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    constexpr uint64_t kUnixEpochFileTime = 116'444'736'000'000'000ULL;
    return ticks.QuadPart >= kUnixEpochFileTime ? (ticks.QuadPart - kUnixEpochFileTime) / 10'000 : 0;
}

double ElapsedUs(LARGE_INTEGER begin, LARGE_INTEGER end, LARGE_INTEGER frequency) {
    return 1'000'000.0 * (end.QuadPart - begin.QuadPart) / frequency.QuadPart;
}

std::wstring Arg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i) if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}

uint64_t NumberArg(int argc, wchar_t** argv, const wchar_t* name, uint64_t fallback) {
    const auto value = Arg(argc, argv, name);
    if (value.empty()) return fallback;
    wchar_t* end = nullptr;
    const auto parsed = wcstoull(value.c_str(), &end, 10);
    return end && *end == L'\0' ? parsed : UINT64_MAX;
}

bool ReadDirectory(const Work& item, Totals& totals, std::queue<Work>& pending) {
    constexpr size_t kBufferBytes = 64 * 1024;
    constexpr uint64_t kMaxEntries = 2'000'000;
    constexpr uint64_t kMaxDirectories = 200'000;
    constexpr unsigned kMaxDepth = 32;
    constexpr ULONGLONG kMaxMs = 30'000;
    static const ULONGLONG started = GetTickCount64();
    if (GetTickCount64() - started > kMaxMs || totals.entries > kMaxEntries || totals.directories > kMaxDirectories) return false;

    const std::wstring path = item.path.native();
    const HANDLE raw = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ++totals.opened;
    DirectoryHandle directory(raw, totals.active, totals.peak);
    const HANDLE handle = directory.handle.get();

    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        ++totals.reparse_dirs;
        return true;
    }
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity))) return false;
    if (std::any_of(identity.FileId.Identifier + 8, identity.FileId.Identifier + 16, [](BYTE b) { return b != 0; })) return false;
    uint64_t actual = 0;
    std::memcpy(&actual, identity.FileId.Identifier, sizeof(actual));
    if (actual != item.id) { ++totals.id_mismatches; return false; }

    alignas(8) std::array<uint64_t, kBufferBytes / sizeof(uint64_t)> buffer{};
    bool first = true;
    for (;;) {
        if (GetTickCount64() - started > kMaxMs) return false;
        const auto info_class = first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
        first = false;
        LARGE_INTEGER call_start{}, call_end{};
        QueryPerformanceCounter(&call_start);
        const BOOL ok = GetFileInformationByHandleEx(handle, info_class, buffer.data(), (DWORD)kBufferBytes);
        QueryPerformanceCounter(&call_end);
        totals.api_us += (uint64_t)ElapsedUs(call_start, call_end, totals.frequency);
        ++totals.batches;
        if (!ok) {
            if (GetLastError() == ERROR_NO_MORE_FILES) break;
            return false;
        }
        const auto* bytes = reinterpret_cast<const BYTE*>(buffer.data());
        size_t offset = 0;
        for (;;) {
            if (offset > kBufferBytes - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) return false;
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(bytes + offset);
            const size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (entry->FileNameLength % sizeof(WCHAR) != 0 ||
                entry->FileNameLength > kBufferBytes - offset - header) return false;
            const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
            const size_t needed = header + entry->FileNameLength;
            if (entry->NextEntryOffset && (entry->NextEntryOffset < needed || entry->NextEntryOffset > kBufferBytes - offset)) return false;
            if (name != L"." && name != L".." && name != L".flowtary-dirapi-fixture") {
                if (++totals.entries > kMaxEntries) return false;
                const bool is_dir = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool is_reparse = (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (is_dir) {
                    ++totals.directories;
                    if (is_reparse) ++totals.reparse_dirs;
                    else if (item.depth < kMaxDepth) pending.push({item.path / name, (uint64_t)entry->FileId.QuadPart, item.depth + 1});
                    else return false;
                } else {
                    ++totals.files;
                    if (name.size() >= 4 && CompareStringOrdinal(name.data() + name.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL) ++totals.exe_files;
                }
            }
            if (!entry->NextEntryOffset) break;
            offset += entry->NextEntryOffset;
        }
    }
    return true;
}

bool WriteReport(const std::filesystem::path& path, const Totals& t, double elapsed_ms, bool complete) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << std::setprecision(12)
        << "{\"schema\":1,\"mode\":\"bounded-local-directory-api-probe\",\"pid\":" << t.pid
        << ",\"complete\":" << (complete ? "true" : "false")
        << ",\"directories_opened\":" << t.opened << ",\"directory_handles\":{\"active_end\":" << t.active
        << ",\"peak_active\":" << t.peak << "},\"entries\":" << t.entries << ",\"directories\":" << t.directories
        << ",\"files\":" << t.files << ",\"exe_files\":" << t.exe_files << ",\"batches\":" << t.batches
        << ",\"passes\":" << t.passes
        << ",\"workload_start_unix_ms\":" << t.workload_start_unix_ms
        << ",\"workload_end_unix_ms\":" << t.workload_end_unix_ms
        << ",\"phase1_passes\":" << t.phase1_passes << ",\"phase1_end_unix_ms\":" << t.phase1_end_unix_ms
        << ",\"pause_start_unix_ms\":" << t.pause_start_unix_ms << ",\"pause_end_unix_ms\":" << t.pause_end_unix_ms
        << ",\"phase2_start_unix_ms\":" << t.phase2_start_unix_ms << ",\"phase2_passes\":" << t.phase2_passes
        << ",\"api_elapsed_us\":" << t.api_us << ",\"walk_elapsed_ms\":" << elapsed_ms
        << ",\"reparse_directories_skipped\":" << t.reparse_dirs << ",\"file_id_mismatches\":" << t.id_mismatches
        << ",\"process_handles\":{\"start\":" << t.process_handles_start << ",\"end\":" << t.process_handles_end
        << "},\"private_bytes\":{\"start\":" << t.private_start << ",\"end\":" << t.private_end
        << "},\"raw_volume_opens\":0,\"ordinary_file_handles_opened\":0,\"file_contents_read\":0}\n";
    return out.good();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path root = Arg(argc, argv, L"--root");
    const std::filesystem::path output = Arg(argc, argv, L"--output");
    const std::wstring start_name = Arg(argc, argv, L"--start-event");
    const std::wstring stop_name = Arg(argc, argv, L"--stop-event");
    const uint64_t repeat_ms = NumberArg(argc, argv, L"--duration-ms", 0);
    const uint64_t active_ms = NumberArg(argc, argv, L"--active-ms", 0);
    const uint64_t pause_ms = NumberArg(argc, argv, L"--pause-ms", 0);
    const uint64_t resume_ms = NumberArg(argc, argv, L"--resume-ms", 0);
    const bool staged = active_ms != 0 || pause_ms != 0 || resume_ms != 0;
    if (repeat_ms > 20'000 || (staged && (repeat_ms != 0 || active_ms == 0 || pause_ms == 0 || resume_ms == 0 ||
        active_ms > 20'000 || pause_ms > 20'000 || resume_ms > 20'000 || active_ms + pause_ms + resume_ms > 20'000))) return 6;
    if (root.empty() || output.empty() || start_name.empty() || stop_name.empty()) return 1;
    const auto marker = root / L".flowtary-dirapi-fixture";
    {
        std::ifstream marker_file(marker, std::ios::binary);
        std::string marker_contents((std::istreambuf_iterator<char>(marker_file)), {});
        if (marker_contents != "FLOWTARY-DIRAPI-FIXTURE-V1") return 2;
    }

    Handle start(OpenEventW(SYNCHRONIZE, FALSE, start_name.c_str()));
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name.c_str()));
    if (!start || !stop || WaitForSingleObject(start.get(), 20'000) != WAIT_OBJECT_0) return 3;

    Totals totals;
    totals.pid = GetCurrentProcessId();
    QueryPerformanceFrequency(&totals.frequency);
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    GetProcessHandleCount(GetCurrentProcess(), &totals.process_handles_start);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) totals.private_start = memory.PrivateUsage;
    std::queue<Work> pending;
    Handle root_handle(CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ID_INFO root_id{};
    if (!root_handle || !GetFileInformationByHandleEx(root_handle.get(), FileIdInfo, &root_id, sizeof(root_id)) ||
        std::any_of(root_id.FileId.Identifier + 8, root_id.FileId.Identifier + 16, [](BYTE b) { return b != 0; })) return 4;
    root_handle = Handle{};
    uint64_t root_file_id = 0;
    std::memcpy(&root_file_id, root_id.FileId.Identifier, sizeof(root_file_id));
    bool complete = true;
    LARGE_INTEGER walk_start{}, walk_end{};
    totals.workload_start_unix_ms = UnixMillis();
    QueryPerformanceCounter(&walk_start);
    auto run_pass = [&]() {
        pending.push({root, root_file_id, 0});
        while (!pending.empty()) {
            Work item = std::move(pending.front()); pending.pop();
            if (!ReadDirectory(item, totals, pending)) { complete = false; break; }
        }
        if (!complete) return false;
        ++totals.passes;
        return true;
    };
    auto run_stage = [&](uint64_t duration_ms, uint64_t& pass_count) {
        const ULONGLONG stage_started = GetTickCount64();
        do {
            if (!run_pass()) return;
            ++pass_count;
        } while (duration_ms != 0 && GetTickCount64() - stage_started < duration_ms);
    };
    if (staged) {
        run_stage(active_ms, totals.phase1_passes);
        totals.phase1_end_unix_ms = UnixMillis();
        if (complete) {
            totals.pause_start_unix_ms = UnixMillis();
            Sleep((DWORD)pause_ms);
            totals.pause_end_unix_ms = UnixMillis();
            totals.phase2_start_unix_ms = UnixMillis();
            run_stage(resume_ms, totals.phase2_passes);
        }
    } else {
        run_stage(repeat_ms, totals.phase1_passes);
    }
    totals.workload_end_unix_ms = UnixMillis();
    QueryPerformanceCounter(&walk_end);
    const double elapsed_ms = 1000.0 * (walk_end.QuadPart - walk_start.QuadPart) / totals.frequency.QuadPart;
    GetProcessHandleCount(GetCurrentProcess(), &totals.process_handles_end);
    memory = {}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) totals.private_end = memory.PrivateUsage;
    const bool written = WriteReport(output, totals, elapsed_ms, complete && pending.empty() && totals.active == 0);
    const bool released = WaitForSingleObject(stop.get(), 20'000) == WAIT_OBJECT_0;
    return written && released && complete && pending.empty() && totals.active == 0 && totals.peak <= 1 && totals.id_mismatches == 0 ? 0 : 5;
}
