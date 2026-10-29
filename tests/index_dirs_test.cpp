// settings_model.cpp 的 IndexDirs 行解析门：编辑器原文 → 优先根列表。
// 规则必须与守护进程侧（tools/ft_search_main.cpp）一致：逐行、丢空白行、# 开头当注释、
// 两端去空格；顺序保持用户书写顺序（守护进程再整表反序插到池头，最后填的排最前）。
#include "settings_model.h"

#include <cstdio>
#include <string>
#include <vector>

static int Check(const wchar_t* text, const std::vector<std::wstring>& want) {
    auto got = ft::ParseIndexDirs(text);
    if (got.size() != want.size()) {
        wprintf(L"FAIL size=%zu want=%zu text=%s\n", got.size(), want.size(), text);
        return 1;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (got[i] != want[i]) {
            wprintf(L"FAIL [%zu] got=%s want=%s\n", i, got[i].c_str(), want[i].c_str());
            return 1;
        }
    }
    return 0;
}

int wmain() {
    int bad = 0;
    bad += Check(L"C:\\workshop\r\nC:\\software\r\n", {L"C:\\workshop", L"C:\\software"});
    bad += Check(L"# 自研应用发布目录\n  C:\\workshop  \n\nC:\\software\n",
                 {L"C:\\workshop", L"C:\\software"});
    bad += Check(L"", {});
    bad += Check(L"# 全是注释\n# 第二行\n", {});
    bad += Check(L"C:\\a\nC:\\b", {L"C:\\a", L"C:\\b"});   // 末行无换行符
    wprintf(bad ? L"index_dirs_test: FAIL\n" : L"index_dirs_test: OK\n");
    return bad ? 1 : 0;
}
