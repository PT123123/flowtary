// =====================================================================
// Wrapper header for Ling::Util
//
// 问题：原始 Util.h 中的 COMPILE_TIME_RAND_STR 宏使用了 C++20 template
//       lambda ([]<size_t... I>)，MSVC 19.44 不完全支持，触发 C3493 错误。
//
// 解决：include 原始头文件后 #undef 有问题的宏，用更简单的实现替换。
//       原始宏在编译期生成随机字符串，替换为运行时生成，功能等价。
// =====================================================================

// 先 include 原始头文件（会定义 COMPILE_TIME_RAND_STR）
// 注意：这里必须用完整路径，避免递归 include 到自己
#include "../../vendor/Ling/include/Util.h"

// 取消有问题的宏定义
#undef COMPILE_TIME_RAND_STR

// 用运行时版本替换（功能等价，只是不再编译期生成）
#define COMPILE_TIME_RAND_STR(LEN) \
    []() -> std::wstring { \
        static const wchar_t chars[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"; \
        static const size_t charsLen = sizeof(chars) / sizeof(chars[0]) - 1; \
        std::wstring result = L"Ling_"; \
        static uint64_t seed = static_cast<uint64_t>(__LINE__) * 31 + __COUNTER__ * 17; \
        for (size_t i = 0; i < LEN; ++i) { \
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; \
            result += chars[(seed >> 3) % charsLen]; \
        } \
        return result; \
    }()
