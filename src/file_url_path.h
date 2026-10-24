#pragma once

#include <shlwapi.h>
#include <string>
#include <vector>

// Convert a local file URL (as returned by IWebBrowser2::get_LocationURL) to a
// Windows path. PathCreateFromUrlW also decodes escaped spaces and UTF-8 bytes.
inline std::wstring FileURLToPath(const std::wstring& url) {
    if (url.size() < 7 || _wcsnicmp(url.c_str(), L"file://", 7) != 0 ||
        url.size() >= MAXDWORD) {
        return {};
    }

    // Decoding can only shorten a URL, so its character count is a safe buffer
    // bound even when the URL contains percent-escaped UTF-8.
    std::vector<WCHAR> path(url.size() + 1, L'\0');
    DWORD pathChars = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(url.c_str(), path.data(), &pathChars, 0))) {
        return {};
    }
    return std::wstring(path.data());
}
