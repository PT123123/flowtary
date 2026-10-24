// ============================================================
// df_search.cpp — d/f 搜索后端实现（不建全盘索引）
//   ① Windows Search：纯 OLE DB 消费者（零 ATL、零第三方依赖）。
//      COM 每线程初始化一次且不反初始化；provider/会话进程级缓存，
//      常驻工作线程上后续查询毫秒级。
//   ② fd：单 exe 子进程，stdout 逐行解析；在途进程注册进 g_fdActive，
//      CancelAll() 统一终止，保证同时最多一个 fd 在遍历。
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#define DBINITCONSTANTS
#include <initguid.h>   // 让 oledb.h 的 DEFINE_GUID 在本编译单元实例化（仅此处引入）
#include <oledb.h>
#include <oledberr.h>

#include "df_search.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <cstdio>
#include <unordered_set>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace df {
namespace {

constexpr DWORD kWsProbeRows = 1;    // 可用性探测：拿到 1 行即可
constexpr DWORD kWsFetchRows = 400;  // 真实查询抓取上限（过滤/排序后只展示前 10）
constexpr size_t kFdMaxRows = 400;   // fd 同款抓取上限，读满即终止 fd

// ---------------- 通用小工具 ----------------
std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// SQL 字符串字面量转义：单引号翻倍。%/_ 保留原样——文件名里出现通配符的概率
// 远小于转义写错的风险，最坏也只是匹配面稍宽。
std::wstring SqlEscape(const std::wstring& w) {
    std::wstring o;
    o.reserve(w.size() + 8);
    for (wchar_t c : w) {
        if (c == L'\'') o += L"''";
        else o += c;
    }
    return o;
}

bool ContainsAny(const std::wstring& lower, const std::vector<std::wstring>& terms) {
    for (const auto& e : terms)
        if (!e.empty() && lower.find(e) != std::wstring::npos) return true;
    return false;
}

// Windows 命令行参数加引号：收尾引号前的连续反斜杠翻倍，否则 "C:\" 的 \"
// 会把收尾引号转义成字面量（C:\ 被解析成 C:"）——alt-search 分支踩过的同一个坑。
std::wstring QuoteArg(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    o += L'"';
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; bs = 0; continue; }
        if (bs) { o.append(bs, L'\\'); bs = 0; }
        o += c;
    }
    o.append(bs * 2, L'\\');
    o += L'"';
    return o;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring s(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), s.data(), n);
    return s;
}

// COM 接口守卫：析构 Release，错误路径不泄漏
template <class T>
struct CI {
    T* p = nullptr;
    T* operator->() const { return p; }
    bool ok() const { return p != nullptr; }
    ~CI() { if (p) p->Release(); }
};

// ---------------- Windows Search ----------------

// System.ItemUrl（"file:C:/Users/a/b.txt"）→ Windows 路径（含 %XX 解码）。
// file:///C:/ 剥三斜；file://server/share 是 UNC，保留双斜。
std::wstring ItemUrlToPath(const wchar_t* u) {
    if (!u || !*u) return L"";
    std::wstring s = u;
    if (_wcsnicmp(s.c_str(), L"file:", 5) == 0) s.erase(0, 5);
    if (s.size() > 4 && s[0] == L'/' && s[1] == L'/' && s[2] == L'/' &&
        iswalpha((wint_t)s[3]) && s[4] == L':')
        s.erase(0, 3);
    std::wstring o;
    o.reserve(s.size());
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { o += (wchar_t)((h << 4) | l); i += 2; continue; }
        }
        o += (s[i] == L'/') ? L'\\' : s[i];
    }
    return o;
}

// COM 每线程只初始化一次、永不主动反初始化：CoUninitialize 会把 MTA 引用计数
// 减到 0 并拆掉整个公寓，缓存里的 provider/会话指针随之失效（第二次查询即崩）。
// 调用方线程均为长命线程（flowtary 常驻 WS 工作线程 / wsquery 主线程），安全。
// fd 路径也需要它：SHGetFolderPathW 解析用户目录要 COM。
void EnsureThreadCom() {
    static thread_local bool t_comInit = false;
    if (!t_comInit) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hrInit)) t_comInit = true;
        // RPC_E_CHANGED_MODE：线程已是 STA——照常走（缓存调用线程不会是 STA 线程）
    }
}

// 进程级会话缓存：provider 初始化 + 命令工厂只建一次，后续查询毫秒级。
// 流程树上可能有两个调用线程（主程序常驻 WS 工作线程 / wsquery 主线程），互斥保护。
struct WsCache {
    std::mutex mu;
    IDBInitialize* init = nullptr;
    IDBCreateCommand* cmdFactory = nullptr;
    bool ready() const { return init && cmdFactory; }
    void reset() {
        if (cmdFactory) { cmdFactory->Release(); cmdFactory = nullptr; }
        if (init) { init->Release(); init = nullptr; }
    }
};
WsCache g_wsCache;

void WsStep(std::wstring* diag, const wchar_t* stage, HRESULT hr) {
    if (diag) {
        wchar_t buf[64];
        swprintf_s(buf, L"0x%08lX", (unsigned long)hr);
        *diag += std::wstring(stage) + L"=" + buf + L" ";
    }
}

// 建会话（调用方已持锁）。返回 false 时 *hrOut 为首个失败点。
bool WsBuildSession(std::wstring* diag, HRESULT* hrOut) {
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"Search.CollatorDSO", &clsid);
    WsStep(diag, L"CLSID", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IDBInitialize,
                          (void**)&g_wsCache.init);
    WsStep(diag, L"CREATE", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    // EXTENDED PROPERTIES="Application:Windows"（官方连接串同款）→ DBPROP_INIT_PROVIDERSTRING
    DBPROP prop{};
    prop.dwPropertyID = DBPROP_INIT_PROVIDERSTRING;
    prop.vValue.vt = VT_BSTR;
    prop.vValue.bstrVal = SysAllocString(L"Application:Windows");
    if (!prop.vValue.bstrVal) { *hrOut = E_OUTOFMEMORY; return false; }
    DBPROPSET set{};
    set.rgProperties = &prop;
    set.cProperties = 1;
    set.guidPropertySet = DBPROPSET_DBINIT;
    {
        CI<IDBProperties> props;
        bool setOk = SUCCEEDED(g_wsCache.init->QueryInterface(IID_IDBProperties, (void**)&props.p)) &&
                     SUCCEEDED(props->SetProperties(1, &set));
        VariantClear(&prop.vValue);
        if (!setOk) { WsStep(diag, L"PROP", E_FAIL); *hrOut = E_FAIL; return false; }
    }
    hr = g_wsCache.init->Initialize();
    WsStep(diag, L"INIT", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    CI<IDBCreateSession> sessFactory;
    if (FAILED(g_wsCache.init->QueryInterface(IID_IDBCreateSession, (void**)&sessFactory.p))) {
        WsStep(diag, L"QI", E_FAIL); *hrOut = E_FAIL; return false;
    }
    hr = sessFactory->CreateSession(nullptr, IID_IDBCreateCommand, (IUnknown**)&g_wsCache.cmdFactory);
    WsStep(diag, L"SESSION", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    return true;
}

// 统一查询入口。maxRows 限制抓取行数（探测用 1，真实查询 400）。
// 文件/文件夹判别放在客户端（System.ItemType == "Folder"）而非 SQL 的
// <> 'Folder'：无扩展名文件的 ItemType 可能为 NULL，三值逻辑会漏行。
// diag 非空时逐步记录失败点与 HRESULT（冒烟工具 wsquery --check 用）。
int WsQueryImpl(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows,
                std::wstring* diag) {
    out.clear();
    if (q.words.empty() && q.excludes.empty()) return 0;

    // ---- 组 SQL：逐词 AND（名字必含；路径模式放宽到 ItemUrl 整条路径）----
    std::wstring where;
    for (const auto& w : q.words) {
        std::wstring like = L" LIKE '%" + SqlEscape(w) + L"%'";
        std::wstring c = L"(System.FileName" + like;
        if (pathMode) c += L" OR System.ItemUrl" + like;
        c += L")";
        where += where.empty() ? c : L" AND " + c;
    }
    if (where.empty()) where = L"System.FileName LIKE '%'";  // 只有排除词：全量再排除
    for (const auto& e : q.excludes) {
        if (e.empty()) continue;
        std::wstring notlike = L" NOT LIKE '%" + SqlEscape(e) + L"%'";
        where += L" AND System.FileName" + notlike;
        if (pathMode) where += L" AND System.ItemUrl" + notlike;
    }
    // 官方示例均为裸标识符，不加双引号
    const std::wstring sql = L"SELECT System.ItemUrl, System.FileName, System.ItemType"
                             L" FROM SystemIndex WHERE " + where;

    // COM 每线程只初始化一次、永不主动反初始化（见 EnsureThreadCom 注释）
    EnsureThreadCom();
    int result = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {  // 会话失效（服务刚重启等）：重建后重试一次
        // 会话缓存：provider/会话只建一次；查询失败重建一次再试（服务重启等情况）
        std::unique_lock<std::mutex> lk(g_wsCache.mu);
        HRESULT hr = S_OK;
        HRESULT buildHr = S_OK;
        if (!g_wsCache.ready() && !WsBuildSession(diag, &buildHr)) break;
        CI<ICommandText> cmd;
        hr = g_wsCache.cmdFactory->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)&cmd.p);
        WsStep(diag, L"CMD", hr);
        if (FAILED(hr)) { g_wsCache.reset(); continue; }   // 重建会话再走一遍
        hr = cmd->SetCommandText(DBGUID_DBSQL, sql.c_str());
        WsStep(diag, L"SETTEXT", hr);
        if (FAILED(hr)) break;

        DBROWCOUNT affected = 0;
        CI<IUnknown> rsUnk;
        hr = cmd->Execute(nullptr, IID_IRowset, nullptr, &affected, &rsUnk.p);
        WsStep(diag, L"EXEC", hr);
        if (FAILED(hr)) {
            if (!diag) { g_wsCache.reset(); continue; }    // 静默重试一次（服务可能刚重启）
            break;
        }
        CI<IRowset> rs;
        if (FAILED(rsUnk->QueryInterface(IID_IRowset, (void**)&rs.p))) { WsStep(diag, L"RS", E_FAIL); break; }
        lk.unlock();

        // 列 ordinal 按名定位
        CI<IColumnsInfo> ci;
        if (FAILED(rsUnk->QueryInterface(IID_IColumnsInfo, (void**)&ci.p))) { WsStep(diag, L"CI", E_FAIL); break; }
        DBORDINAL cCols = 0;
        DBCOLUMNINFO* infos = nullptr;
        OLECHAR* namesBuf = nullptr;
        hr = ci->GetColumnInfo(&cCols, &infos, &namesBuf);
        WsStep(diag, L"COLINFO", hr);
        if (FAILED(hr)) break;
        DBORDINAL ordUrl = 0, ordName = 0, ordType = 0;
        for (DBORDINAL i = 0; i < cCols; ++i) {
            const wchar_t* nm = infos[i].pwszName;
            if (!nm) continue;
            if (_wcsicmp(nm, L"System.ItemUrl") == 0) ordUrl = infos[i].iOrdinal;
            else if (_wcsicmp(nm, L"System.FileName") == 0) ordName = infos[i].iOrdinal;
            else if (_wcsicmp(nm, L"System.ItemType") == 0) ordType = infos[i].iOrdinal;
        }
        CoTaskMemFree(infos);
        CoTaskMemFree(namesBuf);
        if (!ordUrl || !ordName || !ordType) { WsStep(diag, L"ORD", E_FAIL); break; }

        // 行缓冲：Search provider 的列值是 BYREF 宽字符串（服务端分配 wchar_t*，
        // ReleaseRows 前有效），绑定必须 DBTYPE_WSTR|DBTYPE_BYREF + cbMaxLen=0，
        // 固定长度 WSTR 拷贝会报 DB_E_BADBINDINFO。
        // 布局（字节）：url ptr[0,8) st[8,12) pad[12,16) | name ptr[16,24) st[24,28) pad
        //              | type ptr[32,40) st[40,44) → 共 48
        enum { OFF_URL = 0, OFF_URL_ST = 8, OFF_NAME = 16, OFF_NAME_ST = 24,
               OFF_TYPE = 32, OFF_TYPE_ST = 40, ROW_BUF = 48 };
        DBBINDING b[3]{};
        auto bind = [](DBBINDING& x, DBORDINAL ord, ULONG obValue, ULONG obSt) {
            x.iOrdinal = ord;
            x.obValue = obValue; x.obLength = obSt; x.obStatus = obSt;
            x.dwPart = DBPART_VALUE | DBPART_STATUS;
            x.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            x.eParamIO = DBPARAMIO_NOTPARAM;
            x.wType = DBTYPE_WSTR | DBTYPE_BYREF;
            x.cbMaxLen = 0;
        };
        bind(b[0], ordUrl, OFF_URL, OFF_URL_ST);
        bind(b[1], ordName, OFF_NAME, OFF_NAME_ST);
        bind(b[2], ordType, OFF_TYPE, OFF_TYPE_ST);

        CI<IAccessor> acc;
        if (FAILED(rs->QueryInterface(IID_IAccessor, (void**)&acc.p))) { WsStep(diag, L"ACCF", E_FAIL); break; }
        HACCESSOR hacc = 0;
        DBBINDSTATUS st[3]{};
        hr = acc->CreateAccessor(DBACCESSOR_ROWDATA, 3, b, 0, &hacc, st);
        WsStep(diag, L"ACC", hr);
        if (FAILED(hr)) break;

        unsigned char rowBuf[ROW_BUF];
        HROW* prow = nullptr;   // GetNextRows 出参：服务端填行句柄数组
        result = 0;
        for (;;) {
            DBCOUNTITEM got = 0;
            if (FAILED(rs->GetNextRows(DB_NULL_HCHAPTER, 0, 64, &got, &prow)) || got == 0) break;
            for (DBCOUNTITEM i = 0; i < got; ++i) {
                if (FAILED(rs->GetData(prow[i], hacc, rowBuf))) continue;
                // BYREF 绑定：值位是 wchar_t* 指针，ReleaseRows 前必须取走
                const wchar_t* url = *(wchar_t* const*)(rowBuf + OFF_URL);
                const wchar_t* name = *(wchar_t* const*)(rowBuf + OFF_NAME);
                const wchar_t* type = *(wchar_t* const*)(rowBuf + OFF_TYPE);
                auto colOk = [&](unsigned short statusOff) {
                    unsigned short s = *(unsigned short*)(rowBuf + statusOff);
                    return s == DBSTATUS_S_OK;
                };
                if (!colOk(OFF_URL_ST)) url = L"";
                if (!colOk(OFF_NAME_ST)) name = L"";
                if (!colOk(OFF_TYPE_ST)) type = L"";
                bool isFolder = _wcsicmp(type, L"Folder") == 0;
                if (q.isFile == isFolder) continue;  // f 排文件夹 / d 只要文件夹
                std::wstring path = ItemUrlToPath(url);
                if (path.empty()) path = name ? name : L"";
                if (path.empty()) continue;
                if (ContainsAny(ToLower(path), q.excludes)) continue;  // 客户端再兜一道
                out.push_back({path, isFolder});
                if (out.size() >= maxRows) break;
            }
            rs->ReleaseRows(got, prow, nullptr, nullptr, nullptr);
            if (out.size() >= maxRows) break;
        }
        if (hacc) acc->ReleaseAccessor(hacc, nullptr);
        result = (int)out.size();
        break;   // 成功：不再重试
    }

    return result;
}

// ---------------- fd ----------------

std::mutex g_fdMutex;
std::vector<HANDLE> g_fdActive;  // 在途 fd 进程句柄（CancelAll 终止；属主线程回收+注销）

}  // namespace

bool WindowsSearchAvailable() {
    // 探针要求真有行：索引为空（从未建过/被清空）的机器上按不可用处理，
    // 调用方据此跳过 WS 后端（本机 fd 单后端），避免每条查询空跑一个线程
    Query q{};
    q.words.push_back(L"a");
    std::vector<Hit> tmp;
    return WsQueryImpl(q, false, tmp, kWsProbeRows, nullptr) > 0;
}

std::wstring DiagProbe() {
    Query q{};
    q.words.push_back(L"a");
    std::vector<Hit> tmp;
    std::wstring diag;
    WsQueryImpl(q, false, tmp, kWsProbeRows, &diag);
    return diag;
}

int WindowsSearchQuery(const Query& q, bool pathMode, std::vector<Hit>& out) {
    return WsQueryImpl(q, pathMode, out, kWsFetchRows, nullptr);
}

std::wstring FindFdExe() {
    WCHAR self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
        std::wstring dir(self);
        size_t p = dir.find_last_of(L"\\/");
        std::wstring cand = (p == std::wstring::npos ? dir : dir.substr(0, p + 1)) + L"fd.exe";
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    }
    WCHAR found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"fd.exe", nullptr, MAX_PATH, found, nullptr) > 0) return found;
    return L"";
}

void CancelAll() {
    std::vector<HANDLE> hs;
    {
        std::lock_guard<std::mutex> lk(g_fdMutex);
        hs.swap(g_fdActive);
    }
    for (HANDLE h : hs) TerminateProcess(h, 0);  // 已退出的进程上失败无害；回收仍由属主线程做
}

// ---------------- 内存索引（用户常用目录，毫秒级子串搜索） ----------------
// 存储为「每根一个路径池 + 偏移表」：路径连续存放（各自 \0 结尾），条目只记
// 偏移/名字偏移/是否目录——比 vector<wstring> 省一半以上内存（无逐串分配开销）。
// 增量更新 = 整根替换（fd 重列该根后整体 swap），无需跨根挪动下标。

struct MemRootData {
    std::wstring pool;               // 路径池（顺序拼接，每条自带 \0）
    std::vector<uint32_t> offs;      // 每条目路径在 pool 中的起始下标
    std::vector<uint32_t> nameFlags; // 每条目：低 31 位 = 文件名偏移，最高位 = isDir
    size_t size() const { return offs.size(); }
};

std::mutex g_memMu;
std::vector<MemRootData> g_memRoots;        // 与 g_memRootNames 下标一一对应
std::vector<std::wstring> g_memRootNames;   // 根目录列表（UserDirsRoots）
std::atomic<bool> g_memReady{false};
std::atomic<bool> g_memStarted{false};
std::atomic<bool> g_memQuit{false};
std::mutex g_dirtyMu;
std::condition_variable g_dirtyCv;
std::set<size_t> g_dirty;

DWORD MemCap() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_MEMINDEX_MAX", env, 16) > 0) {
        DWORD v = (DWORD)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return 300000;
}

// 大小写不敏感子串查找（朴素算法；16 万条短名 ≈ 每查询几毫秒，无需 SIMD）
const wchar_t* MemCaselessFind(const wchar_t* hay, const wchar_t* needle) {
    if (!*needle) return hay;
    for (; *hay; ++hay) {
        const wchar_t* h = hay;
        const wchar_t* n = needle;
        while (*h && *n && towlower(*h) == towlower(*n)) { ++h; ++n; }
        if (!*n) return hay;
        if (!*h) break;
    }
    return nullptr;
}

// 默认剪枝集（-E 透传给 fd，fd 对目录剪枝=整棵子树不进遍历）：
//  - 点开头（.git/.vscode/.cache/临时点文件）——仅内存索引启用（fd 搜索层由
//    客户端按 skipDotFolders 开关过滤，保持开关语义）
//  - 回收站/系统卷标目录（$RECYCLE.BIN、System Volume Information、RECYCLER）
//  - 临时目录与临时/备份文件（tmp、temp、*.tmp、*.temp、~$*、*~、*.swp）
//  - node_modules、__pycache__
// 与设置「排除路径」默认集同向：这些条目展示层本来就被过滤，剪掉纯省内存和遍历时间。
void AppendPruneArgs(std::wstring& args, bool includeDot) {
    const wchar_t* const prunes[] = {
        L"node_modules", L"__pycache__",
        L"$RECYCLE.BIN", L"System Volume Information", L"RECYCLER",
        L"tmp", L"temp",
        L"*.tmp", L"*.temp", L"~$*", L"*~", L"*.swp",
    };
    if (includeDot) args += L" -E " + QuoteArg(L".*");
    for (const wchar_t* p : prunes) args += L" -E " + QuoteArg(p);
}

void MemPoolAdd(MemRootData& rd, const std::wstring& path, bool isDir) {
    const uint32_t off = (uint32_t)rd.pool.size();
    rd.pool.append(path);
    rd.pool.push_back(L'\0');
    size_t nameOff = path.find_last_of(L'\\');
    nameOff = nameOff == std::wstring::npos ? 0 : nameOff + 1;
    rd.offs.push_back(off);
    rd.nameFlags.push_back((uint32_t)nameOff | (isDir ? 0x80000000u : 0u));
}

// 索引缓存落盘（UTF-8 文本：首行版本标记，R\t根目录 为分节标记，D/F\tpath 为条目）。
// 作用：重启时毫秒级加载立即可用（可能略旧，后台全量重建后覆盖），
// 100s 级的建库成本只在首次运行（或剪枝规则变更后）支付。
std::wstring MemCachePath() {
    EnsureThreadCom();
    wchar_t p[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, p)))
        return L"";
    std::wstring dir = std::wstring(p) + L"\\Flowtary";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\memindex.dat";
}
// 持 g_memMu 调用。分块写文件（1MB 缓冲）——不整串拼接，避免 20MB×2 的堆峰值驻留
void MemIndexSaveLocked() {
    std::wstring path = MemCachePath();
    if (path.empty()) return;
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    auto writeAll = [&](const char* p, DWORD n) {
        while (n > 0) {
            DWORD w = 0;
            if (!WriteFile(h, p, n, &w, nullptr) || w == 0) return false;
            p += w;
            n -= w;
        }
        return true;
    };
    bool ok = writeAll("FTMEMINDEX3\n", 12);   // 格式版本标记：规则/存储变更时旧缓存自动失效
    std::string chunk;
    chunk.reserve(1024 * 1024);
    for (size_t i = 0; ok && i < g_memRoots.size() && i < g_memRootNames.size(); ++i) {
        chunk += "R\t";
        chunk += WideToUtf8(g_memRootNames[i]);
        chunk += "\n";
        const MemRootData& rd = g_memRoots[i];
        for (size_t k = 0; ok && k < rd.offs.size(); ++k) {
            const wchar_t* path = rd.pool.c_str() + rd.offs[k];
            chunk += (rd.nameFlags[k] & 0x80000000u) ? "D\t" : "F\t";
            chunk += WideToUtf8(path);
            chunk += "\n";
            if (chunk.size() >= 1024 * 1024) {
                ok = writeAll(chunk.data(), (DWORD)chunk.size());
                chunk.clear();
            }
        }
        if (ok && !chunk.empty()) {
            ok = writeAll(chunk.data(), (DWORD)chunk.size());
            chunk.clear();
        }
    }
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}
// 持 g_memMu 调用；成功时按 g_memRootNames 对齐填充 g_memRoots（缓存多出的根丢弃）
bool MemIndexLoadLocked() {
    std::wstring path = MemCachePath();
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 256 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string data(size, '\0');
    DWORD rd = 0;
    ReadFile(h, data.data(), size, &rd, nullptr);
    CloseHandle(h);

    std::unordered_map<std::wstring, MemRootData> byRoot;   // 小写根 → 条目
    size_t pos = 0;
    bool versionOk = false;
    std::wstring curRoot;
    auto nextLine = [&](std::string& line) -> bool {
        if (pos >= data.size()) return false;
        size_t p = data.find('\n', pos);
        if (p == std::string::npos) { line = data.substr(pos); pos = data.size(); }
        else { line = data.substr(pos, p - pos); pos = p + 1; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    std::string line;
    while (nextLine(line)) {
        if (!versionOk) {
            versionOk = line == "FTMEMINDEX3";   // 版本不符（旧规则缓存）→ 弃用重建
            if (!versionOk) break;
            continue;
        }
        if (line.size() < 2) continue;
        if (line[0] == 'R' && line[1] == '\t') {
            curRoot = ToLower(Utf8ToWide(line.substr(2)));
            continue;
        }
        const bool isDir = line[0] == 'D';
        if (line[1] != '\t' || curRoot.empty()) continue;
        std::wstring path = Utf8ToWide(line.substr(2));
        if (path.size() < 2) continue;
        MemPoolAdd(byRoot[curRoot], path, isDir);
    }
    if (byRoot.empty()) return false;
    size_t total = 0;
    for (const auto& name : g_memRootNames) {
        auto it = byRoot.find(ToLower(name));
        if (it != byRoot.end()) total += it->second.size();
    }
    if (total == 0) return false;
    g_memRoots.clear();
    g_memRoots.reserve(g_memRootNames.size());
    for (const auto& name : g_memRootNames) {   // 按当前根顺序对齐
        auto it = byRoot.find(ToLower(name));
        if (it != byRoot.end()) g_memRoots.push_back(std::move(it->second));
        else g_memRoots.push_back(MemRootData{});
    }
    return true;
}

// fd 全列一个根（模式 . 匹配任意非空名）。剪枝见 AppendPruneArgs；
// 不带 -I（尊重 .gitignore，build/dist 等未忽略产物仍入索引）。
// fd 子进程运行并按行收集原始输出（UTF-8、正斜杠）。两趟式建库的公共底座：
// 不在解析循环里调 GetFileAttributesW——个别特殊文件名会让它挂死（Defender 环境）。
bool FdCaptureLines(const std::wstring& fdExe, const std::wstring& args, DWORD deadlineMs,
                    std::vector<std::string>& lines) {
    lines.clear();
    std::wstring cmd = QuoteArg(fdExe) + args;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hrd = nullptr, wr = nullptr;
    if (!CreatePipe(&hrd, &wr, &sa, 0)) return false;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;   // CreateProcessW 会改写命令行缓冲：必须传可写副本（const 参数的 data() 只读，强转会崩）
    BOOL ok = CreateProcessW(fdExe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(hrd); return false; }
    CloseHandle(pi.hThread);
    SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);   // 略低于前台：IDLE 会被等待中的普通优先级线程饿死（实测 3.7s 手动 vs 10min 超时）

    char buf[262144];
    std::string acc;
    const ULONGLONG start = GetTickCount64();
    for (;;) {   // PeekNamedPipe 轮询 + 兜底超时（空闲优先级走完全盘可达分钟级）
        DWORD avail = 0;
        if (!PeekNamedPipe(hrd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            DWORD nread = 0;
            if (!ReadFile(hrd, buf, want, &nread, nullptr) || nread == 0) break;
            acc.append(buf, nread);
            size_t p;
            while ((p = acc.find('\n')) != std::string::npos) {
                std::string ln = acc.substr(0, p);
                acc.erase(0, p + 1);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                if (ln.empty()) continue;
                lines.push_back(std::move(ln));
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(hrd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        } else if (deadlineMs && GetTickCount64() - start > deadlineMs) {
            TerminateProcess(pi.hProcess, 0);
            break;
        }
    }
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(hrd);
    return true;
}

// fd 两趟全列一个根（先目录后文件），条目进池。剪枝见 AppendPruneArgs；
// 不带 -I（尊重 .gitignore，build/dist 等未忽略产物仍入索引）。
bool MemListRootFd(const std::wstring& fdExe, const std::wstring& root, size_t cap,
                   MemRootData& rd) {
    rd = MemRootData{};
    // 注意顺序：-E 剪枝必须在「-- 根」之前（-- 之后的参数都是位置参数，不再是选项）
    for (int pass = 0; pass < 2; ++pass) {
        std::wstring args = L" -a -H --max-results " + std::to_wstring(cap);
        AppendPruneArgs(args, /*includeDot=*/true);
        args += pass == 0 ? L" -t d" : L" -t f";
        args += L" . -- " + QuoteArg(root);
        std::vector<std::string> lines;
        if (!FdCaptureLines(fdExe, args, 120000, lines)) return false;
        for (const auto& ln : lines) {
            std::wstring path = Utf8ToWide(ln);
            for (auto& c : path)
                if (c == L'/') c = L'\\';
            if (path.size() > 3 && path.back() == L'\\') path.pop_back();
            if (path.size() < 2) continue;
            MemPoolAdd(rd, path, pass == 0);
        }
    }
    return true;
}
void MemMarkDirty(size_t idx) {
    {
        std::lock_guard<std::mutex> lk(g_dirtyMu);
        g_dirty.insert(idx);
    }
    g_dirtyCv.notify_one();
}

// 每个用户目录一个监听线程：阻塞在 ReadDirectoryChangesW 上（内核等待，零 CPU），
// 任何名字变更（增/删/改名）都把该根标脏，由维护线程防抖后增量重建
struct MemWatchArg {
    size_t idx;
    std::wstring root;
};
unsigned __stdcall MemWatchThread(void* p) {
    std::unique_ptr<MemWatchArg> arg((MemWatchArg*)p);
    const size_t idx = arg->idx;
    const std::wstring root = std::move(arg->root);
    for (;;) {
        if (g_memQuit) break;
        HANDLE h = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {   // 目录暂不可见（U 盘等）：等待重试
            Sleep(5000);
            continue;
        }
        char buf[65536];
        DWORD bytes = 0;
        for (;;) {
            if (g_memQuit) break;
            if (!ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                       &bytes, nullptr, nullptr))
                break;
            // 只要有事件就标脏（防抖由维护线程做）；缓冲区溢出同样整体标脏
            MemMarkDirty(idx);
            break;
        }
        CloseHandle(h);
        if (g_memQuit) break;
        Sleep(200);   // 目录句柄失效（重命名/移动）后稍等重挂
    }
    return 0;
}

// 维护线程：防抖 1.5s 后按根整根重建（fd 重列该根 → 整根 swap，无下标挪动）。
// 重扫一根可达分钟级（Defender），30s 内不重复扫同一根（脏标记留到下轮）。
unsigned __stdcall MemMaintThread(void*) {
    std::wstring fdExe = FindFdExe();
    std::unordered_map<size_t, ULONGLONG> lastWalk;
    for (;;) {
        std::set<size_t> dirty;
        {
            std::unique_lock<std::mutex> lk(g_dirtyMu);
            g_dirtyCv.wait(lk, [] { return !g_dirty.empty() || g_memQuit.load(); });
            if (g_memQuit) break;
            dirty.swap(g_dirty);
        }
        Sleep(1500);
        if (g_memQuit || fdExe.empty()) continue;
        std::set<size_t> deferred;
        const ULONGLONG now = GetTickCount64();
        for (size_t idx : dirty) {
            auto it = lastWalk.find(idx);
            if (it != lastWalk.end() && now - it->second < 30000) {
                deferred.insert(idx);   // 刚扫过：推迟，避免目录高频变更时反复全扫
                continue;
            }
            lastWalk[idx] = now;
            if (idx >= g_memRootNames.size()) continue;
            MemRootData part;
            MemListRootFd(fdExe, g_memRootNames[idx], MemCap(), part);   // 不持锁（可达分钟级）
            {
                std::lock_guard<std::mutex> lk(g_memMu);
                if (idx < g_memRoots.size()) {
                    part.pool.shrink_to_fit();
                    part.offs.shrink_to_fit();
                    part.nameFlags.shrink_to_fit();
                    g_memRoots[idx] = std::move(part);
                    MemIndexSaveLocked();
                }
            }
        }
        if (!deferred.empty()) {
            std::lock_guard<std::mutex> lk(g_dirtyMu);
            g_dirty.insert(deferred.begin(), deferred.end());   // 等冷却期过再扫（wait 谓词会自查）
        }
    }
    return 0;
}

// 建库线程：先加载磁盘缓存（重启即毫秒级可用），后台逐根重建——每根扫完立即换入
// （不同时持有两份全量池，峰值内存 ≈ 全量池 + 单根）
unsigned __stdcall MemBuildThread(void*) {
    {
        std::lock_guard<std::mutex> lk(g_memMu);
        if (MemIndexLoadLocked()) g_memReady = true;   // 缓存可能略旧：先用，逐根刷新
    }
    std::wstring fdExe = FindFdExe();
    size_t total = 0;
    if (!fdExe.empty()) {
        const size_t cap = MemCap();
        for (size_t i = 0; i < g_memRootNames.size() && total < cap; ++i) {
            if (g_memQuit) break;
            MemRootData part;
            MemListRootFd(fdExe, g_memRootNames[i], cap - total, part);
            total += part.size();
            {
                std::lock_guard<std::mutex> lk(g_memMu);
                part.pool.shrink_to_fit();
                part.offs.shrink_to_fit();
                part.nameFlags.shrink_to_fit();
                if (i < g_memRoots.size()) g_memRoots[i] = std::move(part);
                else g_memRoots.push_back(std::move(part));
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_memMu);
            MemIndexSaveLocked();
        }
    }
    g_memReady = true;
    return 0;
}

void MemIndexStart(const std::vector<std::wstring>& extraRoots) {
    bool expect = false;
    if (!g_memStarted.compare_exchange_strong(expect, true)) return;
    if (GetEnvironmentVariableW(L"FT_DF_MEMINDEX_OFF", nullptr, 0) > 0) {
        g_memReady = true;   // 显式关闭：索引恒空，调度链直接走 fd/WS
        return;
    }
    g_memRootNames = UserDirsRoots();
    for (const auto& extra : extraRoots) {   // 注册表 IndexDirs 配置的项目目录等
        if (extra.size() < 3 || extra[1] != L':') continue;
        wchar_t driveRoot[4] = { extra[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(driveRoot) != DRIVE_FIXED) continue;
        bool dup = false;
        for (const auto& e : g_memRootNames)
            if (_wcsicmp(e.c_str(), extra.c_str()) == 0) { dup = true; break; }
        if (!dup) g_memRootNames.push_back(extra);
    }
    for (size_t i = 0; i < g_memRootNames.size(); ++i) {
        auto* arg = new MemWatchArg{i, g_memRootNames[i]};
        CloseHandle((HANDLE)_beginthreadex(nullptr, 0, MemWatchThread, arg, 0, nullptr));
    }
    CloseHandle((HANDLE)_beginthreadex(nullptr, 0, MemMaintThread, nullptr, 0, nullptr));
    CloseHandle((HANDLE)_beginthreadex(nullptr, 0, MemBuildThread, nullptr, 0, nullptr));
}

bool MemIndexReady() { return g_memReady.load(); }

size_t MemIndexSize() {
    std::lock_guard<std::mutex> lk(g_memMu);
    size_t n = 0;
    for (const auto& rd : g_memRoots) n += rd.size();
    return n;
}

int MemIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows) {
    out.clear();
    if (!g_memReady.load()) return -1;
    std::vector<std::wstring> wl;
    wl.reserve(q.words.size());
    for (const auto& w : q.words) wl.push_back(ToLower(w));
    std::lock_guard<std::mutex> lk(g_memMu);
    for (const auto& rd : g_memRoots) {
        for (size_t k = 0; k < rd.offs.size(); ++k) {
            const wchar_t* path = rd.pool.c_str() + rd.offs[k];
            const uint32_t nf = rd.nameFlags[k];
            const bool isDir = (nf & 0x80000000u) != 0;
            if (q.isFile == isDir) continue;   // f 排文件夹 / d 只要文件夹
            const wchar_t* name = path + (nf & 0x7FFFFFFFu);
            bool ok = true;
            if (pathMode) {
                for (const auto& w : wl)
                    if (!MemCaselessFind(path, w.c_str())) { ok = false; break; }
            } else {
                for (const auto& w : wl)
                    if (!MemCaselessFind(name, w.c_str())) { ok = false; break; }
            }
            if (!ok) continue;
            if (!q.excludes.empty() && ContainsAny(ToLower(path), q.excludes)) continue;
            out.push_back({path, isDir});
            if (out.size() >= maxRows) break;
        }
        if (out.size() >= maxRows) break;
    }
    return (int)out.size();
}
DWORD DefaultFdDeadlineMs() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_FD_TIMEOUT_MS", env, 16) > 0) {
        DWORD v = (DWORD)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return 8000;  // 默认 8s 预算（超时后调用方跳过路径兜底，最坏 ≤8s）
}

// 用户常用目录根（桌面/文档/下载/图片/音乐/视频）
std::vector<std::wstring> UserDirsRoots() {
    EnsureThreadCom();
    std::vector<std::wstring> roots;
    auto addRoot = [&](const std::wstring& r) {
        if (r.size() < 3 || r[1] != L':') return;
        // GetDriveTypeW 对非根路径行为不可靠（实测返回 DRIVE_REMOVABLE），
        // 必须先截出 "C:\" 盘根再判类型
        wchar_t driveRoot[4] = { r[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(driveRoot) != DRIVE_FIXED) return;
        for (const auto& e : roots)
            if (_wcsicmp(e.c_str(), r.c_str()) == 0) return;
        roots.push_back(r);
    };
    const int csidls[] = {
        CSIDL_DESKTOPDIRECTORY, CSIDL_PERSONAL, CSIDL_MYPICTURES,
        CSIDL_MYMUSIC, CSIDL_MYVIDEO,
        0x000C,  // CSIDL_DOWNLOADS（老 SDK 无宏，值固定）
    };
    for (int csidl : csidls) {
        wchar_t p[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, p)))
            addRoot(p);
    }
    return roots;
}

// 全部固定盘根
std::vector<std::wstring> DriveRoots() {
    std::vector<std::wstring> roots;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) == DRIVE_FIXED) roots.push_back(root);
    }
    return roots;
}

int FdQuery(const std::wstring& fdExe, const Query& q, bool pathMode, bool skipHidden,
            std::vector<Hit>& out, DWORD deadlineMs, bool* timedOut,
            const std::function<void(std::vector<Hit>&&)>& onBatch,
            const std::vector<std::wstring>& rootsOverride) {
    out.clear();
    if (timedOut) *timedOut = false;
    EnsureThreadCom();   // SHGetFolderPathW 解析用户目录需要 COM
    // 搜索根：默认 = 用户常用目录（桌面/文档/下载/图片/音乐/视频）前置 + 全部固定盘。
    // fd 的共享遍历队列没有根优先级，大根会把小根的结果淹没到几十秒之后——
    // 需要严格保证常用区域先出结果时，调用方应分阶段调用
    //（rootsOverride=UserDirsRoots() 先行，再用 DriveRoots() 补全盘）。
    std::vector<std::wstring> roots = rootsOverride;
    if (roots.empty()) {
        roots = UserDirsRoots();
        for (const auto& r : DriveRoots()) {
            bool dup = false;
            for (const auto& e : roots)
                if (_wcsicmp(e.c_str(), r.c_str()) == 0) { dup = true; break; }
            if (!dup) roots.push_back(r);
        }
    }
    if (roots.empty()) return 0;

    // -a 绝对路径；-F 固定字符串（词里的正则元字符不炸）；-i 不分大小写；
    // -I 不理会 .gitignore/.ignore（build/dist 这类被 ignore 的目录也要能搜到，
    //    点开头过滤交给主程序客户端过滤，与 Everything 结果同规则）；
    // -t f/-t d 类型过滤；-p 路径级兜底；--max-results 命中即收工；
    // 多词 AND：首个词为位置模式，其余逐个 --and（fd 10 的裸位置多模式是 OR）；
    // 「--」之后全部是搜索根。
    std::wstring args = L" -a -F -i -I --max-results 400";
    if (!skipHidden) args += L" -H";
    if (pathMode) args += L" -p";
    args += q.isFile ? L" -t f" : L" -t d";
    AppendPruneArgs(args, /*includeDot=*/false);   // 回收站/系统/临时目录剪枝：提速全盘遍历
    for (size_t i = 0; i < q.words.size(); ++i) {
        if (i > 0) args += L" --and";
        args += L" " + QuoteArg(q.words[i]);
    }
    args += L" --";
    for (const auto& r : roots) args += L" " + QuoteArg(r);
    std::wstring cmd = QuoteArg(fdExe) + args;

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;   // 两端均可继承（sa.bInheritHandle=TRUE）
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;   // 子进程拿走可继承写端；父进程在 CreateProcess 后关闭
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(fdExe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);      // 自身不留写端：fd 退出后管道 EOF 可判定
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(g_fdMutex);
        g_fdActive.push_back(pi.hProcess);
    }

    char buf[16384];
    std::string acc;
    // 轮询读取（PeekNamedPipe + 短等待）：fd 全盘遍历在本机实测可达 100s+（Defender
    // 实时扫描拖慢每层目录枚举），等不了也不该等——超过预算就终止 fd，保留已读到的
    // 部分结果（新一轮查询/隐藏窗口会 CancelAll，不会残留）。onBatch 非空时按批次
    // 增量投递（≥50 条或每 400ms 一批），界面流式上屏不用等预算跑完。
    const ULONGLONG start = GetTickCount64();
    size_t flushed = 0;
    ULONGLONG lastFlush = 0;
    auto flush = [&]() {
        if (!onBatch || out.size() <= flushed) return;
        std::vector<Hit> batch(out.begin() + flushed, out.end());
        flushed = out.size();
        onBatch(std::move(batch));
        lastFlush = GetTickCount64();
    };
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // 管道断开 = fd 已退出
        if (avail > 0) {
            DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            DWORD nread = 0;
            if (!ReadFile(rd, buf, want, &nread, nullptr) || nread == 0) break;
            acc.append(buf, nread);
            size_t p;
            while (out.size() < kFdMaxRows && (p = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, p);
                acc.erase(0, p + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                std::wstring path = Utf8ToWide(line);
                for (auto& c : path)
                    if (c == L'/') c = L'\\';
                if (path.size() > 3 && path.back() == L'\\') path.pop_back();  // 统一去尾分隔符（盘根除外）
                if (path.empty()) continue;
                if (ContainsAny(ToLower(path), q.excludes)) continue;
                out.push_back({path, !q.isFile});
            }
            if (out.size() >= kFdMaxRows) { flush(); break; }
            if (onBatch && out.size() > flushed &&
                (out.size() - flushed >= 50 || GetTickCount64() - lastFlush >= 400))
                flush();
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            // fd 已退出：把尾部残留读完（可能尚有一包未到）
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        } else if (deadlineMs && GetTickCount64() - start >= deadlineMs) {
            if (timedOut) *timedOut = true;
            break;  // 超时：交由下方统一终止 fd，部分结果照常返回
        }
    }
    flush();

    TerminateProcess(pi.hProcess, 0);  // 读满上限提前停时管道里可能还有数据；对已退出进程无害
    WaitForSingleObject(pi.hProcess, 3000);
    // FT_DF_DEBUG=1：向 stderr 打印命令行与退出码（冒烟诊断用，正常运行零输出）
    {
        wchar_t dbg[8]{};
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", dbg, 8) > 0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            fwprintf(stderr, L"[fd] cmd=%ls\n[fd] exit=%lu hits=%zu\n",
                     cmd.c_str(), (unsigned long)code, out.size());
            fflush(stderr);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_fdMutex);
        for (size_t i = 0; i < g_fdActive.size(); ++i)
            if (g_fdActive[i] == pi.hProcess) { g_fdActive.erase(g_fdActive.begin() + i); break; }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return (int)out.size();
}

// ---------------- 磁盘索引（全盘覆盖，索引文件化 + mmap 扫描，进程私有内存近零） ----------------
// fd 全盘遍历一次（空闲优先级，数百秒）→ 小写化 UTF-8 路径 blob（数百 MB 磁盘文件）
// → MapViewOfFile 只读映射；查询 = 首字节 memchr 全 blob 扫描（页缓存暖态 20–40ms）。
// blob 是文件映射页：可被系统按需换出，不计入进程私有内存（plocate 思路的简化版）。
// 新鲜度：每盘根 ReadDirectoryChangesW → RAM 内小 delta 表（增/删），查询时合并；
// delta 超 5 万条或缓存超 7 天自动后台重建（空闲优先级）。FT_DF_DISKINDEX_OFF=1 关闭。

HANDLE g_diFile = nullptr, g_diMap = nullptr;
const unsigned char* g_diView = nullptr;   // 记录区起始（跳过头部）
unsigned long long g_diSize = 0;           // 记录区字节数
unsigned long long g_diDataOff = 0;        // 头部之后的记录区起始偏移
unsigned long long g_diRecords = 0;
std::atomic<bool> g_diReady{false};
std::mutex g_diMu;                         // 保护 view/map 换入换出（搜索期间持有）
std::atomic<bool> g_diStarted{false};
std::atomic<bool> g_diQuit{false};
std::atomic<bool> g_diRebuilding{false};
std::mutex g_ovMu;
std::unordered_map<std::wstring, bool> g_ovAdd;   // 建库后新增：路径 → isDir
std::unordered_set<std::wstring> g_ovDel;         // 建库后删除

DWORD DiskCap() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_DISKINDEX_MAX", env, 16) > 0) {
        DWORD v = (DWORD)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return 8000000;
}

std::wstring DiskCachePath() {
    EnsureThreadCom();
    wchar_t p[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, p)))
        return L"";
    std::wstring dir = std::wstring(p) + L"\\Flowtary";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\diskindex.dat";
}

// blob 内路径已小写化（ASCII 折叠，非 ASCII 原样）——这里只做 ASCII 折叠
std::string Utf8LowerAscii(const std::string& s) {
    std::string o = s;
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return o;
}

// 朴素 + memchr 跳跃的字节子串查找（blob 已小写化，逐字节精确比较）
bool MemContainsBytes(const unsigned char* hay, size_t hl, const char* nd, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    const unsigned char c0 = (unsigned char)nd[0];
    for (size_t i = 0; i + nl <= hl; ++i) {
        if (hay[i] != c0) {
            if (i + 1 + nl - 1 > hl) return false;
            const unsigned char* f = (const unsigned char*)memchr(hay + i + 1, c0, hl - (i + 1) - (nl - 1) + 1);
            if (!f) return false;
            i = (size_t)(f - hay) - 1;   // for ++i 后指向 f
            continue;
        }
        if (nl == 1 || memcmp(hay + i + 1, nd + 1, nl - 1) == 0) return true;
    }
    return false;
}

// 映射已有索引文件（校验版本头 + 解析 COUNT），成功置 ready
bool DiskMapExistingLocked() {
    auto dbg2 = [](const wchar_t* m, unsigned long long v) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
            wchar_t b[160]{};
            swprintf(b, 160, L"[map] %s %llu", m, (unsigned long long)v);
            fwprintf(stderr, L"%ls\n", b);
            fflush(stderr);
        }
    };
    dbg2(L"enter, file size:", 0);
    if (g_diView) { UnmapViewOfFile(g_diView); g_diView = nullptr; }
    if (g_diMap) { CloseHandle(g_diMap); g_diMap = nullptr; }
    if (g_diFile) { CloseHandle(g_diFile); g_diFile = nullptr; }
    g_diReady = false;
    std::wstring path = DiskCachePath();
    if (path.empty()) { dbg2(L"no path:", 0); return false; }
    g_diFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_diFile == INVALID_HANDLE_VALUE) {
        dbg2(L"open failed:", GetLastError());
        g_diFile = nullptr;
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(g_diFile, &sz) || sz.QuadPart < 64) {
        dbg2(L"size bad:", (unsigned long long)sz.QuadPart);
        CloseHandle(g_diFile);
        g_diFile = nullptr;
        return false;
    }
    g_diMap = CreateFileMappingW(g_diFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!g_diMap) {
        dbg2(L"mapobj failed:", GetLastError());
        CloseHandle(g_diFile);
        g_diFile = nullptr;
        return false;
    }
    const unsigned char* view = (const unsigned char*)MapViewOfFile(g_diMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        dbg2(L"view failed:", GetLastError());
        CloseHandle(g_diMap);
        CloseHandle(g_diFile);
        g_diMap = g_diFile = nullptr;
        return false;
    }
    // 头部："FTDISKIDX2\nCOUNT\t<10位数字>\n"
    if (memcmp(view, "FTDISKIDX2\n", 11) != 0 ||
        memcmp(view + 11, "COUNT\t", 6) != 0 ||
        view[11 + 6 + 10] != '\n') {
        dbg2(L"header bad:", view[0]);
        UnmapViewOfFile(view);
        CloseHandle(g_diMap);
        CloseHandle(g_diFile);
        g_diMap = g_diFile = nullptr;
        return false;
    }
    unsigned long long n = 0;
    for (int i = 0; i < 10; ++i) n = n * 10 + (unsigned long long)(view[11 + 6 + i] - '0');
    if (n == 0) {   // 空索引（fd 失败等）：视为不可用，交由重建
        dbg2(L"empty index:", 0);
        UnmapViewOfFile(view);
        CloseHandle(g_diMap);
        CloseHandle(g_diFile);
        g_diMap = g_diFile = nullptr;
        return false;
    }
    g_diRecords = n;
    g_diDataOff = 11 + 6 + 10 + 1;
    g_diView = view + g_diDataOff;
    g_diSize = (unsigned long long)sz.QuadPart - g_diDataOff;
    g_diReady = true;
    dbg2(L"map ok, records:", n);
    return true;
}

unsigned __stdcall DiskBuildThread(void*) {
    auto dbg = [](const wchar_t* msg, unsigned long long v) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0)
            fwprintf(stderr, L"[disk] %s %llu\n", msg, v), fflush(stderr);
    };
    g_diRebuilding = true;
    const std::wstring fdExe = FindFdExe();
    dbg(L"fdExe chars:", fdExe.size());
    do {
        if (fdExe.empty()) break;
        std::wstring path = DiskCachePath();
        if (path.empty()) break;
        dbg(L"cache path ok, rebuilding:", 1);
        std::wstring tmp = path + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { dbg(L"tmp open failed:", GetLastError()); break; }
        auto writeAll = [&](const char* p, DWORD n) {
            while (n > 0) {
                DWORD w = 0;
                if (!WriteFile(h, p, n, &w, nullptr) || w == 0) return false;
                p += w;
                n -= w;
            }
            return true;
        };
        if (!writeAll("FTDISKIDX2\nCOUNT\t0000000000\n", 29)) { CloseHandle(h); break; }
        dbg(L"header written:", 0);

        // 两趟全列（先目录后文件）+ 统一剪枝（与内存索引同规则，junk 不进索引）；
        // 解析期零 GetFileAttributes 调用（个别特殊文件名会挂死）
        std::vector<std::wstring> diRoots = DriveRoots();
        {
            wchar_t renv[1024]{};
            if (GetEnvironmentVariableW(L"FT_DF_DISKINDEX_ROOTS", renv, 1024) > 0) {
                diRoots.clear();
                std::wstring s = renv;
                size_t p = 0;
                while (p < s.size()) {
                    size_t q = s.find(L';', p);
                    if (q == std::wstring::npos) q = s.size();
                    std::wstring one = s.substr(p, q - p);
                    if (!one.empty()) diRoots.push_back(one);
                    p = q + 1;
                }
            }
        }
        dbg(L"roots:", diRoots.size());
        std::string chunk;
        chunk.reserve(4 * 1024 * 1024);
        unsigned long long count = 0;
        bool okW = true;
        for (int pass = 0; okW && pass < 2; ++pass) {   // 0=目录 1=文件
            for (const auto& root : diRoots) {
                if (g_diQuit || count >= DiskCap()) break;
                std::wstring args = L" -a -H --max-results " + std::to_wstring(DiskCap() - count);
                AppendPruneArgs(args, /*includeDot=*/true);
                args += pass == 0 ? L" -t d" : L" -t f";
                args += L" . -- " + QuoteArg(root);
                std::vector<std::string> lines;
                bool capOk = FdCaptureLines(fdExe, args, 600000, lines);
                dbg(L"pass lines:", (unsigned long long)((pass * 1000000) + lines.size()));
                if (!capOk) continue;
                for (const auto& ln : lines) {
                    std::string low = Utf8LowerAscii(ln);
                    if (low.size() > 3 && low.back() == '/') low.pop_back();
                    if (low.size() < 2) continue;
                    chunk += pass == 0 ? "D" : "F";
                    chunk += low;
                    chunk += "\n";
                    ++count;
                    if (chunk.size() >= 4 * 1024 * 1024) {
                        okW = writeAll(chunk.data(), (DWORD)chunk.size());
                        chunk.clear();
                    }
                }
                if (!okW) break;
            }
        }
        dbg(L"walk done, count:", count);
        if (okW && !chunk.empty()) writeAll(chunk.data(), (DWORD)chunk.size());
        // 回写真实 COUNT（定宽 10 位）
        char cnt[11];
        snprintf(cnt, sizeof(cnt), "%010llu", count);
        LARGE_INTEGER off{};
        off.QuadPart = 11 + 6;
        SetFilePointer(h, off.LowPart, &off.HighPart, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(h, cnt, 10, &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);   // 先关独占句柄：否则映射的读打开会 sharing violation
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            dbg(L"rename failed:", GetLastError());
            break;
        }
        dbg(L"renamed:", count);
        {   // 换映射 + 清 delta
            std::lock_guard<std::mutex> lk(g_diMu);
            DiskMapExistingLocked();
            std::lock_guard<std::mutex> lk2(g_ovMu);
            g_ovAdd.clear();
            g_ovDel.clear();
        }
        dbg(L"mapped, ready:", g_diReady.load() ? 1 : 0);
    } while (false);
    g_diRebuilding = false;
    return 0;
}

void DiskMaybeStartRebuild() {
    bool expect = false;
    if (g_diRebuilding.compare_exchange_strong(expect, true)) {   // 抢到启动权（线程结束时自清零）
        CloseHandle((HANDLE)_beginthreadex(nullptr, 0, DiskBuildThread, nullptr, 0, nullptr));
    }
}

// 每盘根一个监听线程：变更进 RAM delta 表（查询时与 blob 合并）
// 每盘根一个监听线程：变更进 RAM delta 表（查询时与 blob 合并）。
// 铁律：GetFileAttributesW（个别路径会挂死）绝不持锁调用、甚至不在事件循环里同步调用——
// 解析出事件后先在本地无锁解析属性，再短暂持锁入表。挂死只影响该盘新鲜度，永不影响搜索。
unsigned __stdcall DiskWatchThread(void* p) {
    std::unique_ptr<std::wstring> rootHolder((std::wstring*)p);
    const std::wstring root = *rootHolder;
    for (;;) {
        if (g_diQuit) break;
        HANDLE h = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Sleep(5000); continue; }
        char buf[65536];
        DWORD bytes = 0;
        for (;;) {
            if (g_diQuit) break;
            if (!ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                       &bytes, nullptr, nullptr))
                break;
            if (bytes == 0) break;   // 溢出：交给重建周期
            // 先无锁解析事件（含属性），再短暂持锁入表
            std::vector<std::pair<std::wstring, int>> resolved;   // path, isDir(-1=删除)
            const FILE_NOTIFY_INFORMATION* fni = (const FILE_NOTIFY_INFORMATION*)buf;
            for (;;) {
                std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                std::wstring path = root + L"\\" + rel;
                int isDir = -1;
                if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                    DWORD attr = GetFileAttributesW(path.c_str());   // 无锁：挂死只拖住本线程
                    if (attr != INVALID_FILE_ATTRIBUTES)
                        isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                }
                resolved.push_back({path, isDir});
                if (fni->NextEntryOffset == 0) break;
                fni = (const FILE_NOTIFY_INFORMATION*)((const char*)fni + fni->NextEntryOffset);
            }
            {
                std::lock_guard<std::mutex> lk(g_ovMu);   // 短暂持锁：纯内存插入
                for (auto& kv : resolved) {
                    if (kv.second < 0) { g_ovDel.insert(kv.first); g_ovAdd.erase(kv.first); }
                    else { g_ovAdd[kv.first] = kv.second != 0; g_ovDel.erase(kv.first); }
                }
            }
            size_t ov = 0;
            { std::lock_guard<std::mutex> lk(g_ovMu); ov = g_ovAdd.size() + g_ovDel.size(); }
            if (ov > 50000 && !g_diRebuilding) DiskMaybeStartRebuild();   // delta 太大：整体重建
            break;
        }
        CloseHandle(h);
        if (g_diQuit) break;
        Sleep(200);
    }
    return 0;
}

void DiskIndexStart() {
    bool expect = false;
    if (!g_diStarted.compare_exchange_strong(expect, true)) return;
    if (GetEnvironmentVariableW(L"FT_DF_DISKINDEX_OFF", nullptr, 0) > 0) return;
    {   // 映射已有索引；超过 7 天未更新的缓存直接视为不存在（后台重建）
        std::lock_guard<std::mutex> lk(g_diMu);
        g_diReady = DiskMapExistingLocked();
        if (g_diReady) {
            // 过期检查：文件时间（1601 纪元）对系统时间（同纪元）——
            // 不能用 GetTickCount64（开机毫秒），否则永远判定为 150 天前
            std::wstring path = DiskCachePath();
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
                ULONGLONG writeFt = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) |
                                    fa.ftLastWriteTime.dwLowDateTime;
                FILETIME nowFt{};
                GetSystemTimeAsFileTime(&nowFt);
                ULONGLONG now64 = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
                ULONGLONG ageMs = writeFt <= now64 ? (now64 - writeFt) / 10000 : 0;
                if (ageMs > 7ULL * 24 * 3600 * 1000) g_diReady = false;   // 超 7 天：重建
            }
        }
    }
    if (!g_diReady) DiskMaybeStartRebuild();
    for (const auto& r : DriveRoots()) {
        CloseHandle((HANDLE)_beginthreadex(nullptr, 0, DiskWatchThread,
                                           new std::wstring(r), 0, nullptr));
    }
}

bool DiskIndexReady() { return g_diReady.load(); }

size_t DiskIndexRecords() { return g_diReady ? g_diRecords : 0; }

// 全 blob 首字节 memchr 扫描 + 行级 AND 校验；delta 表合并增删
int DiskIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows) {
    out.clear();
    auto dbg = [](const wchar_t* m, unsigned long long v) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
            wchar_t b[160]{};
            swprintf(b, 160, L"[disks] %s %llu", m, (unsigned long long)v);
            fwprintf(stderr, L"%ls\n", b);
            fflush(stderr);
        }
    };
    dbg(L"enter, ready:", g_diReady.load() ? 1 : 0);
    if (!g_diReady.load()) return -1;
    if (q.words.empty()) return 0;
    std::vector<std::string> wl;                       // 小写 UTF-8 词
    std::vector<std::wstring> wlw;                     // 宽词（delta 表用）
    for (const auto& w : q.words) {
        std::string u = Utf8LowerAscii(WideToUtf8(w));
        wl.push_back(u);
        wlw.push_back(ToLower(w));
    }
    std::vector<std::string> ex;                       // 排除词（小写 UTF-8）
    for (const auto& e : q.excludes) ex.push_back(Utf8LowerAscii(WideToUtf8(e)));

    std::unordered_set<std::string> seen;              // 去重（fd 对 junction 可能重复枚举）
    std::lock_guard<std::mutex> lk(g_diMu);
    if (!g_diView) return -1;
    dbg(L"locked, size:", g_diSize);
    const unsigned char* base = g_diView;
    const unsigned long long total = g_diSize;
    const std::string& w0 = wl[0];
    const unsigned char c0 = (unsigned char)w0[0];
    for (const unsigned char* p = base; out.size() < (size_t)maxRows; ++p) {
        if ((unsigned long long)(p - base) + w0.size() > total) break;
        p = (const unsigned char*)memchr(p, c0, (size_t)(total - (p - base)));
        if (!p) break;
        // 行边界：命中点回退到上一行首、前进到行尾
        const unsigned char* ls = p;
        while (ls > base && ls[-1] != '\n') --ls;
        const unsigned char* le = p;
        while (le < base + total && *le != '\n') ++le;
        const size_t recLen = (size_t)(le - ls);
        if (recLen < 2) continue;
        const bool isDir = ls[0] == 'D';
        if (q.isFile == isDir) continue;
        const unsigned char* rec = ls + 1;
        const size_t pathLen = recLen - 1;
        // 名字模式：词必须落在最后一段（基名）内；路径模式：整条路径
        const unsigned char* seg = rec;
        size_t segLen = pathLen;
        if (!pathMode) {
            for (size_t i = pathLen; i > 0; --i)
                if (rec[i - 1] == '/') { seg = rec + i; segLen = pathLen - i; break; }
        }
        if (!MemContainsBytes(seg, segLen, w0.data(), w0.size())) continue;   // 命中跨行/跨段等假阳性
        bool ok = true;
        for (size_t i = 1; i < wl.size(); ++i)
            if (!MemContainsBytes(seg, segLen, wl[i].data(), wl[i].size())) { ok = false; break; }
        if (!ok) continue;
        for (const auto& e : ex)
            if (!e.empty() && MemContainsBytes(rec, pathLen, e.data(), e.size())) { ok = false; break; }
        if (!ok) continue;
        if (!seen.insert(std::string((const char*)rec, pathLen)).second) continue;
        std::wstring path = Utf8ToWide(std::string((const char*)rec, pathLen));
        {   // delta 合并：已删除跳过；已是新增条目则交给下方 additions 扫描，避免重复
            std::lock_guard<std::mutex> lk2(g_ovMu);
            if (g_ovDel.count(path)) continue;
            if (g_ovAdd.count(path)) continue;
        }
        // blob 存的是小写路径：还原真实大小写（文件存在；不存在时保留小写）
        wchar_t longBuf[MAX_PATH * 2]{};
        if (GetLongPathNameW(path.c_str(), longBuf, MAX_PATH * 2) > 0) path = longBuf;
        out.push_back({path, isDir});
    }
    // 建库后新增条目（RAM delta）：数量小，宽字符线性过滤
    std::lock_guard<std::mutex> lk2(g_ovMu);
    for (const auto& kv : g_ovAdd) {
        if (out.size() >= (size_t)maxRows) break;
        if (q.isFile == kv.second) continue;
        std::wstring lp = ToLower(kv.first);
        bool ok = true;
        for (const auto& w : wlw)
            if (lp.find(w) == std::wstring::npos) { ok = false; break; }
        if (!ok) continue;
        for (const auto& e : q.excludes)
            if (!e.empty() && lp.find(e) != std::wstring::npos) { ok = false; break; }
        if (!ok) continue;
        std::wstring base = kv.first;
        size_t bp = base.find_last_of(L'\\');
        base = bp == std::wstring::npos ? base : base.substr(bp + 1);
        bool hit = true;
        if (!pathMode) {
            std::wstring bl = ToLower(base);
            for (const auto& w : wlw)
                if (bl.find(w) == std::wstring::npos) { hit = false; break; }
        } else {
            std::wstring fl = ToLower(kv.first);
            for (const auto& w : wlw)
                if (fl.find(w) == std::wstring::npos) { hit = false; break; }
        }
        if (!hit) continue;
        out.push_back({kv.first, kv.second});
    }
    return (int)out.size();
}

}  // namespace df
