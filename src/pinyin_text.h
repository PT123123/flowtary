#pragma once
#include <string>
#include <string_view>

namespace ft {
// cpp-pinyin rejects UTF-16 surrogate pairs. Keep the original filename in the index,
// but omit non-BMP code points (and malformed surrogate units) from the phonetic key.
inline std::wstring PinyinBmpText(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        if ((c >= 0xD800 && c <= 0xDFFF) || static_cast<unsigned int>(c) > 0xFFFF) continue;
        out.push_back(c);
    }
    return out;
}
}
