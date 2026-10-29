// 路径副标题显示取证：同一批真实深路径，同宽同字体下分别用 DT_PATH_ELLIPSIS（中段省略）
// 和 DT_END_ELLIPSIS（尾部截断）画进 DIB，存成 BMP 供肉眼确认「最深一层目录（版本号所在）」
// 是否保住。用法：path_ellipsis_probe <out.bmp> [rectWidthPx] [fontHeight]
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>

static void SaveBmp(const wchar_t* path, HBITMAP dib, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> bits((size_t)w * h * 4);
    if (GetDIBits(GetDC(nullptr), dib, 0, h, bits.data(), &bi, DIB_RGB_COLORS) == 0) {
        printf("GetDIBits failed\n");
        return;
    }
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;                  // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + (DWORD)bits.size();
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("open out failed\n"); return; }
    DWORD wr = 0;
    WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
    WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr);
    WriteFile(f, bits.data(), (DWORD)bits.size(), &wr, nullptr);
    CloseHandle(f);
}

int main() {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargc < 2) { printf("usage: path_ellipsis_probe <out.bmp> [width] [fontH]\n"); return 2; }
    const int room = wargc > 2 ? _wtoi(wargv[2]) : 260;
    const int fontH = wargc > 3 ? _wtoi(wargv[3]) : -14;
    // sub 行的实际形状：父目录全路径（最深一层目录里带版本号）
    std::vector<std::wstring> paths = {
        L"C:\\workshop\\aura-1.2.25",
        L"C:\\workshop\\aura-1.2.25-backup-20261028-1755",
        L"C:\\Users\\Public\\Desktop\\quire\\sandbox-20261017\\quire-desktop\\target\\debug",
        L"C:\\software\\CapsWriter-Offline-GUI-v2.0.4\\site-packages\\torch\\include\\ATen\\ops",
    };
    const int rowH = 26, pad = 8, labelW = 120;
    const int W = labelW + room + pad * 3;
    const int H = (int)paths.size() * rowH * 2 + pad;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP old = (HBITMAP)SelectObject(dc, dib);
    HBRUSH bg = CreateSolidBrush(RGB(250, 247, 240));
    RECT whole{0, 0, W, H};
    FillRect(dc, &whole, bg);
    DeleteObject(bg);
    HFONT font = CreateFontW(fontH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(90, 84, 76));

    int y = pad;
    for (const auto& p : paths) {
        for (int mode = 0; mode < 2; ++mode) {
            const wchar_t* lab = mode ? L"END" : L"PATH";
            RECT lr{pad, y, pad + labelW, y + rowH};
            DrawTextW(dc, lab, (int)wcslen(lab), &lr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT sr{pad + labelW, y, pad + labelW + room, y + rowH};
            HBRUSH box = CreateSolidBrush(RGB(236, 230, 220));
            FillRect(dc, &sr, box);
            DeleteObject(box);
            SetTextColor(dc, RGB(30, 28, 26));
            DrawTextW(dc, p.c_str(), (int)p.size(), &sr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                          (mode ? DT_END_ELLIPSIS : DT_PATH_ELLIPSIS));
            SetTextColor(dc, RGB(90, 84, 76));
            y += rowH;
        }
    }
    SaveBmp(wargv[1], dib, W, H);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    wprintf(L"ok  W=%d H=%d room=%d fontH=%d\n", W, H, room, fontH);
    return 0;
}
