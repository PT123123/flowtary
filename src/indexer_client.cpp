#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include "indexer_client.h"
#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>

namespace df {
namespace {
struct Child {
    HANDLE process = nullptr, input = nullptr, output = nullptr, readTimer = nullptr;
    bool attempted = false, ready = false, servicePipe = false, serviceTried = false;
    ~Child() { Stop(); }
    void Stop() {
        if (input && input == output) { CloseHandle(input); input = nullptr; output = nullptr; }
        else {
            if (input) { CloseHandle(input); input = nullptr; }
            if (output) { CloseHandle(output); output = nullptr; }
        }
        if (readTimer) { CloseHandle(readTimer); readTimer = nullptr; }
        if (process) { if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) TerminateProcess(process, 1); CloseHandle(process); process = nullptr; }
        ready = false; servicePipe = false; serviceTried = false; attempted = false;
    }
};
Child child;
std::mutex childMutex;
constexpr wchar_t kIndexerPipe[] = L"\\\\.\\pipe\\Flowtary.Indexer.v1";

bool TrustedIndexerPipe(HANDLE pipe) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || !owner || !descriptor) {
        if (descriptor) LocalFree(descriptor);
        return false;
    }
    BYTE systemSid[SECURITY_MAX_SID_SIZE]{};
    DWORD sidBytes = sizeof(systemSid);
    const BOOL made = CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &sidBytes);
    const bool trusted = made && EqualSid(owner, systemSid);
    LocalFree(descriptor);
    return trusted;
}

bool ConnectIndexerService() {
    HANDLE pipe = CreateFileW(kIndexerPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    if (!TrustedIndexerPipe(pipe)) { CloseHandle(pipe); return false; }
    child.input = pipe;
    child.output = pipe;
    child.servicePipe = true;
    child.readTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!child.readTimer) child.readTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    return true;
}

std::wstring ModuleDir() {
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    if (!n || n >= std::size(path)) return {};
    std::wstring result(path, n);
    size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
}
std::wstring SnapshotPath() {
#if defined(FLOWTARY_TEST_SNAPSHOT_OVERRIDE)
    wchar_t testSnapshot[32768]{};
    DWORD testLength = GetEnvironmentVariableW(L"FLOWTARY_INDEXER_TEST_SNAPSHOT", testSnapshot, (DWORD)std::size(testSnapshot));
    if (testLength > 0 && testLength < std::size(testSnapshot)) return std::wstring(testSnapshot, testLength);
#endif
    wchar_t base[32768]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, (DWORD)std::size(base));
    if (!n || n >= std::size(base)) return {};
    return std::wstring(base, n) + L"\\Flowtary\\index-C.fti";
}
bool StartChild() {
    if (child.attempted) {
        if (child.servicePipe) return true;
        if (child.process && WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0) return true;
        child.Stop();
    }
    child.attempted = true;
#if !defined(FLOWTARY_TEST_SNAPSHOT_OVERRIDE)
    if (!child.serviceTried) {
        child.serviceTried = true;
        if (ConnectIndexerService()) return true;
    }
#endif
    const std::wstring dir = ModuleDir(), snapshot = SnapshotPath();
    if (dir.empty() || snapshot.empty()) { child.attempted = false; child.serviceTried = false; return false; }
    const std::wstring exe = dir + L"\\flowtary-indexer.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(snapshot.c_str()) == INVALID_FILE_ATTRIBUTES) { child.attempted = false; child.serviceTried = false; return false; }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childInRead = nullptr, parentInWrite = nullptr;
    HANDLE parentOutRead = nullptr, childOutWrite = nullptr;
    if (!CreatePipe(&childInRead, &parentInWrite, &sa, 0) ||
        !CreatePipe(&parentOutRead, &childOutWrite, &sa, 0)) {
        if (childInRead) CloseHandle(childInRead); if (parentInWrite) CloseHandle(parentInWrite);
        if (parentOutRead) CloseHandle(parentOutRead); if (childOutWrite) CloseHandle(childOutWrite);
        child.attempted = false; child.serviceTried = false;
        return false;
    }
    SetHandleInformation(parentInWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOutRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullErr == INVALID_HANDLE_VALUE) nullErr = nullptr;
    std::wstring command = L"\"" + exe + L"\" serve-snapshot --snapshot \"" + snapshot + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childInRead; si.hStdOutput = childOutWrite; si.hStdError = nullErr ? nullErr : childOutWrite;
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(childInRead); CloseHandle(childOutWrite); if (nullErr) CloseHandle(nullErr);
    if (!created) { CloseHandle(parentInWrite); CloseHandle(parentOutRead); child.attempted = false; child.serviceTried = false; return false; }
    CloseHandle(pi.hThread);
    child.process = pi.hProcess; child.input = parentInWrite; child.output = parentOutRead;
    child.readTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!child.readTimer) child.readTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    return true;
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (!n) return {};
    std::string out(n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), out.data(), n, nullptr, nullptr)) return {};
    return out;
}
std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { const char* hex = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (char)c;
    }
    return out + "\"";
}
bool WriteLine(HANDLE pipe, const std::string& line) {
    DWORD written = 0;
    return WriteFile(pipe, line.data(), (DWORD)line.size(), &written, nullptr) && written == line.size();
}
bool ReadLine(HANDLE pipe, HANDLE process, std::string& line, DWORD timeoutMs) {
    line.clear();
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    unsigned int yields = 0;
    constexpr size_t kMaxResponseBytes = 3 * 1024 * 1024;
    while (GetTickCount64() < deadline && line.size() <= kMaxResponseBytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (!available) {
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return false;
            // Fast replies usually arrive during a few scheduler yields. For a slower
            // child, switch to one reusable high-resolution timer instead of spinning.
            if (yields < 64) { ++yields; SwitchToThread(); continue; }
            yields = 0;
            if (child.readTimer) {
                LARGE_INTEGER due{}; due.QuadPart = -1000; // 100 microseconds
                if (SetWaitableTimer(child.readTimer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(child.readTimer, 10);
                    continue;
                }
            }
            Sleep(1); continue;
        }
        yields = 0;
        char buf[4096]; DWORD got = 0;
        if (!ReadFile(pipe, buf, (DWORD)std::min<size_t>(sizeof(buf), available), &got, nullptr) || !got) return false;
        for (DWORD i = 0; i < got; ++i) { if (buf[i] == '\n') return true; line += buf[i]; }
    }
    return false;
}
bool EnsureChildReady(DWORD timeoutMs) {
    if (child.ready) return child.servicePipe || (child.process && WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0);
    std::string line;
    if (!ReadLine(child.output, child.process, line, timeoutMs) || line != "{\"schema\":1,\"ready\":true}") {
        const bool serviceRejected = child.servicePipe;
        child.Stop();
        if (serviceRejected) {
            child.serviceTried = true;
            return StartChild() && EnsureChildReady(timeoutMs);
        }
        return false;
    }
    child.ready = true;
    return true;
}
void AppendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7f) out += (char)cp;
    else if (cp <= 0x7ff) { out += (char)(0xc0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
    else if (cp <= 0xffff) { out += (char)(0xe0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
    else { out += (char)(0xf0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 63)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
}
bool ParseString(const std::string& s, size_t& i, std::wstring& out) {
    if (i >= s.size() || s[i++] != '"') return false;
    std::string utf8;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i++];
        if (c == '"') {
            if (utf8.empty()) { out.clear(); return true; }
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
            if (!n) return false; out.resize(n);
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), out.data(), n) == n;
        }
        if (c != '\\') { utf8 += (char)c; continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        if (e == '"' || e == '\\' || e == '/') utf8 += e;
        else if (e == 'b') utf8 += '\b'; else if (e == 'f') utf8 += '\f';
        else if (e == 'n') utf8 += '\n'; else if (e == 'r') utf8 += '\r'; else if (e == 't') utf8 += '\t';
        else if (e == 'u') {
            if (i + 4 > s.size()) return false; unsigned cp = 0;
            for (int k = 0; k < 4; ++k) { char h = s[i++]; cp <<= 4; if (h >= '0' && h <= '9') cp |= h-'0'; else if (h >= 'a' && h <= 'f') cp |= h-'a'+10; else if (h >= 'A' && h <= 'F') cp |= h-'A'+10; else return false; }
            if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= s.size() && s[i] == '\\' && s[i+1] == 'u') {
                i += 2; unsigned low = 0; for (int k=0;k<4;++k) { char h=s[i++]; low<<=4; if(h>='0'&&h<='9')low|=h-'0';else if(h>='a'&&h<='f')low|=h-'a'+10;else if(h>='A'&&h<='F')low|=h-'A'+10;else return false; }
                if (low < 0xdc00 || low > 0xdfff) return false; cp = 0x10000 + ((cp-0xd800)<<10) + low-0xdc00;
            }
            AppendUtf8(utf8, cp);
        } else return false;
    }
    return false;
}
bool SkipValue(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i >= s.size()) return false;
    if (s[i] == '"') { std::wstring ignored; return ParseString(s, i, ignored); }
    if (s[i] == '[' || s[i] == '{') {
        std::vector<char> stack; stack.push_back(s[i++] == '[' ? ']' : '}'); bool inString=false, escape=false;
        while (i < s.size() && !stack.empty()) {
            char c=s[i++];
            if (inString) { if (escape) escape=false; else if (c=='\\') escape=true; else if (c=='"') inString=false; continue; }
            if (c=='"') inString=true;
            else if (c=='[') stack.push_back(']'); else if (c=='{') stack.push_back('}');
            else if (c==']' || c=='}') { if (stack.back()!=c) return false; stack.pop_back(); }
        }
        return stack.empty();
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return true;
}
bool FindField(const std::string& s, size_t objectStart, const wchar_t* wanted, size_t& valueAt) {
    if (objectStart >= s.size() || s[objectStart] != '{') return false;
    size_t i = objectStart + 1;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n' || s[i] == ',')) ++i;
        if (i < s.size() && s[i] == '}') return false;
        std::wstring key; if (!ParseString(s, i, key)) return false;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) ++i;
        if (i >= s.size() || s[i++] != ':') return false;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) ++i;
        if (key == wanted) { valueAt = i; return true; }
        if (!SkipValue(s, i)) return false;
    }
    return false;
}
bool ParseUtf16Array(const std::string& s, size_t i, std::wstring& out) {
    if (i >= s.size() || s[i++] != '[') return false;
    out.clear();
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n' || s[i] == ',')) ++i;
        if (i < s.size() && s[i] == ']') return true;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        unsigned value = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            value = value * 10 + (unsigned)(s[i++] - '0');
            if (value > 0xffff) return false;
        }
        if (out.size() >= 32768) return false;
        out.push_back((wchar_t)value);
    }
    return false;
}
bool ParseHits(const std::string& json, std::vector<Hit>& hits) {
    size_t resultAt = 0, arrayAt = 0;
    size_t stateAt = 0;
    if (!FindField(json, 0, L"result", resultAt) ||
        !FindField(json, resultAt, L"stale", stateAt) || json.compare(stateAt,5,"false") != 0 ||
        !FindField(json, resultAt, L"truncated", stateAt) || json.compare(stateAt,5,"false") != 0 ||
        !FindField(json, resultAt, L"hits", arrayAt) || arrayAt >= json.size() || json[arrayAt] != '[') return false;
    size_t i = arrayAt + 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\r' || json[i] == '\n' || json[i] == ',')) ++i;
        if (i < json.size() && json[i] == ']') return true;
        if (i >= json.size() || json[i] != '{') return false;
        const size_t objectAt = i;
        Hit hit; bool hasPath = false, hasFolder = false; size_t valueAt = 0;
        if (FindField(json, objectAt, L"path", valueAt)) { if (!ParseString(json, valueAt, hit.path)) return false; hasPath = true; }
        if (FindField(json, objectAt, L"path_utf16", valueAt)) {
            std::wstring exact;
            if (!ParseUtf16Array(json, valueAt, exact) || exact.empty()) return false;
            hit.path = std::move(exact);
        }
        if (FindField(json, objectAt, L"is_folder", valueAt)) {
            if (json.compare(valueAt,4,"true")==0) hit.isFolder=true;
            else if (json.compare(valueAt,5,"false")==0) hit.isFolder=false;
            else return false;
            hasFolder = true;
        }
        if (FindField(json, objectAt, L"workshop", valueAt)) {
            if (json.compare(valueAt,4,"true")==0) hit.workshop=true;
            else if (json.compare(valueAt,5,"false")==0) hit.workshop=false;
            else return false;
        }
        if (!hasPath || !hasFolder) return false;
        if (!SkipValue(json, i)) return false;
        hits.push_back(std::move(hit));
    }
    return false;
}
} // namespace

bool ParseFlowtaryIndexReply(const std::string& json, std::vector<Hit>& out) {
    out.clear();
    if (!ParseHits(json, out)) { out.clear(); return false; }
    return true;
}
bool FlowtaryIndexAvailable() {
    std::lock_guard<std::mutex> lock(childMutex);
    if (child.servicePipe || (child.process && WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0)) return true;
#if !defined(FLOWTARY_TEST_SNAPSHOT_OVERRIDE)
    if (WaitNamedPipeW(kIndexerPipe, 0)) return true;
#endif
    const auto snapshot = SnapshotPath();
    const auto dir = ModuleDir();
    const auto helper = dir.empty() ? std::wstring() : dir + L"\\flowtary-indexer.exe";
    return !snapshot.empty() && !helper.empty() &&
        GetFileAttributesW(snapshot.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(helper.c_str()) != INVALID_FILE_ATTRIBUTES;
}
#if defined(FLOWTARY_TEST_SNAPSHOT_OVERRIDE)
unsigned long long FlowtaryIndexTestChildCpu100ns() {
    std::lock_guard<std::mutex> lock(childMutex);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!child.process || !GetProcessTimes(child.process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}
unsigned long FlowtaryIndexTestChildPid() {
    std::lock_guard<std::mutex> lock(childMutex);
    return child.process ? GetProcessId(child.process) : 0;
}
#endif
bool FlowtaryIndexPrewarm() {
    std::lock_guard<std::mutex> lock(childMutex);
    return StartChild() && EnsureChildReady(15'000);
}
int FlowtaryIndexQuery(const Query& query, bool pathMode, std::vector<Hit>& out) {
    out.clear(); std::lock_guard<std::mutex> lock(childMutex);
    if (!StartChild() || !EnsureChildReady(15'000) ||
        (!child.servicePipe && (!child.process || WaitForSingleObject(child.process, 0) == WAIT_OBJECT_0))) return -1;
    std::vector<std::wstring> pathWords = query.words;
    std::wstring pathExtension;
    if (pathMode && query.isFile) {
        for (auto& word : pathWords) {
            const size_t separator = word.find_last_of(L"\\/");
            if (separator == std::wstring::npos || separator + 3 >= word.size() ||
                word[separator + 1] != L'*' || word[separator + 2] != L'.') continue;
            const std::wstring extension = word.substr(separator + 3);
            if (extension.empty() || extension.find_first_of(L"\\/*?\"<>|:") != std::wstring::npos) return -1;
            if (!pathExtension.empty() && CompareStringOrdinal(pathExtension.c_str(), (int)pathExtension.size(),
                    extension.c_str(), (int)extension.size(), TRUE) != CSTR_EQUAL) return -1;
            pathExtension = extension;
            word.resize(separator + 1);
        }
    }
    std::string request = "{\"schema\":1,\"id\":1,\"query\":{\"words\":[";
    if (!pathMode) for (size_t i = 0; i < query.words.size(); ++i) {
        const std::string term = Utf8(query.words[i]);
        if (!query.words[i].empty() && term.empty()) return -1;
        if (i) request += ',';
        request += Quote(term);
    }
    request += "],\"folders_only\":"; request += query.isFile ? "false" : "true";
    if (!pathExtension.empty()) {
        const std::string extension = Utf8(pathExtension);
        if (extension.empty()) return -1;
        request += ",\"extension\":" + Quote(extension);
    }
    request += ",\"path_words\":[";
    if (pathMode) for (size_t i = 0; i < pathWords.size(); ++i) {
        const std::string term = Utf8(pathWords[i]);
        if (!pathWords[i].empty() && term.empty()) return -1;
        if (i) request += ',';
        request += Quote(term);
    }
    request += "],\"excludes\":[";
    for (size_t i = 0; i < query.excludes.size(); ++i) {
        const std::string term = Utf8(query.excludes[i]);
        if (!query.excludes[i].empty() && term.empty()) return -1;
        if (i) request += ',';
        request += Quote(term);
    }
    request += "],\"limit\":10,\"budget_ms\":25}}\n";
    std::string response;
    if (request.size() > 64 * 1024 || !WriteLine(child.input, request) ||
        !ReadLine(child.output, child.process, response, 1'500) || !ParseHits(response, out)) {
        out.clear(); child.Stop(); return -1;
    }
    return (int)out.size();
}
} // namespace df
