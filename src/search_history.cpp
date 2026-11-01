#include "search_history.h"

#include <shlobj.h>

#include <cstring>
#include <cstdio>
#include <vector>

#include "settings_model.h"

namespace ft {
namespace hist {

// 单行化：控制符（含换行、制表）统一成空格，连续空格归一，超长截断。
// 台账一行一条记录，任何原样写进去的换行都会把后面所有行的对齐搞乱。
std::wstring OneLine(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() < kMaxFieldChars ? s.size() : kMaxFieldChars);
    bool prevSpace = false;
    for (size_t i = 0; i < s.size() && out.size() < kMaxFieldChars; ++i) {
        WCHAR c = s[i];
        bool sp = (c <= 0x20 || c == 0x3000);   // 0x3000：中文全角空格
        if (sp) {
            if (!prevSpace && !out.empty()) out += L' ';
            prevSpace = true;
            continue;
        }
        prevSpace = false;
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static std::wstring MsField(int ms) {
    return ms < 0 ? std::wstring(L"-") : std::to_wstring(ms) + L"ms";
}

std::wstring FormatQueryLine(const QueryLine& q) {
    std::wstring s = q.ts + L"  Q  [" + q.layer + L"] " + OneLine(q.query);
    s += L"  first=" + MsField(q.firstMs);
    s += L" total=" + MsField(q.totalMs);
    s += L" rows=" + std::to_wstring(q.rows);
    s += L"  src=索引" + std::to_wstring(q.daemonRows);
    s += L"/fd" + std::to_wstring(q.fdRows);
    s += L"/WS" + std::to_wstring(q.wsRows);
    s += L"  top=" + (q.top.empty() ? std::wstring(L"-") : OneLine(q.top));
    if (!q.missed.empty()) s += L"  未回=" + OneLine(q.missed);
    return s;
}

std::wstring FormatOpenLine(const std::wstring& ts, const std::wstring& layer,
                            const std::wstring& query, const std::wstring& path) {
    return ts + L"  O  [" + layer + L"] " + OneLine(query) + L"  -> " + OneLine(path);
}

std::wstring TakeTail(const std::wstring& text, size_t maxLines) {
    if (maxLines == 0 || text.empty()) return std::wstring();
    // 按 \n 切段并从尾部丢弃空段（文件以换行结尾时末段必为空）
    std::vector<std::wstring> lines;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find_first_of(L'\n', i);
        std::wstring seg = text.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
        if (!seg.empty() && seg.back() == L'\r') seg.pop_back();
        if (!seg.empty()) lines.push_back(std::move(seg));
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    size_t keep = lines.size() <= maxLines ? lines.size() : maxLines;
    std::wstring out;
    for (size_t k = lines.size() - keep; k < lines.size(); ++k) {
        out += lines[k];
        out += L'\n';
    }
    return out;
}

bool NeedTrim(size_t fileBytes, size_t maxLines) {
    if (maxLines == 0) return false;
    return fileBytes > maxLines * kAvgLineBytes;
}

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    WCHAR buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::wstring FilePath() { return FlowtaryDataDir() + L"\\search_history.log"; }

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring W16(const char* p, size_t n) {
    if (n == 0) return std::wstring();
    int c = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
    if (c <= 0) return std::wstring();
    std::wstring w((size_t)c, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, (int)n, w.data(), c);
    return w;
}

constexpr char kBom[] = "\xEF\xBB\xBF";
constexpr size_t kFileReadMax = 8u << 20;   // 台账超过这个大小就只读尾部，绝不整份吞进内存

// 整份读出为宽字符（去 BOM）。文件不存在返回 false。
static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    size_t len = (size_t)sz.QuadPart;
    size_t off = 0;
    if (len > kFileReadMax) {
        off = len - kFileReadMax;
        len = kFileReadMax;
    }
    if (off) {
        LARGE_INTEGER seek{};
        seek.QuadPart = (LONGLONG)off;
        SetFilePointerEx(f, seek, nullptr, FILE_BEGIN);
    }
    out.assign(len, '\0');
    DWORD rd = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)len, &rd, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    out.resize(rd);
    if (off == 0 && out.size() >= 3 && memcmp(out.data(), kBom, 3) == 0) out.erase(0, 3);
    if (off != 0) {   // 分块读的首行是半行：丢掉到第一个换行为止
        size_t nl = out.find('\n');
        if (nl == std::string::npos) out.clear();
        else out.erase(0, nl + 1);
    }
    return true;
}

static bool WriteAll(const std::wstring& path, const std::string& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(f);
    return ok && wr == bytes.size();
}

bool TrimTo(size_t maxLines) {
    std::string raw;
    if (!ReadAll(FilePath(), raw)) return false;
    // TakeTail 用 \n 拼接（侧车显示用），写回文件要换回 \r\n
    std::wstring tail = TakeTail(W16(raw.data(), raw.size()), maxLines);
    std::wstring crlf;
    crlf.reserve(tail.size() + 8);
    for (WCHAR c : tail) {
        if (c == L'\n') crlf += L"\r\n";
        else crlf += c;
    }
    std::string out = kBom;
    out += U8(crlf);
    return WriteAll(FilePath(), out);
}

bool Write(const std::wstring& line, size_t maxLines) {
    const std::wstring path = FilePath();
    // 多级建目录：CreateDirectoryW 只补一层，数据目录整体缺失时会静默写成失败
    SHCreateDirectoryExW(nullptr, FlowtaryDataDir().c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::string bytes;
    if (sz.QuadPart == 0) bytes += kBom;   // 新建文件补 BOM，编辑器与记事本才按 UTF-8 认
    bytes += U8(line);
    bytes += "\r\n";
    DWORD wr = 0;
    BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(f);
    if (!ok || wr != bytes.size()) return false;
    if (NeedTrim((size_t)sz.QuadPart + bytes.size(), maxLines)) TrimTo(maxLines);
    return true;
}

std::wstring ReadTail(size_t maxLines) {
    std::string raw;
    if (!ReadAll(FilePath(), raw)) return std::wstring();
    return TakeTail(W16(raw.data(), raw.size()), maxLines);
}

bool RemoveFile() { return DeleteFileW(FilePath().c_str()) != FALSE; }
}  // namespace hist
}  // namespace ft
