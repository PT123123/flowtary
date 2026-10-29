// ============================================================
// app_ver.h — 「<应用名>-<版本号>」发布目录的版本解析（纯头文件，无项目依赖）
//   发布目录布局（workshop / software 等）：aura-1.2.25、Flowtary-1.1.23.10、
//   TaskbarX_1.7.8.0_x86，外加同层快照副本 aura-1.2.25-backup-20261028-1755。
//   搜索结果同分时要用它把「该应用最新版」抬到最前、快照沉后，所以解析规则
//   必须与界面排序一致——主程序与冒烟测试都包含本文件，规则只写一遍。
// ============================================================
#pragma once

#include <cwctype>
#include <string>
#include <unordered_map>

namespace appver {

struct Info {
    bool has = false;
    std::wstring app;      // 分组键 = 版本段的父目录 + 版本段去掉版本号（不同应用不互相压制）
    unsigned long long ver = 0;   // 四段各 16 位打包，"1.2" == "1.2.0.0"
    bool plain = true;     // 版本号后面还挂后缀（-backup-20261028…）= 快照/变体
};

inline std::wstring Lower(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) o += (wchar_t)towlower(c);
    return o;
}

// 段名里最左侧的「点分版本号」（至少两段数字）。要求版本号起于段首或分隔符之后，
// 且后面不紧跟字母数字——排除 HWiNFO64（名字里带数字）与 9.0c（尾巴粘着字母）；
// 纯数字段（20261028 这类日期）没有点，也不算版本。允许 v/V 前缀（CapsWriter-v2.0.4）。
inline bool SegVersion(const std::wstring& seg, size_t& vStart, size_t& vEnd) {
    for (size_t i = 0; i < seg.size(); ++i) {
        if (seg[i] < L'0' || seg[i] > L'9') continue;
        if (i) {
            const wchar_t prev = seg[i - 1];
            const bool vp = (prev == L'v' || prev == L'V');
            const bool sep = !iswalnum(prev) || prev == L'.';
            if (!sep) {
                if (!vp) continue;
                if (i > 1 && (iswalnum(seg[i - 2]) || seg[i - 2] == L'.')) continue;   // "nov2.0" 不算
            }
        }
        size_t j = i;
        int dots = 0;
        while (true) {
            size_t d = j;
            while (d < seg.size() && seg[d] >= L'0' && seg[d] <= L'9') ++d;
            if (d == j) break;
            if (d < seg.size() && seg[d] == L'.') { j = d + 1; ++dots; continue; }
            j = d;
            break;
        }
        if (dots == 0) continue;
        if (j < seg.size() && iswalnum(seg[j])) continue;
        vStart = i;   // 'v' 前缀留在名字里（分组键 = "capswriter-offline-gui-v"），版本号只取数字
        vEnd = j;
        return true;
    }
    return false;
}

inline unsigned long long Pack(const std::wstring& s) {
    unsigned long long f[4] = {0, 0, 0, 0};
    int part = 0;
    size_t i = 0;
    while (i < s.size() && part < 4) {
        unsigned long long num = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') { num = num * 10 + (s[i] - L'0'); ++i; }
        f[part++] = num > 0xFFFF ? 0xFFFF : num;
        if (i < s.size() && s[i] == L'.') ++i; else break;
    }
    return (f[0] << 48) | (f[1] << 32) | (f[2] << 16) | f[3];
}

// 从最深层目录往上找第一个带版本号的段（文件名层不参与：同应用各版本的 exe 同名）。
// 最多上溯 4 层，覆盖 <app>-<ver>\dist\tool.exe 这类嵌套发布布局。
inline Info Parse(const std::wstring& path) {
    Info out;
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 3) return out;
    std::wstring dir = path.substr(0, slash);
    for (int depth = 0; depth < 4 && dir.size() > 2; ++depth) {
        size_t cut = dir.find_last_of(L'\\');
        std::wstring seg = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
        size_t vs = 0, ve = 0;
        if (SegVersion(seg, vs, ve)) {
            out.has = true;
            out.ver = Pack(seg.substr(vs, ve - vs));
            out.plain = ve == seg.size();
            std::wstring parent = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
            out.app = Lower(parent) + L"|" + Lower(seg.substr(0, vs));
            return out;
        }
        if (cut == std::wstring::npos) break;
        dir = dir.substr(0, cut);
    }
    return out;
}

// 一组候选行的版本档：先累计每个应用出现的最高版本，再给每行定档。
// 有正式版时以「最高正式版」为准（快照哪怕版本号更高也不算目标版本）。
struct Table {
    std::unordered_map<std::wstring, unsigned long long> bestPlain, bestAny;

    void Feed(const Info& v) {
        if (!v.has) return;
        auto a = bestAny.find(v.app);
        if (a == bestAny.end() || v.ver > a->second) bestAny[v.app] = v.ver;
        if (v.plain) {
            auto p = bestPlain.find(v.app);
            if (p == bestPlain.end() || v.ver > p->second) bestPlain[v.app] = v.ver;
        }
    }

    // 0=该应用最新版 1=路径里没有版本信息（或该应用未累计过）2=旧版本（含比最新版低的快照）
    int Rank(const Info& v) const {
        if (!v.has) return 1;
        auto p = bestPlain.find(v.app);
        if (p != bestPlain.end()) return v.ver >= p->second ? 0 : 2;
        auto a = bestAny.find(v.app);
        if (a == bestAny.end()) return 1;
        return v.ver >= a->second ? 0 : 2;
    }
};

}  // namespace appver
