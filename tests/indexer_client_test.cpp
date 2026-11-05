#include "../src/indexer_client.h"
#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static unsigned long long CurrentProcessCpu100ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}
static void PublishChildPidForSampler() {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"FLOWTARY_TEST_CHILD_PID_FILE", path, (DWORD)std::size(path));
    if (!length) return;
    Check(length < std::size(path), "child pid output path fits its buffer");
    const unsigned long pid = df::FlowtaryIndexTestChildPid();
    Check(pid != 0, "persistent query child has a process id");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create child pid handoff file");
    char value[32]{}; const int count = std::snprintf(value, sizeof(value), "%lu", pid);
    DWORD written = 0;
    const BOOL ok = count > 0 && WriteFile(file, value, (DWORD)count, &written, nullptr) && written == (DWORD)count;
    CloseHandle(file);
    Check(ok != 0, "publish child pid to resource sampler");
}
int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--integration" || std::string(argv[1]) == "--integration-stress")) {
        df::Query query; query.words = {L"flowtary"}; query.isFile = true;
        bool pathMode=false;
        std::vector<df::Hit> live;
        if (std::string(argv[1]) == "--integration-stress") {
            char queryText[129]{};
            const DWORD queryLength = GetEnvironmentVariableA("FLOWTARY_TEST_QUERY", queryText, (DWORD)sizeof(queryText));
            if (queryLength > 0 && queryLength < sizeof(queryText)) {
                query.words = {std::wstring(queryText, queryText + queryLength)};
            }
            char pathModeText[8]{};
            const DWORD pathModeLength = GetEnvironmentVariableA("FLOWTARY_TEST_PATH_QUERY", pathModeText, (DWORD)sizeof(pathModeText));
            pathMode = pathModeLength == 1 && pathModeText[0] == '1';
            const bool workshopExePattern = pathMode && query.words.size() == 1 && query.words[0].size() >= 6 &&
                CompareStringOrdinal(query.words[0].data() + query.words[0].size() - 6, 6, L"\\*.exe", 6, TRUE) == CSTR_EQUAL;
            auto verifyWorkshopExeHits = [&] {
                if (!workshopExePattern) return;
                Check(!live.empty(), "C:\\workshop\\*.exe returns at least one indexed executable");
                for (const auto& hit : live) {
                    Check(!hit.isFolder && hit.path.size() > 12 &&
                        CompareStringOrdinal(hit.path.data(), 12, L"C:\\workshop\\", 12, TRUE) == CSTR_EQUAL &&
                        CompareStringOrdinal(hit.path.data() + hit.path.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL,
                        "C:\\workshop\\*.exe results stay under the requested root and have the .exe extension");
                }
            };
            long durationSeconds = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 0;
            long queriesPerSecond = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 0;
            Check(durationSeconds >= 0 && durationSeconds <= 3600 && queriesPerSecond >= 0 && queriesPerSecond <= 1000,
                  "stress duration and query rate are bounded");
            const size_t queryCount = queriesPerSecond > 0
                ? 1 + (size_t)durationSeconds * (size_t)queriesPerSecond : 100000;
            Check(queryCount > 0 && queryCount <= 2000000, "stress query count is bounded");
            Sleep(2000); // let the external whole-machine sampler establish its baseline
            const auto clientCpuBeforeStartup = CurrentProcessCpu100ns();
            auto startup = std::chrono::steady_clock::now();
            Check(df::FlowtaryIndexPrewarm(), "persistent query child prewarms the bounded snapshot");
            const auto startupMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-startup).count();
            PublishChildPidForSampler();
            const auto clientCpuAfterStartup = CurrentProcessCpu100ns();
            const auto childCpuAfterStartup = df::FlowtaryIndexTestChildCpu100ns();
            auto firstQueryStarted=std::chrono::steady_clock::now();
            const int startupCount = df::FlowtaryIndexQuery(query, pathMode, live);
            const auto firstQueryMicros=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-firstQueryStarted).count();
            Check(startupCount > 0, "first query after prewarm returns a complete non-empty result set");
            verifyWorkshopExeHits();
            std::vector<long long> micros; micros.reserve(queryCount);
            const auto clientCpuBeforeWarm = CurrentProcessCpu100ns();
            const auto childCpuBeforeWarm = df::FlowtaryIndexTestChildCpu100ns();
            const auto childPidBeforeWarm = df::FlowtaryIndexTestChildPid();
            auto warmStarted = std::chrono::steady_clock::now();
            const auto warmStartUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < queryCount; ++i) {
                auto start = std::chrono::steady_clock::now();
                const int count = df::FlowtaryIndexQuery(query, pathMode, live);
                Check(count == startupCount, "stress query returns the same complete result set");
                verifyWorkshopExeHits();
                micros.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
                if (queriesPerSecond > 0 && i + 1 < queryCount) {
                    std::this_thread::sleep_until(warmStarted + std::chrono::milliseconds((1000 * (i + 1)) / queriesPerSecond));
                }
            }
            const auto warmElapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-warmStarted).count();
            const auto warmEndUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto clientCpuAfterWarm = CurrentProcessCpu100ns();
            const auto childCpuAfterWarm = df::FlowtaryIndexTestChildCpu100ns();
            const auto childPidAfterWarm = df::FlowtaryIndexTestChildPid();
            std::sort(micros.begin(), micros.end());
            std::cout << "startup_us=" << startupMicros
                      << ",startup_client_cpu_us=" << (clientCpuAfterStartup-clientCpuBeforeStartup)/10
                      << ",startup_child_cpu_us=" << childCpuAfterStartup/10
                      << ",first_query_us=" << firstQueryMicros
                      << ",queries=" << micros.size() << ",warm_elapsed_us=" << warmElapsedMicros
                      << ",warm_start_unix_ms=" << warmStartUnixMs << ",warm_end_unix_ms=" << warmEndUnixMs
                      << ",warm_client_cpu_us=" << (clientCpuAfterWarm-clientCpuBeforeWarm)/10
                      << ",warm_child_cpu_us=" << (childCpuAfterWarm-childCpuBeforeWarm)/10
                      << ",p50_us=" << micros[micros.size()/2]
                      << ",p95_us=" << micros[(micros.size()-1)*95/100]
                      << ",max_us=" << micros.back()
                      << ",child_pid_before_warm=" << childPidBeforeWarm
                      << ",child_pid_after_warm=" << childPidAfterWarm << std::endl;
            Sleep(7000); // retain the monitored client through the system telemetry window
            return 0;
        }
        const int count = df::FlowtaryIndexQuery(query, false, live);
        Check(count == 4 && live.front().path.rfind(L"T:\\", 0) == 0,
              "persistent child process returns indexed snapshot results over anonymous pipes");
        std::cout << "PASS: persistent snapshot child query IPC (" << count << " hits)\n";
        return 0;
    }
    std::vector<df::Hit> hits;
    const std::string reply = R"({"id":7,"result":{"candidates_examined":2,"elapsed_us":19,"hits":[{"backup":false,"is_folder":false,"name_score":1000,"path":"C:\\workshop\\\uD83D\uDE80.exe","version":[1,2],"workshop":true}],"stale":false,"truncated":false},"schema":1})";
    Check(df::ParseFlowtaryIndexReply(reply, hits), "parses persistent index response with reordered fields");
    Check(hits.size() == 1 && hits[0].path == L"C:\\workshop\\\U0001F680.exe" && !hits[0].isFolder && hits[0].workshop,
          "decodes escaped supplementary Unicode and preserves file type and workshop priority");
    Check(!df::ParseFlowtaryIndexReply(R"({"id":7,"error":"bad query","schema":1})", hits) && hits.empty(),
          "rejects provider error and clears partial results");
    Check(!df::ParseFlowtaryIndexReply(R"({"result":{"hits":[{"path":"x"}]}})", hits) && hits.empty(),
          "rejects malformed hit rather than returning a partial path");
    Check(!df::ParseFlowtaryIndexReply(R"({"result":{"hits":[],"stale":true,"truncated":false}})", hits),
          "rejects stale snapshots");
    Check(!df::ParseFlowtaryIndexReply(R"({"result":{"hits":[],"stale":false,"truncated":true}})", hits),
          "does not present time-truncated results as complete");
    std::cout << "PASS: persistent index reply JSON and Unicode parsing\n";
}
