// ============================================================
// df_search.cpp â d/f æç´¢åç«¯å®ç°ï¼ä¸å»ºå¨çç´¢å¼ï¼
//   â  Windows Searchï¼çº¯ OLE DB æ¶è´¹èï¼é¶ ATLãé¶ç¬¬ä¸æ¹ä¾èµï¼ã
//      COM æ¯çº¿ç¨åå§åä¸æ¬¡ä¸ä¸ååå§åï¼provider/ä¼è¯è¿ç¨çº§ç¼å­ï¼
//      å¸¸é©»å·¥ä½çº¿ç¨ä¸åç»­æ¥è¯¢æ¯«ç§çº§ã
//   â¡ fdï¼å exe å­è¿ç¨ï¼stdout éè¡è§£æï¼å¨éè¿ç¨æ³¨åè¿ g_fdActiveï¼
//      CancelAll() ç»ä¸ç»æ­¢ï¼ä¿è¯åæ¶æå¤ä¸ä¸ª fd å¨éåã
// ============================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#define DBINITCONSTANTS
#include <initguid.h>   // è®© oledb.h ç DEFINE_GUID å¨æ¬ç¼è¯ååå®ä¾åï¼ä»æ­¤å¤å¼å¥ï¼
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

// cpp-pinyin 拼音引擎（内存索引拼音搜索；仅守护进程运行时启用，见 MemPinyinStart）
#include <cpp-pinyin/Pinyin.h>
#include <cpp-pinyin/ManTone.h>
#include <cpp-pinyin/G2pglobal.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

namespace df {
namespace {

constexpr DWORD kWsProbeRows = 1;    // å¯ç¨æ§æ¢æµï¼æ¿å° 1 è¡å³å¯
constexpr DWORD kWsFetchRows = 400;  // çå®æ¥è¯¢æåä¸éï¼è¿æ»¤/æåºååªå±ç¤ºå 10ï¼
constexpr size_t kFdMaxRows = 400;   // fd åæ¬¾æåä¸éï¼è¯»æ»¡å³ç»æ­¢ fd

// ---------------- éç¨å°å·¥å· ----------------
std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// SQL å­ç¬¦ä¸²å­é¢éè½¬ä¹ï¼åå¼å·ç¿»åã%/_ ä¿çåæ ·ââæä»¶åéåºç°ééç¬¦çæ¦ç
// è¿å°äºè½¬ä¹åéçé£é©ï¼æåä¹åªæ¯å¹éé¢ç¨å®½ã
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

// Windows å½ä»¤è¡åæ°å å¼å·ï¼æ¶å°¾å¼å·åçè¿ç»­åææ ç¿»åï¼å¦å "C:\" ç \"
// ä¼ææ¶å°¾å¼å·è½¬ä¹æå­é¢éï¼C:\ è¢«è§£ææ C:"ï¼ââalt-search åæ¯è¸©è¿çåä¸ä¸ªåã
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

// COM æ¥å£å®å«ï¼ææ Releaseï¼éè¯¯è·¯å¾ä¸æ³æ¼
template <class T>
struct CI {
    T* p = nullptr;
    T* operator->() const { return p; }
    bool ok() const { return p != nullptr; }
    ~CI() { if (p) p->Release(); }
};

// ---------------- Windows Search ----------------

// System.ItemUrlï¼"file:C:/Users/a/b.txt"ï¼â Windows è·¯å¾ï¼å« %XX è§£ç ï¼ã
// file:///C:/ å¥ä¸æï¼file://server/share æ¯ UNCï¼ä¿çåæã
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

// COM æ¯çº¿ç¨åªåå§åä¸æ¬¡ãæ°¸ä¸ä¸»å¨ååå§åï¼CoUninitialize ä¼æ MTA å¼ç¨è®¡æ°
// åå° 0 å¹¶æææ´ä¸ªå¬å¯ï¼ç¼å­éç provider/ä¼è¯æééä¹å¤±æï¼ç¬¬äºæ¬¡æ¥è¯¢å³å´©ï¼ã
// è°ç¨æ¹çº¿ç¨åä¸ºé¿å½çº¿ç¨ï¼flowtary å¸¸é©» WS å·¥ä½çº¿ç¨ / wsquery ä¸»çº¿ç¨ï¼ï¼å®å¨ã
// fd è·¯å¾ä¹éè¦å®ï¼SHGetFolderPathW è§£æç¨æ·ç®å½è¦ COMã
void EnsureThreadCom() {
    static thread_local bool t_comInit = false;
    if (!t_comInit) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hrInit)) t_comInit = true;
        // RPC_E_CHANGED_MODEï¼çº¿ç¨å·²æ¯ STAââç§å¸¸èµ°ï¼ç¼å­è°ç¨çº¿ç¨ä¸ä¼æ¯ STA çº¿ç¨ï¼
    }
}

// è¿ç¨çº§ä¼è¯ç¼å­ï¼provider åå§å + å½ä»¤å·¥ååªå»ºä¸æ¬¡ï¼åç»­æ¥è¯¢æ¯«ç§çº§ã
// æµç¨æ ä¸å¯è½æä¸¤ä¸ªè°ç¨çº¿ç¨ï¼ä¸»ç¨åºå¸¸é©» WS å·¥ä½çº¿ç¨ / wsquery ä¸»çº¿ç¨ï¼ï¼äºæ¥ä¿æ¤ã
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

// å»ºä¼è¯ï¼è°ç¨æ¹å·²æéï¼ãè¿å false æ¶ *hrOut ä¸ºé¦ä¸ªå¤±è´¥ç¹ã
bool WsBuildSession(std::wstring* diag, HRESULT* hrOut) {
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"Search.CollatorDSO", &clsid);
    WsStep(diag, L"CLSID", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IDBInitialize,
                          (void**)&g_wsCache.init);
    WsStep(diag, L"CREATE", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    // EXTENDED PROPERTIES="Application:Windows"ï¼å®æ¹è¿æ¥ä¸²åæ¬¾ï¼â DBPROP_INIT_PROVIDERSTRING
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

// ç»ä¸æ¥è¯¢å¥å£ãmaxRows éå¶æåè¡æ°ï¼æ¢æµç¨ 1ï¼çå®æ¥è¯¢ 400ï¼ã
// æä»¶/æä»¶å¤¹å¤å«æ¾å¨å®¢æ·ç«¯ï¼System.ItemType == "Folder"ï¼èé SQL ç
// <> 'Folder'ï¼æ æ©å±åæä»¶ç ItemType å¯è½ä¸º NULLï¼ä¸å¼é»è¾ä¼æ¼è¡ã
// diag éç©ºæ¶éæ­¥è®°å½å¤±è´¥ç¹ä¸ HRESULTï¼åçå·¥å· wsquery --check ç¨ï¼ã
int WsQueryImpl(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows,
                std::wstring* diag) {
    out.clear();
    if (q.words.empty() && q.excludes.empty()) return 0;

    // ---- ç» SQLï¼éè¯ ANDï¼åå­å¿å«ï¼è·¯å¾æ¨¡å¼æ¾å®½å° ItemUrl æ´æ¡è·¯å¾ï¼----
    std::wstring where;
    for (const auto& w : q.words) {
        std::wstring like = L" LIKE '%" + SqlEscape(w) + L"%'";
        std::wstring c = L"(System.FileName" + like;
        if (pathMode) c += L" OR System.ItemUrl" + like;
        c += L")";
        where += where.empty() ? c : L" AND " + c;
    }
    if (where.empty()) where = L"System.FileName LIKE '%'";  // åªææé¤è¯ï¼å¨éåæé¤
    for (const auto& e : q.excludes) {
        if (e.empty()) continue;
        std::wstring notlike = L" NOT LIKE '%" + SqlEscape(e) + L"%'";
        where += L" AND System.FileName" + notlike;
        if (pathMode) where += L" AND System.ItemUrl" + notlike;
    }
    // å®æ¹ç¤ºä¾åä¸ºè£¸æ è¯ç¬¦ï¼ä¸å åå¼å·
    const std::wstring sql = L"SELECT System.ItemUrl, System.FileName, System.ItemType"
                             L" FROM SystemIndex WHERE " + where;

    // COM æ¯çº¿ç¨åªåå§åä¸æ¬¡ãæ°¸ä¸ä¸»å¨ååå§åï¼è§ EnsureThreadCom æ³¨éï¼
    EnsureThreadCom();
    int result = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {  // ä¼è¯å¤±æï¼æå¡åéå¯ç­ï¼ï¼éå»ºåéè¯ä¸æ¬¡
        // ä¼è¯ç¼å­ï¼provider/ä¼è¯åªå»ºä¸æ¬¡ï¼æ¥è¯¢å¤±è´¥éå»ºä¸æ¬¡åè¯ï¼æå¡éå¯ç­æåµï¼
        std::unique_lock<std::mutex> lk(g_wsCache.mu);
        HRESULT hr = S_OK;
        HRESULT buildHr = S_OK;
        if (!g_wsCache.ready() && !WsBuildSession(diag, &buildHr)) break;
        CI<ICommandText> cmd;
        hr = g_wsCache.cmdFactory->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)&cmd.p);
        WsStep(diag, L"CMD", hr);
        if (FAILED(hr)) { g_wsCache.reset(); continue; }   // éå»ºä¼è¯åèµ°ä¸é
        hr = cmd->SetCommandText(DBGUID_DBSQL, sql.c_str());
        WsStep(diag, L"SETTEXT", hr);
        if (FAILED(hr)) break;

        DBROWCOUNT affected = 0;
        CI<IUnknown> rsUnk;
        hr = cmd->Execute(nullptr, IID_IRowset, nullptr, &affected, &rsUnk.p);
        WsStep(diag, L"EXEC", hr);
        if (FAILED(hr)) {
            if (!diag) { g_wsCache.reset(); continue; }    // éé»éè¯ä¸æ¬¡ï¼æå¡å¯è½åéå¯ï¼
            break;
        }
        CI<IRowset> rs;
        if (FAILED(rsUnk->QueryInterface(IID_IRowset, (void**)&rs.p))) { WsStep(diag, L"RS", E_FAIL); break; }
        lk.unlock();

        // å ordinal æåå®ä½
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

        // è¡ç¼å²ï¼Search provider çåå¼æ¯ BYREF å®½å­ç¬¦ä¸²ï¼æå¡ç«¯åé wchar_t*ï¼
        // ReleaseRows åææï¼ï¼ç»å®å¿é¡» DBTYPE_WSTR|DBTYPE_BYREF + cbMaxLen=0ï¼
        // åºå®é¿åº¦ WSTR æ·è´ä¼æ¥ DB_E_BADBINDINFOã
        // å¸å±ï¼å­èï¼ï¼url ptr[0,8) st[8,12) pad[12,16) | name ptr[16,24) st[24,28) pad
        //              | type ptr[32,40) st[40,44) â å± 48
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
        HROW* prow = nullptr;   // GetNextRows åºåï¼æå¡ç«¯å¡«è¡å¥ææ°ç»
        result = 0;
        for (;;) {
            DBCOUNTITEM got = 0;
            if (FAILED(rs->GetNextRows(DB_NULL_HCHAPTER, 0, 64, &got, &prow)) || got == 0) break;
            for (DBCOUNTITEM i = 0; i < got; ++i) {
                if (FAILED(rs->GetData(prow[i], hacc, rowBuf))) continue;
                // BYREF ç»å®ï¼å¼ä½æ¯ wchar_t* æéï¼ReleaseRows åå¿é¡»åèµ°
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
                if (q.isFile == isFolder) continue;  // f ææä»¶å¤¹ / d åªè¦æä»¶å¤¹
                std::wstring path = ItemUrlToPath(url);
                if (path.empty()) path = name ? name : L"";
                if (path.empty()) continue;
                if (ContainsAny(ToLower(path), q.excludes)) continue;  // å®¢æ·ç«¯ååä¸é
                out.push_back({path, isFolder});
                if (out.size() >= maxRows) break;
            }
            rs->ReleaseRows(got, prow, nullptr, nullptr, nullptr);
            if (out.size() >= maxRows) break;
        }
        if (hacc) acc->ReleaseAccessor(hacc, nullptr);
        result = (int)out.size();
        break;   // æåï¼ä¸åéè¯
    }

    return result;
}

// ---------------- fd ----------------

std::mutex g_fdMutex;
std::vector<HANDLE> g_fdActive;  // å¨é fd è¿ç¨å¥æï¼CancelAll ç»æ­¢ï¼å±ä¸»çº¿ç¨åæ¶+æ³¨éï¼

}  // namespace

bool WindowsSearchAvailable() {
    // æ¢éè¦æ±çæè¡ï¼ç´¢å¼ä¸ºç©ºï¼ä»æªå»ºè¿/è¢«æ¸ç©ºï¼çæºå¨ä¸æä¸å¯ç¨å¤çï¼
    // è°ç¨æ¹æ®æ­¤è·³è¿ WS åç«¯ï¼æ¬æº fd ååç«¯ï¼ï¼é¿åæ¯æ¡æ¥è¯¢ç©ºè·ä¸ä¸ªçº¿ç¨
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
    // éåå¨ fd.exeï¼sharkdp/fd 10.5.0 ååï¼MIT/Apache-2.0ï¼ï¼å®æ¤è¿ç¨çå»ºåº/ååºè¶
    // ä¸ flowtary ç fd å±é½ç¨å®ï¼åªå¨éåç¬é´å­å¨ï¼ä»»å¡ç®¡çå¨å¹³æ¶çä¸å°
    auto tryLocate = [](const wchar_t* name, std::wstring& out) -> bool {
        WCHAR self[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
            std::wstring dir(self);
            size_t p = dir.find_last_of(L"\\/");
            std::wstring cand = (p == std::wstring::npos ? dir : dir.substr(0, p + 1)) + name;
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                out = cand;
                return true;
            }
        }
        WCHAR found[MAX_PATH]{};
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr) > 0) {
            out = found;
            return true;
        }
        return false;
    };
    std::wstring out;
    if (tryLocate(L"fd.exe", out)) return out;
    return L"";
}

void CancelAll() {
    std::vector<HANDLE> hs;
    {
        std::lock_guard<std::mutex> lk(g_fdMutex);
        hs.swap(g_fdActive);
    }
    for (HANDLE h : hs) TerminateProcess(h, 0);  // å·²éåºçè¿ç¨ä¸å¤±è´¥æ å®³ï¼åæ¶ä»ç±å±ä¸»çº¿ç¨å
}

// ---------------- åå­ç´¢å¼ï¼ç¨æ·å¸¸ç¨ç®å½ï¼æ¯«ç§çº§å­ä¸²æç´¢ï¼ ----------------
// å­å¨ä¸ºãæ¯æ ¹ä¸ä¸ªè·¯å¾æ±  + åç§»è¡¨ãï¼è·¯å¾è¿ç»­å­æ¾ï¼åèª \0 ç»å°¾ï¼ï¼æ¡ç®åªè®°
// åç§»/åå­åç§»/æ¯å¦ç®å½ââæ¯ vector<wstring> çä¸åä»¥ä¸åå­ï¼æ éä¸²åéå¼éï¼ã
// å¢éæ´æ° = æ´æ ¹æ¿æ¢ï¼fd éåè¯¥æ ¹åæ´ä½ swapï¼ï¼æ éè·¨æ ¹æªå¨ä¸æ ã

// ---------------- 拼音索引（仅守护进程启用：SearchDaemonServe 调 MemPinyinStart） ----------------
// 每条目名字转全拼（小写）存入拼音池；查询时词条可命中「名字 / 全拼子串 / 首字母子序列」。
// 主程序自带一份 Pinyin 实例，这里只在守护进程内启用，避免 UI 进程重复加载词典；
// exe 同目录无 dict/（或转换失败）时保持空指针，功能静默降级为纯名字匹配。
std::unique_ptr<Pinyin::Pinyin> g_py;
std::mutex g_pyMu;   // 建库线程与维护线程可能并发转换

void MemPinyinStart() {
    if (g_py) return;
    WCHAR exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;
    std::wstring dir(exePath);
    size_t p = dir.find_last_of(L"\\/");
    if (p == std::wstring::npos) return;
    std::wstring dict = dir.substr(0, p + 1) + L"dict";
    if (GetFileAttributesW(dict.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    Pinyin::setDictionaryPath(dict);
    g_py = std::make_unique<Pinyin::Pinyin>();
}

// 名字 → 全拼（小写）。未启用或名字不含汉字时返回空（调用方不存，查询回退名字匹配）。
std::wstring MemNameToPinyin(const wchar_t* name) {
    if (!g_py || !name || !*name) return L"";
    bool hasHan = false;
    for (const wchar_t* p = name; *p; ++p)
        if (*p >= 0x4E00 && *p <= 0x9FFF) { hasHan = true; break; }
    if (!hasHan) return L"";
    std::lock_guard<std::mutex> lk(g_pyMu);
    auto res = g_py->hanziToPinyin(WideToUtf8(name), Pinyin::ManTone::Style::NORMAL,
                                   Pinyin::Error::Default, false, false, false);
    std::string py;
    for (const auto& r : res) {
        if (!r.error && !r.pinyin.empty()) py += r.pinyin;
        else if (!r.hanzi.empty()) py += r.hanzi;
    }
    if (py.empty()) return L"";
    return ToLower(Utf8ToWide(py));
}

// q 是否为 s 的子序列（首字母匹配：wxltjl ⊆ weixinliaotianjilu）；空 q 视为不匹配
bool MemIsSubsequence(const wchar_t* s, const wchar_t* q) {
    if (!*q) return false;
    while (*s) {
        if (*s == *q && !*++q) return true;   // q 消耗尽即命中
        ++s;
    }
    return false;
}

struct MemRootData {
    std::wstring pool;               // è·¯å¾æ± ï¼é¡ºåºæ¼æ¥ï¼æ¯æ¡èªå¸¦ \0ï¼
    std::vector<uint32_t> offs;      // æ¯æ¡ç®è·¯å¾å¨ pool ä¸­çèµ·å§ä¸æ 
    std::vector<uint32_t> nameFlags; // æ¯æ¡ç®ï¼ä½ 31 ä½ = æä»¶ååç§»ï¼æé«ä½ = isDir
    std::wstring pyPool;             // 拼音池（每条目自带 \0；首字符恒为 \0 = 「无拼音」哨兵位）
    std::vector<uint32_t> pyOffs;    // 与 offs 对齐；0 = 无拼音（纯 ASCII 名字不存，查询回退名字匹配）
    size_t size() const { return offs.size(); }
};

std::mutex g_memMu;
std::vector<MemRootData> g_memRoots;        // ä¸ g_memRootNames ä¸æ ä¸ä¸å¯¹åº
std::vector<std::wstring> g_memRootNames;   // æ ¹ç®å½åè¡¨ï¼UserDirsRootsï¼
std::atomic<bool> g_memReady{false};
std::atomic<bool> g_memStarted{false};
std::atomic<bool> g_memQuit{false};
std::mutex g_dirtyMu;
std::condition_variable g_dirtyCv;
std::set<size_t> g_dirty;

// ---------------- 内存索引 delta（监听事件即时入库，查询时与池合并） ----------------
// 池的增量粒度是「整根重扫」（大根 113s 级）：新文件从落盘到可拼音搜要等整根
// 重建。监听线程把增/删/改名事件即时写进 delta 表，查询层与池合并——新条目
// 秒级可搜；防抖后的整根重建照旧进行，换入时按根前缀折叠清表。
struct MemDeltaEntry {
    bool isDir = false;
    std::wstring path;     // 真实大小写（展示）
    std::wstring pinyin;   // 全拼小写；空 = 无拼音（纯 ASCII 名 / 未启用）
};
std::mutex g_memDeltaMu;
std::unordered_map<std::wstring, MemDeltaEntry> g_memDeltaAdd;   // 键=小写反斜杠全路径
std::unordered_set<std::wstring> g_memDeltaDel;

// 整根重建换入后：该根前缀下的 delta 已折叠进池，清表（重建期间的新事件由
// 监听继续入库、下轮收敛——与磁盘索引同一容忍窗口）
void MemDeltaDropRoot(const std::wstring& root) {
    std::wstring low = ToLower(root);
    if (low.back() != L'\\') low += L'\\';
    std::lock_guard<std::mutex> lk(g_memDeltaMu);
    size_t dropped = 0;
    for (auto it = g_memDeltaAdd.begin(); it != g_memDeltaAdd.end();) {
        if (_wcsnicmp(it->first.c_str(), low.c_str(), low.size()) == 0) {
            it = g_memDeltaAdd.erase(it);
            ++dropped;
        } else ++it;
    }
    for (auto it = g_memDeltaDel.begin(); it != g_memDeltaDel.end();) {
        if (_wcsnicmp(it->c_str(), low.c_str(), low.size()) == 0)
            it = g_memDeltaDel.erase(it);
        else ++it;
    }
    if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
        fwprintf(stderr, L"[memdelta] drop root=%hs add=%zu\n",
                 WideToUtf8(root).c_str(), dropped);
        fflush(stderr);
    }
}

DWORD MemCap() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_MEMINDEX_MAX", env, 16) > 0) {
        DWORD v = (DWORD)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return 300000;
}

// å¤§å°åä¸ææå­ä¸²æ¥æ¾ï¼æ´ç´ ç®æ³ï¼16 ä¸æ¡ç­å â æ¯æ¥è¯¢å æ¯«ç§ï¼æ é SIMDï¼
// ASCII 用内联折叠，仅非 ASCII 才调 towlower CRT——扫描热循环主体是 ASCII 路径，
// 逐字符 CRT 调用曾占名字扫描的大头（165K 条 31ms → 快路径后约 1/10）。
inline wchar_t MemFoldChar(wchar_t c) {
    if (c < 0x80) return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
    return (wchar_t)towlower(c);
}

// 在 hay（原名/拼音）里找已折叠词条 nf。词条折叠由查询入口做一次——这里若有
// per-call 构造（此前版本），16 万次调用的堆分配会吃掉 ASCII 快路径的全部收益。
const wchar_t* MemFindFolded(const wchar_t* hay, const std::wstring& nf) {
    if (nf.empty()) return hay;
    const size_t nl = nf.size();
    for (; *hay; ++hay) {
        if (MemFoldChar(*hay) != nf[0]) continue;
        const wchar_t* h = hay + 1;
        size_t k = 1;
        while (k < nl && *h && MemFoldChar(*h) == nf[k]) { ++h; ++k; }
        if (k == nl) return hay;
        if (!*h) break;
    }
    return nullptr;
}

// é»è®¤åªæéï¼-E éä¼ ç» fdï¼fd å¯¹ç®å½åªæ=æ´æ£µå­æ ä¸è¿éåï¼ï¼
//  - ç¹å¼å¤´ï¼.git/.vscode/.cache/ä¸´æ¶ç¹æä»¶ï¼ââä»åå­ç´¢å¼å¯ç¨ï¼fd æç´¢å±ç±
//    å®¢æ·ç«¯æ skipDotFolders å¼å³è¿æ»¤ï¼ä¿æå¼å³è¯­ä¹ï¼
//  - åæ¶ç«/ç³»ç»å·æ ç®å½ï¼$RECYCLE.BINãSystem Volume InformationãRECYCLERï¼
//  - ä¸´æ¶ç®å½ä¸ä¸´æ¶/å¤ä»½æä»¶ï¼tmpãtempã*.tmpã*.tempã~$*ã*~ã*.swpï¼
//  - node_modulesã__pycache__
// ä¸è®¾ç½®ãæé¤è·¯å¾ãé»è®¤éååï¼è¿äºæ¡ç®å±ç¤ºå±æ¬æ¥å°±è¢«è¿æ»¤ï¼åªæçº¯çåå­åéåæ¶é´ã
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

// 基础入池：仅路径池；拼音位补 0（对齐）。加载缓存用（拼音随缓存附带，不重转）。
void MemPoolAddRaw(MemRootData& rd, const std::wstring& path, bool isDir) {
    const uint32_t off = (uint32_t)rd.pool.size();
    rd.pool.append(path);
    rd.pool.push_back(L'\0');
    size_t nameOff = path.find_last_of(L'\\');
    nameOff = nameOff == std::wstring::npos ? 0 : nameOff + 1;
    rd.offs.push_back(off);
    rd.nameFlags.push_back((uint32_t)nameOff | (isDir ? 0x80000000u : 0u));
    rd.pyOffs.push_back(0);
}

// 给最近入池的条目附拼音（py 非空才占池；与名字同形的不存——查询回退名字匹配已覆盖）
void MemPoolAttachPinyin(MemRootData& rd, const std::wstring& py) {
    if (py.empty() || rd.pyOffs.empty()) return;
    if (rd.pyPool.empty()) rd.pyPool.push_back(L'\0');   // 0 号哨兵
    rd.pyOffs.back() = (uint32_t)rd.pyPool.size();
    rd.pyPool.append(py);
    rd.pyPool.push_back(L'\0');
}

// 建库路径：入池 + 汉字名字转拼音（仅守护进程启用时生效）
void MemPoolAdd(MemRootData& rd, const std::wstring& path, bool isDir) {
    MemPoolAddRaw(rd, path, isDir);
    if (!g_py) return;
    size_t nameOff = path.find_last_of(L'\\');
    const wchar_t* name = path.c_str() + (nameOff == std::wstring::npos ? 0 : nameOff + 1);
    std::wstring py = MemNameToPinyin(name);
    if (!py.empty() && _wcsicmp(py.c_str(), name) != 0)
        MemPoolAttachPinyin(rd, py);
}

// ç´¢å¼ç¼å­è½çï¼UTF-8 ææ¬ï¼é¦è¡çæ¬æ è®°ï¼R\tæ ¹ç®å½ ä¸ºåèæ è®°ï¼D/F\tpath ä¸ºæ¡ç®ï¼ã
// ä½ç¨ï¼éå¯æ¶æ¯«ç§çº§å è½½ç«å³å¯ç¨ï¼å¯è½ç¥æ§ï¼åå°å¨ééå»ºåè¦çï¼ï¼
// 100s çº§çå»ºåºææ¬åªå¨é¦æ¬¡è¿è¡ï¼æåªæè§ååæ´åï¼æ¯ä»ã
std::wstring MemCachePath() {
    EnsureThreadCom();
    wchar_t p[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, p)))
        return L"";
    std::wstring dir = std::wstring(p) + L"\\Flowtary";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\memindex.dat";
}
// æ g_memMu è°ç¨ãåååæä»¶ï¼1MB ç¼å²ï¼ââä¸æ´ä¸²æ¼æ¥ï¼é¿å 20MBÃ2 çå å³°å¼é©»ç
std::mutex g_memSaveMu;
// 缓存落盘：锁内仅做快照（pool 是单个 wstring，拷贝≈一次 memcpy，~5ms），
// 序列化+写盘在锁外——此前整段 41MB 落盘都持 g_memMu，根重建完成时查询被卡 50-150ms。
// 可不持 g_memMu 调用；g_memSaveMu 串行化并发写（维护线程与建库线程可能同时保存）。
void MemIndexSave() {
    std::vector<MemRootData> snap;
    std::vector<std::wstring> names;
    {
        std::lock_guard<std::mutex> lk(g_memMu);
        names = g_memRootNames;
        snap.reserve(g_memRoots.size());
        for (const auto& rd : g_memRoots) {
            MemRootData c;
            c.pool = rd.pool;
            c.offs = rd.offs;
            c.nameFlags = rd.nameFlags;
            snap.push_back(std::move(c));
        }
    }
    std::lock_guard<std::mutex> savelk(g_memSaveMu);
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
    bool ok = writeAll("FTMEMINDEX4\n", 12);   // 格式版本标记：规则/存储变更时旧缓存自动失效
    std::string chunk;
    chunk.reserve(1024 * 1024);
    for (size_t i = 0; ok && i < snap.size() && i < names.size(); ++i) {
        chunk += "R\t";
        chunk += WideToUtf8(names[i]);
        chunk += "\n";
        const MemRootData& rd = snap[i];
        for (size_t k = 0; ok && k < rd.offs.size(); ++k) {
            const wchar_t* path2 = rd.pool.c_str() + rd.offs[k];
            chunk += (rd.nameFlags[k] & 0x80000000u) ? "D\t" : "F\t";
            chunk += WideToUtf8(path2);
            chunk += "\n";
            if (k < rd.pyOffs.size() && rd.pyOffs[k] != 0) {   // 附拼音行（仅汉字名字条目）
                chunk += "P\t";
                chunk += WideToUtf8(rd.pyPool.c_str() + rd.pyOffs[k]);
                chunk += "\n";
            }
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
// æ g_memMu è°ç¨ï¼æåæ¶æ g_memRootNames å¯¹é½å¡«å g_memRootsï¼ç¼å­å¤åºçæ ¹ä¸¢å¼ï¼
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

    std::unordered_map<std::wstring, MemRootData> byRoot;   // å°åæ ¹ â æ¡ç®
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
    bool has = nextLine(line);
    while (has) {
        bool consumed = true;   // 本行已处理完，循环尾再读下一行
        if (!versionOk) {
            versionOk = line == "FTMEMINDEX4";   // 版本不符（旧格式缓存）→ 弃用重建
            if (!versionOk) break;
        } else if (line.size() < 2) {
            ;
        } else if (line[0] == 'R' && line[1] == '\t') {
            curRoot = ToLower(Utf8ToWide(line.substr(2)));
        } else {
            const bool isDir = line[0] == 'D';
            if (line[1] != '\t' || curRoot.empty()) {
                ;
            } else {
                std::wstring path = Utf8ToWide(line.substr(2));
                if (path.size() >= 2) {
                    MemRootData& rd = byRoot[curRoot];
                    MemPoolAddRaw(rd, path, isDir);
                    has = nextLine(line);   // 预读：可能是该条目的拼音行
                    consumed = false;
                    if (has && line.size() >= 2 && line[0] == 'P' && line[1] == '\t') {
                        std::wstring py = Utf8ToWide(line.substr(2));
                        if (!py.empty()) MemPoolAttachPinyin(rd, py);
                        has = nextLine(line);
                    }
                }
            }
        }
        if (consumed) has = nextLine(line);
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
    for (const auto& name : g_memRootNames) {   // æå½åæ ¹é¡ºåºå¯¹é½
        auto it = byRoot.find(ToLower(name));
        if (it != byRoot.end()) g_memRoots.push_back(std::move(it->second));
        else g_memRoots.push_back(MemRootData{});
    }
    return true;
}

// fd å¨åä¸ä¸ªæ ¹ï¼æ¨¡å¼ . å¹éä»»æéç©ºåï¼ãåªæè§ AppendPruneArgsï¼
// ä¸å¸¦ -Iï¼å°é .gitignoreï¼build/dist ç­æªå¿½ç¥äº§ç©ä»å¥ç´¢å¼ï¼ã
// fd å­è¿ç¨è¿è¡å¹¶æè¡æ¶éåå§è¾åºï¼UTF-8ãæ­£ææ ï¼ãä¸¤è¶å¼å»ºåºçå¬å±åºåº§ï¼
// ä¸å¨è§£æå¾ªç¯éè° GetFileAttributesWââä¸ªå«ç¹æ®æä»¶åä¼è®©å®ææ­»ï¼Defender ç¯å¢ï¼ã
// fd 遍历子进程统一挂 KILL_ON_JOB_CLOSE Job：宿主（守护进程/flowtary/wsquery）退出
// 或被杀时，未完成的遍历一并终止，杜绝孤儿 walker 抢占磁盘 I/O。MemListRootFd 与
// FdQuery 两条 spawn 路径都必须挂——此前只有 FdQuery 挂了，杀守护进程会留下孤儿 fd。
// 返回进程级单例句柄（创建失败返回 nullptr，fd 照常运行）。
static HANDLE InitFdWalkerJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
        jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof(jli));
    }
    return job;
}
static HANDLE FdWalkerJob() {
    static HANDLE s_job = InitFdWalkerJob();   // C++11 魔法静态：并发初始化安全
    return s_job;
}

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
    std::wstring mutableCmd = cmd;   // CreateProcessW ä¼æ¹åå½ä»¤è¡ç¼å²ï¼å¿é¡»ä¼ å¯åå¯æ¬ï¼const åæ°ç data() åªè¯»ï¼å¼ºè½¬ä¼å´©ï¼
    BOOL ok = CreateProcessW(fdExe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(hrd); return false; }
    CloseHandle(pi.hThread);
    SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);   // 略低于前台：IDLE 会被等待中的普通优先级线程饿死（实测 3.7s 手动 vs 10min 超时）
    if (HANDLE job = FdWalkerJob()) AssignProcessToJobObject(job, pi.hProcess);

    char buf[262144];
    std::string acc;
    const ULONGLONG start = GetTickCount64();
    for (;;) {   // PeekNamedPipe è½®è¯¢ + ååºè¶æ¶ï¼ç©ºé²ä¼åçº§èµ°å®å¨çå¯è¾¾åéçº§ï¼
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

// fd ä¸¤è¶å¨åä¸ä¸ªæ ¹ï¼åç®å½åæä»¶ï¼ï¼æ¡ç®è¿æ± ãåªæè§ AppendPruneArgsï¼
// ä¸å¸¦ -Iï¼å°é .gitignoreï¼build/dist ç­æªå¿½ç¥äº§ç©ä»å¥ç´¢å¼ï¼ã
bool MemListRootFd(const std::wstring& fdExe, const std::wstring& root, size_t cap,
                   MemRootData& rd) {
    rd = MemRootData{};
    // æ³¨æé¡ºåºï¼-E åªæå¿é¡»å¨ã-- æ ¹ãä¹åï¼-- ä¹åçåæ°é½æ¯ä½ç½®åæ°ï¼ä¸åæ¯éé¡¹ï¼
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

// æ¯ä¸ªç¨æ·ç®å½ä¸ä¸ªçå¬çº¿ç¨ï¼é»å¡å¨ ReadDirectoryChangesW ä¸ï¼åæ ¸ç­å¾ï¼é¶ CPUï¼ï¼
// ä»»ä½åå­åæ´ï¼å¢/å /æ¹åï¼é½æè¯¥æ ¹æ èï¼ç±ç»´æ¤çº¿ç¨é²æåå¢ééå»º
struct MemWatchArg {
    size_t idx;
    std::wstring root;
};
// 监听事件 → delta 表。属性解析不持任何锁（个别路径 GetFileAttributesW 会挂死，
// 挂死只拖住本监听线程）；入表短暂持锁。delta 超过 5 万条放弃增量（交给整根重建）。
void MemDeltaIngest(const std::wstring& root, const char* buf, DWORD bytes) {
    std::vector<std::pair<std::wstring, int>> resolved;   // path, isDir(-1=删除)
    const FILE_NOTIFY_INFORMATION* fni = (const FILE_NOTIFY_INFORMATION*)buf;
    for (;;) {
        std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
        if (!rel.empty()) {
            std::wstring path = root + L"\\" + rel;
            int isDir = -1;
            if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                DWORD attr = GetFileAttributesW(path.c_str());   // 无锁：挂死只拖本线程
                if (attr != INVALID_FILE_ATTRIBUTES)
                    isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
            resolved.push_back({std::move(path), isDir});
        }
        if (fni->NextEntryOffset == 0) break;
        fni = (const FILE_NOTIFY_INFORMATION*)((const char*)fni + fni->NextEntryOffset);
    }
    std::lock_guard<std::mutex> lk(g_memDeltaMu);
    if (g_memDeltaAdd.size() + g_memDeltaDel.size() > 50000) return;   // 洪峰：等整根重建
    for (auto& kv : resolved) {
        std::wstring key = ToLower(kv.first);
        if (kv.second < 0) { g_memDeltaDel.insert(key); g_memDeltaAdd.erase(key); }
        else {
            size_t np = kv.first.find_last_of(L'\\');
            MemDeltaEntry de;
            de.isDir = kv.second != 0;
            de.path = std::move(kv.first);
            de.pinyin = MemNameToPinyin(de.path.c_str() + (np == std::wstring::npos ? 0 : np + 1));
            g_memDeltaAdd[key] = std::move(de);
            g_memDeltaDel.erase(key);
        }
    }
}

unsigned __stdcall MemWatchThread(void* p) {
    std::unique_ptr<MemWatchArg> arg((MemWatchArg*)p);
    const size_t idx = arg->idx;
    const std::wstring root = std::move(arg->root);
    for (;;) {
        if (g_memQuit) break;
        HANDLE h = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {   // ç®å½æä¸å¯è§ï¼U çç­ï¼ï¼ç­å¾éè¯
            Sleep(5000);
            continue;
        }
        char buf[65536];
        DWORD bytes = 0;
        for (;;) {
            if (g_memQuit) break;
            // 原地续挂（同句柄反复排队等待）：每批事件后 close+Sleep+reopen 的空窗期
            // 内的变更会漏标脏。句柄失效（目录被删/重命名）才落到外层重开。
            if (!ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                       &bytes, nullptr, nullptr))
                break;
            // 增/删/改名事件即时入 delta（新条目秒级可搜）；缓冲区溢出时事件不全，
            // 交给整根重建收敛
            if (bytes > 0) MemDeltaIngest(root, buf, bytes);
            // 无论事件是否可解析都标脏（防抖由维护线程做）；溢出同样整体标脏
            MemMarkDirty(idx);
        }
        CloseHandle(h);
        if (g_memQuit) break;
        Sleep(200);   // ç®å½å¥æå¤±æï¼éå½å/ç§»å¨ï¼åç¨ç­éæ
    }
    return 0;
}

// ç»´æ¤çº¿ç¨ï¼é²æ 1.5s åææ ¹æ´æ ¹éå»ºï¼fd éåè¯¥æ ¹ â æ´æ ¹ swapï¼æ ä¸æ æªå¨ï¼ã
// éæ«ä¸æ ¹å¯è¾¾åéçº§ï¼Defenderï¼ï¼30s åä¸éå¤æ«åä¸æ ¹ï¼èæ è®°çå°ä¸è½®ï¼ã
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
                deferred.insert(idx);   // åæ«è¿ï¼æ¨è¿ï¼é¿åç®å½é«é¢åæ´æ¶åå¤å¨æ«
                continue;
            }
            lastWalk[idx] = now;
            if (idx >= g_memRootNames.size()) continue;
            MemRootData part;
            MemListRootFd(fdExe, g_memRootNames[idx], MemCap(), part);   // ä¸æéï¼å¯è¾¾åéçº§ï¼
        bool swapped = false;
        {
            std::lock_guard<std::mutex> lk(g_memMu);
            if (idx < g_memRoots.size()) {
                part.pool.shrink_to_fit();
                part.offs.shrink_to_fit();
                part.nameFlags.shrink_to_fit();
                g_memRoots[idx] = std::move(part);
                swapped = true;
            }
        }
        if (swapped) {
            MemIndexSave();   // 锁外落盘（快照式）
            MemDeltaDropRoot(g_memRootNames[idx]);   // 该根 delta 已折叠进池
        }
        }
        if (!deferred.empty()) {
            std::lock_guard<std::mutex> lk(g_dirtyMu);
            g_dirty.insert(deferred.begin(), deferred.end());   // ç­å·å´æè¿åæ«ï¼wait è°è¯ä¼èªæ¥ï¼
        }
    }
    return 0;
}

// å»ºåºçº¿ç¨ï¼åå è½½ç£çç¼å­ï¼éå¯å³æ¯«ç§çº§å¯ç¨ï¼ï¼åå°éæ ¹éå»ºââæ¯æ ¹æ«å®ç«å³æ¢å¥
// ï¼ä¸åæ¶ææä¸¤ä»½å¨éæ± ï¼å³°å¼åå­ â å¨éæ±  + åæ ¹ï¼
size_t MemIndexSizeLocked();   // 持 g_memMu 调用；定义在 MemIndexSize 处

unsigned __stdcall MemBuildThread(void*) {
    auto dbg = [](const wchar_t* m, unsigned long long v) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
            fwprintf(stderr, L"[mem] %s %llu\n", m, (unsigned long long)v);
            fflush(stderr);
        }
    };
    {
        std::lock_guard<std::mutex> lk(g_memMu);
        if (MemIndexLoadLocked()) {
            g_memReady = true;   // 缓存可能略旧：先用，逐根刷新
            dbg(L"cache loaded, entries:", MemIndexSizeLocked());
        } else {
            dbg(L"cache load failed", 0);
        }
    }
    std::wstring fdExe = FindFdExe();
    size_t total = 0;
    if (!fdExe.empty()) {
        const size_t cap = MemCap();
        for (size_t i = 0; i < g_memRootNames.size() && total < cap; ++i) {
            if (g_memQuit) break;
            const ULONGLONG t0 = GetTickCount64();
            MemRootData part;
            MemListRootFd(fdExe, g_memRootNames[i], cap - total, part);
            total += part.size();
            dbg(L"root done:", ((unsigned long long)i << 32) | part.size());
            dbg(L"root ms:", GetTickCount64() - t0);
            {
                std::lock_guard<std::mutex> lk(g_memMu);
                part.pool.shrink_to_fit();
                part.offs.shrink_to_fit();
                part.nameFlags.shrink_to_fit();
                if (i < g_memRoots.size()) g_memRoots[i] = std::move(part);
                else g_memRoots.push_back(std::move(part));
            }
            MemDeltaDropRoot(g_memRootNames[i]);   // 该根 delta 已折叠进池
        }
    MemIndexSave();   // 锁外落盘（快照式）
        dbg(L"build done, total:", total);
    }
    g_memReady = true;
    return 0;
}

void MemIndexStart(const std::vector<std::wstring>& extraRoots) {
    bool expect = false;
    if (!g_memStarted.compare_exchange_strong(expect, true)) return;
    if (GetEnvironmentVariableW(L"FT_DF_MEMINDEX_OFF", nullptr, 0) > 0) {
        g_memReady = true;   // æ¾å¼å³é­ï¼ç´¢å¼æç©ºï¼è°åº¦é¾ç´æ¥èµ° fd/WS
        return;
    }
    g_memRootNames = UserDirsRoots();
    for (const auto& extra : extraRoots) {   // æ³¨åè¡¨ IndexDirs éç½®çé¡¹ç®ç®å½ç­
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

// 持 g_memMu 调用
size_t MemIndexSizeLocked() {
    size_t n = 0;
    for (const auto& rd : g_memRoots) n += rd.size();
    return n;
}

size_t MemIndexSize() {
    std::lock_guard<std::mutex> lk(g_memMu);
    return MemIndexSizeLocked();
}

int MemIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows) {
    out.clear();
    if (!g_memReady.load()) return -1;
    const ULONGLONG t0 = GetTickCount64();
    std::vector<std::wstring> wl;
    wl.reserve(q.words.size());
    for (const auto& w : q.words) {   // 词条折叠一次（与 MemFoldChar 同规则，供 MemFindFolded）
        std::wstring f;
        f.reserve(w.size());
        for (wchar_t c : w) f += MemFoldChar(c);
        wl.push_back(std::move(f));
    }
    std::lock_guard<std::mutex> lk(g_memMu);
    bool hasDel = false;   // 查询窗口内被监听标记删除的池条目（快照，竞态下轮收敛）
    { std::lock_guard<std::mutex> lk2(g_memDeltaMu); hasDel = !g_memDeltaDel.empty(); }
    for (const auto& rd : g_memRoots) {
        for (size_t k = 0; k < rd.offs.size(); ++k) {
            const wchar_t* path = rd.pool.c_str() + rd.offs[k];
            const uint32_t nf = rd.nameFlags[k];
            const bool isDir = (nf & 0x80000000u) != 0;
            if (q.isFile == isDir) continue;   // f 排文件夹 / d 只要文件夹
            const wchar_t* name = path + (nf & 0x7FFFFFFFu);
            const wchar_t* py = (k < rd.pyOffs.size() && !rd.pyPool.empty())
                                    ? rd.pyPool.c_str() + rd.pyOffs[k] : nullptr;
            bool ok = true;
            if (pathMode) {
                for (const auto& w : wl)
                    if (!MemFindFolded(path, w)) { ok = false; break; }
            } else {
                for (const auto& w : wl) {
                    if (MemFindFolded(name, w)) continue;
                    if (py && *py) {   // 拼音匹配：全拼子串 或 首字母子序列（≥2 字符防噪声）
                        if (MemFindFolded(py, w)) continue;
                        if (w.size() >= 2 && MemIsSubsequence(py, w.c_str())) continue;
                    }
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (!q.excludes.empty() && ContainsAny(ToLower(path), q.excludes)) continue;
            if (hasDel) {
                std::lock_guard<std::mutex> lk2(g_memDeltaMu);
                if (g_memDeltaDel.count(ToLower(std::wstring(path)))) continue;
            }
            out.push_back({path, isDir});
            if (out.size() >= maxRows) break;
        }
        if (out.size() >= maxRows) break;
    }
    // delta 合并：监听线程即时入库的新增/改名条目（秒级可搜，整根重建换入后折叠），
    // 匹配规则与池一致（名字 / 全拼子串 / 首字母子序列）
    {
        std::lock_guard<std::mutex> lk2(g_memDeltaMu);
        for (const auto& kv : g_memDeltaAdd) {
            if (out.size() >= (size_t)maxRows) break;
            const MemDeltaEntry& de = kv.second;
            if (q.isFile == de.isDir) continue;
            size_t np = de.path.find_last_of(L'\\');
            const wchar_t* name = de.path.c_str() + (np == std::wstring::npos ? 0 : np + 1);
            bool ok = true;
            if (pathMode) {
                for (const auto& w : wl)
                    if (!MemFindFolded(de.path.c_str(), w)) { ok = false; break; }
            } else {
                for (const auto& w : wl) {
                    if (MemFindFolded(name, w)) continue;
                    if (!de.pinyin.empty()) {
                        if (MemFindFolded(de.pinyin.c_str(), w)) continue;
                        if (w.size() >= 2 && MemIsSubsequence(de.pinyin.c_str(), w.c_str())) continue;
                    }
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (!q.excludes.empty() && ContainsAny(ToLower(de.path), q.excludes)) continue;
            out.push_back({de.path, de.isDir});
        }
    }
    if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
        fwprintf(stderr, L"[mems] mode=%s ms=%llu hits=%zu\n",
                 pathMode ? L"path" : L"name",
                 (unsigned long long)(GetTickCount64() - t0), out.size());
        fflush(stderr);
    }
    return (int)out.size();
}
DWORD DefaultFdDeadlineMs() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_FD_TIMEOUT_MS", env, 16) > 0) {
        DWORD v = (DWORD)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return 8000;  // é»è®¤ 8s é¢ç®ï¼è¶æ¶åè°ç¨æ¹è·³è¿è·¯å¾ååºï¼æå â¤8sï¼
}

// ç¨æ·å¸¸ç¨ç®å½æ ¹ï¼æ¡é¢/ææ¡£/ä¸è½½/å¾ç/é³ä¹/è§é¢ï¼
std::vector<std::wstring> UserDirsRoots() {
    EnsureThreadCom();
    std::vector<std::wstring> roots;
    auto addRoot = [&](const std::wstring& r) {
        if (r.size() < 3 || r[1] != L':') return;
        // GetDriveTypeW å¯¹éæ ¹è·¯å¾è¡ä¸ºä¸å¯é ï¼å®æµè¿å DRIVE_REMOVABLEï¼ï¼
        // å¿é¡»åæªåº "C:\" çæ ¹åå¤ç±»å
        wchar_t driveRoot[4] = { r[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(driveRoot) != DRIVE_FIXED) return;
        for (const auto& e : roots)
            if (_wcsicmp(e.c_str(), r.c_str()) == 0) return;
        roots.push_back(r);
    };
    const int csidls[] = {
        CSIDL_DESKTOPDIRECTORY, CSIDL_PERSONAL, CSIDL_MYPICTURES,
        CSIDL_MYMUSIC, CSIDL_MYVIDEO,
        0x000C,  // CSIDL_DOWNLOADSï¼è SDK æ å®ï¼å¼åºå®ï¼
        CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86,   // å¸¸è§å®è£ç®å½ä¹è¿æ¯«ç§å±
    };
    for (int csidl : csidls) {
        wchar_t p[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, p)))
            addRoot(p);
    }
    {   // ç¨æ·çº§å®è£ç®å½ï¼VS Code / Discord ç­è£å¨ %LOCALAPPDATA%\Programsï¼
        wchar_t la[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, la))) {
            std::wstring progs = std::wstring(la) + L"\\Programs";
            if (GetFileAttributesW(progs.c_str()) != INVALID_FILE_ATTRIBUTES) addRoot(progs);
        }
    }
    return roots;
}

// å¨é¨åºå®çæ ¹
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
    EnsureThreadCom();   // SHGetFolderPathW è§£æç¨æ·ç®å½éè¦ COM
    // æç´¢æ ¹ï¼é»è®¤ = ç¨æ·å¸¸ç¨ç®å½ï¼æ¡é¢/ææ¡£/ä¸è½½/å¾ç/é³ä¹/è§é¢ï¼åç½® + å¨é¨åºå®çã
    // fd çå±äº«éåéåæ²¡ææ ¹ä¼åçº§ï¼å¤§æ ¹ä¼æå°æ ¹çç»ææ·¹æ²¡å°å åç§ä¹åââ
    // éè¦ä¸¥æ ¼ä¿è¯å¸¸ç¨åºåååºç»ææ¶ï¼è°ç¨æ¹åºåé¶æ®µè°ç¨
    //ï¼rootsOverride=UserDirsRoots() åè¡ï¼åç¨ DriveRoots() è¡¥å¨çï¼ã
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

    // -a ç»å¯¹è·¯å¾ï¼-F åºå®å­ç¬¦ä¸²ï¼è¯éçæ­£ååå­ç¬¦ä¸ç¸ï¼ï¼-i ä¸åå¤§å°åï¼
    // -I ä¸çä¼ .gitignore/.ignoreï¼build/dist è¿ç±»è¢« ignore çç®å½ä¹è¦è½æå°ï¼
    //    ç¹å¼å¤´è¿æ»¤äº¤ç»ä¸»ç¨åºå®¢æ·ç«¯è¿æ»¤ï¼ä¸ Everything ç»æåè§åï¼ï¼
    // -t f/-t d ç±»åè¿æ»¤ï¼-p è·¯å¾çº§ååºï¼--max-results å½ä¸­å³æ¶å·¥ï¼
    // å¤è¯ ANDï¼é¦ä¸ªè¯ä¸ºä½ç½®æ¨¡å¼ï¼å¶ä½éä¸ª --andï¼fd 10 çè£¸ä½ç½®å¤æ¨¡å¼æ¯ ORï¼ï¼
    // ã--ãä¹åå¨é¨æ¯æç´¢æ ¹ã
    std::wstring args = L" -a -F -i -I --max-results 400";
    if (!skipHidden) args += L" -H";
    if (pathMode) args += L" -p";
    args += q.isFile ? L" -t f" : L" -t d";
    AppendPruneArgs(args, /*includeDot=*/false);   // åæ¶ç«/ç³»ç»/ä¸´æ¶ç®å½åªæï¼æéå¨çéå
    for (size_t i = 0; i < q.words.size(); ++i) {
        if (i > 0) args += L" --and";
        args += L" " + QuoteArg(q.words[i]);
    }
    args += L" --";
    for (const auto& r : roots) args += L" " + QuoteArg(r);
    std::wstring cmd = QuoteArg(fdExe) + args;

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;   // ä¸¤ç«¯åå¯ç»§æ¿ï¼sa.bInheritHandle=TRUEï¼
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;   // å­è¿ç¨æ¿èµ°å¯ç»§æ¿åç«¯ï¼ç¶è¿ç¨å¨ CreateProcess åå³é­
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(fdExe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);      // èªèº«ä¸çåç«¯ï¼fd éåºåç®¡é EOF å¯å¤å®
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    // Jobï¼KILL_ON_JOB_CLOSEââå®¿ä¸»ï¼å®æ¤è¿ç¨/flowtary/wsqueryï¼éåºæè¢«ææ¶ï¼
    // æªå®æç fd éåä¸å¹¶ç»æ­¢ï¼æç»å­¤å¿ walker æ¢å ç£ç I/Oï¼å®æµ 6 ä¸ªå­¤å¿å¹¶åï¼
    if (HANDLE job = FdWalkerJob()) AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(g_fdMutex);
        g_fdActive.push_back(pi.hProcess);
    }

    char buf[16384];
    std::string acc;
    // è½®è¯¢è¯»åï¼PeekNamedPipe + ç­ç­å¾ï¼ï¼fd å¨çéåå¨æ¬æºå®æµå¯è¾¾ 100s+ï¼Defender
    // å®æ¶æ«æææ¢æ¯å±ç®å½æä¸¾ï¼ï¼ç­ä¸äºä¹ä¸è¯¥ç­ââè¶è¿é¢ç®å°±ç»æ­¢ fdï¼ä¿çå·²è¯»å°ç
    // é¨åç»æï¼æ°ä¸è½®æ¥è¯¢/éèçªå£ä¼ CancelAllï¼ä¸ä¼æ®çï¼ãonBatch éç©ºæ¶ææ¹æ¬¡
    // å¢éæéï¼â¥50 æ¡ææ¯ 400ms ä¸æ¹ï¼ï¼çé¢æµå¼ä¸å±ä¸ç¨ç­é¢ç®è·å®ã
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
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // ç®¡éæ­å¼ = fd å·²éåº
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
                if (path.size() > 3 && path.back() == L'\\') path.pop_back();  // ç»ä¸å»å°¾åéç¬¦ï¼çæ ¹é¤å¤ï¼
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
            // fd å·²éåºï¼æå°¾é¨æ®çè¯»å®ï¼å¯è½å°æä¸åæªå°ï¼
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        } else if (deadlineMs && GetTickCount64() - start >= deadlineMs) {
            if (timedOut) *timedOut = true;
            break;  // è¶æ¶ï¼äº¤ç±ä¸æ¹ç»ä¸ç»æ­¢ fdï¼é¨åç»æç§å¸¸è¿å
        }
    }
    flush();

    TerminateProcess(pi.hProcess, 0);  // è¯»æ»¡ä¸éæååæ¶ç®¡ééå¯è½è¿ææ°æ®ï¼å¯¹å·²éåºè¿ç¨æ å®³
    WaitForSingleObject(pi.hProcess, 3000);
    // FT_DF_DEBUG=1ï¼å stderr æå°å½ä»¤è¡ä¸éåºç ï¼åçè¯æ­ç¨ï¼æ­£å¸¸è¿è¡é¶è¾åºï¼
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

// ---------------- ç£çç´¢å¼ï¼å¨çè¦çï¼ç´¢å¼æä»¶å + mmap æ«æï¼è¿ç¨ç§æåå­è¿é¶ï¼ ----------------
// fd å¨çéåä¸æ¬¡ï¼ç©ºé²ä¼åçº§ï¼æ°ç¾ç§ï¼â å°åå UTF-8 è·¯å¾ blobï¼æ°ç¾ MB ç£çæä»¶ï¼
// â MapViewOfFile åªè¯»æ å°ï¼æ¥è¯¢ = é¦å­è memchr å¨ blob æ«æï¼é¡µç¼å­ææ 20â40msï¼ã
// blob æ¯æä»¶æ å°é¡µï¼å¯è¢«ç³»ç»æéæ¢åºï¼ä¸è®¡å¥è¿ç¨ç§æåå­ï¼plocate æè·¯çç®åçï¼ã
// æ°é²åº¦ï¼æ¯çæ ¹ ReadDirectoryChangesW â RAM åå° delta è¡¨ï¼å¢/å ï¼ï¼æ¥è¯¢æ¶åå¹¶ï¼
// delta è¶ 5 ä¸æ¡æç¼å­è¶ 7 å¤©èªå¨åå°éå»ºï¼ç©ºé²ä¼åçº§ï¼ãFT_DF_DISKINDEX_OFF=1 å³é­ã

HANDLE g_diFile = nullptr, g_diMap = nullptr;
const unsigned char* g_diView = nullptr;   // è®°å½åºèµ·å§ï¼è·³è¿å¤´é¨ï¼
unsigned long long g_diSize = 0;           // è®°å½åºå­èæ°
unsigned long long g_diDataOff = 0;        // å¤´é¨ä¹åçè®°å½åºèµ·å§åç§»
unsigned long long g_diRecords = 0;
std::atomic<bool> g_diReady{false};
std::mutex g_diMu;                         // ä¿æ¤ view/map æ¢å¥æ¢åºï¼æç´¢æé´ææï¼
std::atomic<bool> g_diStarted{false};
std::atomic<bool> g_diQuit{false};
std::atomic<bool> g_diRebuilding{false};
std::mutex g_ovMu;
std::unordered_map<std::wstring, std::pair<bool, std::wstring>> g_ovAdd;   // 建库后新增：键(小写反斜杠路径) → {isDir, 真实路径}
std::unordered_set<std::wstring> g_ovDel;         // å»ºåºåå é¤

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

// blob åè·¯å¾å·²å°ååï¼ASCII æå ï¼é ASCII åæ ·ï¼ââè¿éåªå ASCII æå 
std::string Utf8LowerAscii(const std::string& s) {
    std::string o = s;
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return o;
}

// æ´ç´  + memchr è·³è·çå­èå­ä¸²æ¥æ¾ï¼blob å·²å°ååï¼éå­èç²¾ç¡®æ¯è¾ï¼
bool MemContainsBytes(const unsigned char* hay, size_t hl, const char* nd, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    const unsigned char c0 = (unsigned char)nd[0];
    for (size_t i = 0; i + nl <= hl; ++i) {
        if (hay[i] != c0) {
            if (i + 1 + nl - 1 > hl) return false;
            const unsigned char* f = (const unsigned char*)memchr(hay + i + 1, c0, hl - (i + 1) - (nl - 1) + 1);
            if (!f) return false;
            i = (size_t)(f - hay) - 1;   // for ++i åæå f
            continue;
        }
        if (nl == 1 || memcmp(hay + i + 1, nd + 1, nl - 1) == 0) return true;
    }
    return false;
}

// æ å°å·²æç´¢å¼æä»¶ï¼æ ¡éªçæ¬å¤´ + è§£æ COUNTï¼ï¼æåç½® ready
// ---------------- 目录级大小写还原缓存 ----------------
// blob 存小写路径，命中后要还原真实大小写（展示用）。GetLongPathNameW 每路径
// 一次、Defender 环境实测 6-8ms/次（34 命中 = 250ms，800 行请求最坏秒级）。
// 改为按目录 FindFirstFileW 枚举一次拿到整目录「小写名→真名」映射并进程级缓存
// ——暖态查询零文件系统调用。缓存里的名字缺失（新建/改名）时查询侧自愈刷新，
// 无需监听线程失效同步。
using CaseMap = std::unordered_map<std::wstring, std::wstring>;
std::mutex g_caseMu;
std::unordered_map<std::wstring, std::shared_ptr<const CaseMap>> g_caseDirCache;

// 取目录的名字映射（键=全小写目录，无尾分隔符；盘根为 "c:\"）。refresh 强制重枚举。
// shared_ptr 保证外层 rehash/清理时已取出的映射仍然有效。
std::shared_ptr<const CaseMap> DiskDirNameMap(const std::wstring& lowDir, bool refresh = false) {
    {
        std::lock_guard<std::mutex> lk(g_caseMu);
        auto it = g_caseDirCache.find(lowDir);
        if (it != g_caseDirCache.end() && !refresh)
            return it->second;
    }
    const ULONGLONG tEnum = GetTickCount64();
    CaseMap m;
    std::wstring pat = lowDir;
    if (pat.back() != L'\\') pat += L'\\';
    pat += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                m.emplace(ToLower(fd.cFileName), fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
        fwprintf(stderr, L"[case] enum ms=%llu dir=%hs n=%zu\n",
                 (unsigned long long)(GetTickCount64() - tEnum),
                 WideToUtf8(lowDir).c_str(), m.size());
        fflush(stderr);
    }
    std::lock_guard<std::mutex> lk(g_caseMu);
    if (g_caseDirCache.size() > 8192) g_caseDirCache.clear();   // 粗粒度防膨胀
    auto& slot = g_caseDirCache[lowDir];
    if (!slot || refresh) slot = std::make_shared<const CaseMap>(std::move(m));
    return slot;
}

// blob 小写路径（fd 风格反斜杠已归一）→ 真实大小写。逐段查**父目录**的映射表
// （该目录枚举出的「子名→真名」）；目录不存在/缓存缺失该名（新建、改名）时
// 重枚举一次，仍缺则保留小写。
std::wstring DiskRealCasePath(const std::wstring& lowPath) {
    if (lowPath.size() < 4 || lowPath[1] != L':' || lowPath[2] != L'\\') return lowPath;
    std::wstring real;
    real += (wchar_t)towupper(lowPath[0]);
    real += L":\\";
    size_t pos = 3;
    while (pos < lowPath.size() && real.size() < 1024) {
        size_t slash = lowPath.find(L'\\', pos);
        if (slash == std::wstring::npos) slash = lowPath.size();
        const std::wstring comp = lowPath.substr(pos, slash - pos);
        if (!comp.empty()) {
            // 父目录 = 当前组件之前的路径（首个组件的父是盘根 "c:\"，键保留尾分隔符）
            std::wstring lowDir = lowPath.substr(0, pos);
            if (lowDir.size() > 3 && lowDir.back() == L'\\') lowDir.pop_back();
            auto m = DiskDirNameMap(lowDir);
            std::wstring realComp = comp;
            auto it = m ? m->find(comp) : CaseMap::iterator();
            if (m && it == m->end()) {   // 父目录表里没有该名：可能有新条目 → 刷新一次
                m = DiskDirNameMap(lowDir, /*refresh=*/true);
                it = m ? m->find(comp) : CaseMap::iterator();
            }
            if (m && it != m->end()) realComp = it->second;
            if (real.back() != L'\\') real += L'\\';
            real += realComp;
        }
        pos = slash + 1;
    }
    return real;
}

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
    // å¤´é¨ï¼"FTDISKIDX2\nCOUNT\t<10ä½æ°å­>\n"
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
    if (n == 0) {   // ç©ºç´¢å¼ï¼fd å¤±è´¥ç­ï¼ï¼è§ä¸ºä¸å¯ç¨ï¼äº¤ç±éå»º
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

void DiskPrefetchAsync();   // 定义在 DiskMaybeStartRebuild 之后（重建完成后新 blob 预热）

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

        // ä¸¤è¶å¨åï¼åç®å½åæä»¶ï¼+ ç»ä¸åªæï¼ä¸åå­ç´¢å¼åè§åï¼junk ä¸è¿ç´¢å¼ï¼ï¼
        // è§£ææé¶ GetFileAttributes è°ç¨ï¼ä¸ªå«ç¹æ®æä»¶åä¼ææ­»ï¼
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
        for (int pass = 0; okW && pass < 2; ++pass) {   // 0=ç®å½ 1=æä»¶
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
        // ååçå® COUNTï¼å®å®½ 10 ä½ï¼
        char cnt[11];
        snprintf(cnt, sizeof(cnt), "%010llu", count);
        LARGE_INTEGER off{};
        off.QuadPart = 11 + 6;
        SetFilePointer(h, off.LowPart, &off.HighPart, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(h, cnt, 10, &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);   // åå³ç¬å å¥æï¼å¦åæ å°çè¯»æå¼ä¼ sharing violation
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            dbg(L"rename failed:", GetLastError());
            break;
        }
        dbg(L"renamed:", count);
        {   // æ¢æ å° + æ¸ delta
            std::lock_guard<std::mutex> lk(g_diMu);
            DiskMapExistingLocked();
            std::lock_guard<std::mutex> lk2(g_ovMu);
            g_ovAdd.clear();
            g_ovDel.clear();
        }
        dbg(L"mapped, ready:", g_diReady.load() ? 1 : 0);
        if (g_diReady.load()) DiskPrefetchAsync();   // 新 blob 预热（避免重建完成后首查付冷页成本）
    } while (false);
    g_diRebuilding = false;
    return 0;
}

void DiskMaybeStartRebuild() {
    bool expect = false;
    if (g_diRebuilding.compare_exchange_strong(expect, true)) {   // æ¢å°å¯å¨æï¼çº¿ç¨ç»ææ¶èªæ¸é¶ï¼
        CloseHandle((HANDLE)_beginthreadex(nullptr, 0, DiskBuildThread, nullptr, 0, nullptr));
    }
}

// 磁盘索引冷页预热：blob 是文件映射页，守护进程重启后首查要为页缓存回填付
// 数百毫秒（实测 172-609ms，Defender 环境下波动）。顺序读文件只暖缓存管理器的
// 页，映射视图首触仍有软故障——PrefetchVirtualMemory 直接对映射视图预取才对症
// （Win8.1+，经 KernelBase 运行时解析；地址失效时优雅失败，不会像裸触碰那样崩）。
// 旧系统回落为顺序读文件（缓存管理器与映射视图共享同一批物理页，聊胜于无）。
unsigned __stdcall DiskPrefetchThread(void* p) {
    std::unique_ptr<std::wstring> path((std::wstring*)p);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    const ULONGLONG t0 = GetTickCount64();
    const unsigned char* base = nullptr;
    unsigned long long size = 0;
    {   // 快照映射范围（重建线程可能并发换映射；快照后不持锁，失效地址预取无害）
        std::lock_guard<std::mutex> lk(g_diMu);
        base = g_diView;
        size = g_diSize;
    }
    bool mapped = false;
    if (base && size) {
        struct MemRangeEntry { void* addr; SIZE_T bytes; };   // == MEMORY_RANGE_ENTRY（避免依赖 SDK 版本宏）
        using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, MemRangeEntry*, ULONG);
        HMODULE kb = GetModuleHandleW(L"KernelBase.dll");
        PrefetchFn fn = kb ? (PrefetchFn)GetProcAddress(kb, "PrefetchVirtualMemory") : nullptr;
        if (fn) {
            HANDLE proc = GetCurrentProcess();
            for (unsigned long long off = 0; off < size && !g_diQuit; off += 64ull << 20) {
                MemRangeEntry e{(void*)(base + off),
                                (SIZE_T)((size - off) > (64ull << 20) ? (64ull << 20) : (size - off))};
                if (!fn(proc, 1, &e, 0)) break;
            }
            mapped = true;
        }
    }
    if (!mapped) {
        HANDLE h = CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[1 << 16];
            DWORD got = 0;
            while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
                if (g_diQuit) break;
            }
            CloseHandle(h);
        }
    }
    if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
        fwprintf(stderr, L"[prefetch] ms=%llu map=%d\n",
                 (unsigned long long)(GetTickCount64() - t0), mapped ? 1 : 0);
        fflush(stderr);
    }
    return 0;
}
void DiskPrefetchAsync() {
    std::wstring path = DiskCachePath();
    if (path.empty()) return;
    CloseHandle((HANDLE)_beginthreadex(nullptr, 0, DiskPrefetchThread,
                                       new std::wstring(std::move(path)), 0, nullptr));
}

// æ¯çæ ¹ä¸ä¸ªçå¬çº¿ç¨ï¼åæ´è¿ RAM delta è¡¨ï¼æ¥è¯¢æ¶ä¸ blob åå¹¶ï¼
// æ¯çæ ¹ä¸ä¸ªçå¬çº¿ç¨ï¼åæ´è¿ RAM delta è¡¨ï¼æ¥è¯¢æ¶ä¸ blob åå¹¶ï¼ã
// éå¾ï¼GetFileAttributesWï¼ä¸ªå«è·¯å¾ä¼ææ­»ï¼ç»ä¸æéè°ç¨ãçè³ä¸å¨äºä»¶å¾ªç¯éåæ­¥è°ç¨ââ
// è§£æåºäºä»¶ååå¨æ¬å°æ éè§£æå±æ§ï¼åç­ææéå¥è¡¨ãææ­»åªå½±åè¯¥çæ°é²åº¦ï¼æ°¸ä¸å½±åæç´¢ã
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
            // 原地续挂（同句柄反复排队等待）：每批事件后 close+Sleep(200)+reopen 的
            // 空窗期内的事件会丢——磁盘索引靠 delta 表保新鲜度，丢了就是陈旧结果。
            if (!ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                       &bytes, nullptr, nullptr))
                break;
            if (bytes == 0) break;   // æº¢åºï¼äº¤ç»éå»ºå¨æ
            // åæ éè§£æäºä»¶ï¼å«å±æ§ï¼ï¼åç­ææéå¥è¡¨
            std::vector<std::pair<std::wstring, int>> resolved;   // path, isDir(-1=å é¤)
            const FILE_NOTIFY_INFORMATION* fni = (const FILE_NOTIFY_INFORMATION*)buf;
            for (;;) {
                std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                std::wstring path = root;
                if (path.back() != L'\\') path += L'\\';   // 盘根自带尾分隔符（"C:\"），别再加成 "C:\\Users"
                path += rel;
                int isDir = -1;
                if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                    DWORD attr = GetFileAttributesW(path.c_str());   // æ éï¼ææ­»åªæä½æ¬çº¿ç¨
                    if (attr != INVALID_FILE_ATTRIBUTES)
                        isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                }
                resolved.push_back({path, isDir});
                if (fni->NextEntryOffset == 0) break;
                fni = (const FILE_NOTIFY_INFORMATION*)((const char*)fni + fni->NextEntryOffset);
            }
            {
                std::lock_guard<std::mutex> lk(g_ovMu);   // ç­ææéï¼çº¯åå­æå¥
                for (auto& kv : resolved) {
                    std::wstring key = kv.first;
                    for (auto& c : key)
                        if (c == L'/') c = L'\\';
                    key = ToLower(key);   // 键统一小写反斜杠形，与 blob 命中过滤同形可比
                    if (kv.second < 0) { g_ovDel.insert(key); g_ovAdd.erase(key); }
                    else { g_ovAdd[key] = {kv.second != 0, kv.first}; g_ovDel.erase(key); }
                }
            }
            size_t ov = 0;
            { std::lock_guard<std::mutex> lk(g_ovMu); ov = g_ovAdd.size() + g_ovDel.size(); }
            if (ov > 50000 && !g_diRebuilding) DiskMaybeStartRebuild();   // delta å¤ªå¤§ï¼æ´ä½éå»º
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
    {   // æ å°å·²æç´¢å¼ï¼è¶è¿ 7 å¤©æªæ´æ°çç¼å­ç´æ¥è§ä¸ºä¸å­å¨ï¼åå°éå»ºï¼
        std::lock_guard<std::mutex> lk(g_diMu);
        g_diReady = DiskMapExistingLocked();
        if (g_diReady) {
            // è¿ææ£æ¥ï¼æä»¶æ¶é´ï¼1601 çºªåï¼å¯¹ç³»ç»æ¶é´ï¼åçºªåï¼ââ
            // ä¸è½ç¨ GetTickCount64ï¼å¼æºæ¯«ç§ï¼ï¼å¦åæ°¸è¿å¤å®ä¸º 150 å¤©å
            std::wstring path = DiskCachePath();
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
                ULONGLONG writeFt = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) |
                                    fa.ftLastWriteTime.dwLowDateTime;
                FILETIME nowFt{};
                GetSystemTimeAsFileTime(&nowFt);
                ULONGLONG now64 = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
                ULONGLONG ageMs = writeFt <= now64 ? (now64 - writeFt) / 10000 : 0;
                if (ageMs > 7ULL * 24 * 3600 * 1000) g_diReady = false;   // è¶ 7 å¤©ï¼éå»º
            }
        }
    }
    if (g_diReady) DiskPrefetchAsync();   // 冷页预热：首查的 218-500ms 页缓存回填挪到后台
    if (!g_diReady) DiskMaybeStartRebuild();
    for (const auto& r : DriveRoots()) {
        CloseHandle((HANDLE)_beginthreadex(nullptr, 0, DiskWatchThread,
                                           new std::wstring(r), 0, nullptr));
    }
}

bool DiskIndexReady() { return g_diReady.load(); }

size_t DiskIndexRecords() { return g_diReady ? g_diRecords : 0; }

// å¨ blob é¦å­è memchr æ«æ + è¡çº§ AND æ ¡éªï¼delta è¡¨åå¹¶å¢å 
int DiskIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows) {
    out.clear();
    ULONGLONG t0 = GetTickCount64();
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
    std::vector<std::string> wl;                       // å°å UTF-8 è¯
    std::vector<std::wstring> wlw;                     // å®½è¯ï¼delta è¡¨ç¨ï¼
    for (const auto& w : q.words) {
        std::string u = Utf8LowerAscii(WideToUtf8(w));
        wl.push_back(u);
        wlw.push_back(ToLower(w));
    }
    std::vector<std::string> ex;                       // æé¤è¯ï¼å°å UTF-8ï¼
    for (const auto& e : q.excludes) ex.push_back(Utf8LowerAscii(WideToUtf8(e)));

    std::unordered_set<std::string> seen;              // å»éï¼fd å¯¹ junction å¯è½éå¤æä¸¾ï¼
    std::vector<std::pair<std::wstring, bool>> found;   // 锁内先收集小写路径
    {   // GetLongPathNameW 属文件系统调用（个别路径会挂死），绝不持 g_diMu 调用
        std::lock_guard<std::mutex> lk(g_diMu);
        if (!g_diView) return -1;
        dbg(L"locked, size:", g_diSize);
        const unsigned char* base = g_diView;
        const unsigned long long total = g_diSize;
        const std::string& w0 = wl[0];
        const unsigned char c0 = (unsigned char)w0[0];
        for (const unsigned char* p = base; found.size() < (size_t)maxRows; ++p) {
            if ((unsigned long long)(p - base) + w0.size() > total) break;
            p = (const unsigned char*)memchr(p, c0, (size_t)(total - (p - base)));
            if (!p) break;
        // åå¨è¯ memcmp ç¡®è®¤å½ä¸­ï¼å»ä»·ï¼ï¼ååéè¡é¦ï¼åéæ¯æ¯å½ä¸­æè´µçæä½ââ
        // é¦å­èå½ä¸­çæé«ï¼åç¡®è®¤å¯æåéæ¬¡æ°ä»æ°ç¾ä¸éå°å½ä¸­æ°ï¼
        if ((unsigned long long)(total - (p - base)) < w0.size()) break;
        if (memcmp(p, w0.data(), w0.size()) != 0) continue;
        // è¡è¾¹çï¼å½ä¸­ç¹åéå°ä¸ä¸è¡é¦ãåè¿å°è¡å°¾
        const unsigned char* ls = p;
        while (ls > base && ls[-1] != '\n') --ls;
        const unsigned char* le = p + w0.size();
        while (le < base + total && *le != '\n') ++le;
        const size_t recLen = (size_t)(le - ls);
        if (recLen < 2) continue;
        const bool isDir = ls[0] == 'D';
        if (q.isFile == isDir) continue;
        const unsigned char* rec = ls + 1;
        const size_t pathLen = recLen - 1;
        // åå­æ¨¡å¼ï¼è¯å¿é¡»è½å¨æåä¸æ®µï¼åºåï¼åï¼è·¯å¾æ¨¡å¼ï¼æ´æ¡è·¯å¾
        const unsigned char* seg = rec;
        size_t segLen = pathLen;
        if (!pathMode) {
            for (size_t i = pathLen; i > 0; --i)
                if (rec[i - 1] == '/') { seg = rec + i; segLen = pathLen - i; break; }
        }
        if (!MemContainsBytes(seg, segLen, w0.data(), w0.size())) continue;   // å½ä¸­è·¨è¡/è·¨æ®µç­åé³æ§
        bool ok = true;
        for (size_t i = 1; i < wl.size(); ++i)
            if (!MemContainsBytes(seg, segLen, wl[i].data(), wl[i].size())) { ok = false; break; }
        if (!ok) continue;
        for (const auto& e : ex)
            if (!e.empty() && MemContainsBytes(rec, pathLen, e.data(), e.size())) { ok = false; break; }
        if (!ok) continue;
        if (!seen.insert(std::string((const char*)rec, pathLen)).second) continue;
        std::wstring path = Utf8ToWide(std::string((const char*)rec, pathLen));
        {   // delta 合并：已删除跳过；已是新增条目则交给下方 additions 扫描，避免重复。
            // 键为小写反斜杠形，与 blob 路径（小写正斜杠）只差分隔符
            std::wstring key = path;
            for (auto& c : key)
                if (c == L'/') c = L'\\';
            std::lock_guard<std::mutex> lk2(g_ovMu);
            if (g_ovDel.count(key)) continue;
            if (g_ovAdd.count(key)) continue;
        }
        found.push_back({std::move(path), isDir});
        }
    }   // g_diMu 释放
    // blob 存的是小写路径：还原真实大小写（目录级映射缓存，暖态零文件系统调用；
    // 文件已删时保留小写）。fd 风格正斜杠先归一为反斜杠，与其他层/UI 一致
    for (auto& f : found) {
        if (out.size() >= (size_t)maxRows) break;
        for (auto& c : f.first)
            if (c == L'/') c = L'\\';
        f.first = DiskRealCasePath(f.first);
        if (f.first.size() >= 2 && f.first[1] == L':' &&
            f.first[0] >= L'a' && f.first[0] <= L'z')
            f.first[0] -= L'a' - L'A';   // 盘符兜底大写（DiskRealCasePath 已做）
        out.push_back({std::move(f.first), f.second});
    }
    // å»ºåºåæ°å¢æ¡ç®ï¼RAM deltaï¼ï¼æ°éå°ï¼å®½å­ç¬¦çº¿æ§è¿æ»¤
    std::lock_guard<std::mutex> lk2(g_ovMu);
    for (const auto& kv : g_ovAdd) {
        if (out.size() >= (size_t)maxRows) break;
        if (q.isFile == kv.second.first) continue;
        const std::wstring& lp = kv.first;   // 已是小写反斜杠
        bool ok = true;
        for (const auto& w : wlw)
            if (lp.find(w) == std::wstring::npos) { ok = false; break; }
        if (!ok) continue;
        for (const auto& e : q.excludes)
            if (!e.empty() && lp.find(e) != std::wstring::npos) { ok = false; break; }
        if (!ok) continue;
        const std::wstring& real = kv.second.second;   // 真实大小写路径（展示用）
        bool hit = true;
        if (!pathMode) {
            std::wstring base = real;
            size_t bp = base.find_last_of(L'\\');
            base = bp == std::wstring::npos ? base : base.substr(bp + 1);
            std::wstring bl = ToLower(base);
            for (const auto& w : wlw)
                if (bl.find(w) == std::wstring::npos) { hit = false; break; }
        } else {
            std::wstring fl = ToLower(real);
            for (const auto& w : wlw)
                if (fl.find(w) == std::wstring::npos) { hit = false; break; }
        }
        if (!hit) continue;
        out.push_back({real, kv.second.first});
    }
    dbg(L"scan ms:", (unsigned long long)(GetTickCount64() - t0));
    return (int)out.size();
}

// ---------------- 守护进程协议 I/O（客户端/服务端共用） ----------------
// 逐字节 recv 在 200 行结果规模下是上万次系统调用（客户端读结果 + 服务端读词条
// 各一遍），是小包扫描之外最大的延迟项——改为 4KB 块收、行从块里切。
struct LineReader {
    SOCKET s;
    char buf[4096];
    size_t pos = 0, len = 0;
    explicit LineReader(SOCKET sock) : s(sock) {}
    // 读一行（剥 \r\n）。对端断开/收包超时/超长（> maxLen）返回 false，
    // line 保留已收到的部分（与原逐字节版语义一致）。
    bool next(std::string& line, size_t maxLen = 4096) {
        line.clear();
        for (;;) {
            while (pos < len) {
                char b = buf[pos++];
                if (b == '\n') return true;
                if (b != '\r') line += b;
                if (line.size() > maxLen) return false;
            }
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            pos = 0;
            len = (size_t)n;
        }
    }
};

// 循环 send 直到收完（阻塞 socket 单次 send 可能只收一部分；对端断开返回 false）
bool SendAll(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}


// ---------------- æç´¢å®æ¤è¿ç¨ï¼åå­/ç£çç´¢å¼çå®¿ä¸»è¿ç¨ï¼ ----------------
// flowtary-search.exe --serve å¸¸é©»ï¼ææåå­ç´¢å¼æ±  + ç£ç blob æ å° + çå¬çº¿ç¨ï¼
// flowtary.exe åªå TCP å®¢æ·ç«¯ãç´¢å¼åå­å¨é¨è®°å¨å®æ¤è¿ç¨åä¸ï¼å¯¹æ¯ Everything ä¸ç®äºç¶ï¼ï¼
// å®æ¤è¿ç¨å´©æº/éåºé½ä¸å½±å flowtaryï¼æ¥è¯¢è½ fd/WS å±ï¼éæå®æ¤è¿ç¨ç§çº§æ¢å¤ï¼ã

namespace dfdaemon {

constexpr u_long kDaemonPort = 47771;                     // FT_DF_DAEMON_PORT å¯è¦ç
constexpr DWORD kDaemonIdleExitMs = 30ULL * 60 * 1000;    // ç©ºé² 30 åéèªå¨éåºï¼ç¼å­å·²è½çï¼

u_long DaemonPort() {
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"FT_DF_DAEMON_PORT", env, 16) > 0) {
        u_long v = (u_long)_wtoi(env);
        if (v || env[0] == L'0') return v;
    }
    return kDaemonPort;
}

void DaemonStatsReply(SOCKET s);   // 定义在下方第二个 dfdaemon 块（DaemonHandleConn 转 STAT 用）

}  // namespace

// åè¿æ¥å¤çï¼è¯»æ¥è¯¢ â mem+disk åæåå¹¶å»é â å N/è¡/END
void DaemonHandleConn(SOCKET s) {
    DWORD rcvTo = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTo, sizeof(rcvTo));
    LineReader in(s);
    std::string meta;
    if (!in.next(meta)) return;
    if (meta == "STAT") { dfdaemon::DaemonStatsReply(s); return; }   // 状态查询（此前漏接：STAT 永远无应答，客户端恒判守护进程离线）
    auto dbg = [](const char* m) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
            fwprintf(stderr, L"[conn] %hs\n", m);
            fflush(stderr);
        }
    };
    int isFile = 1, pathMode = 0, maxRows = 200, nwords = 0, nexcl = 0;
    if (sscanf(meta.c_str(), "Q %d %d %d %d %d", &isFile, &pathMode, &maxRows, &nwords, &nexcl) != 5) {
        dbg("meta parse failed");
        return;
    }
    if (nwords < 0 || nwords > 16 || nexcl < 0 || nexcl > 16 || maxRows < 1 || maxRows > 1000) {
        dbg("meta validation failed");
        return;
    }
    Query q{};
    q.isFile = isFile != 0;
    auto readLines = [&](int n, std::vector<std::wstring>& out) {
        for (int i = 0; i < n; ++i) {
            std::string ln;
            if (!in.next(ln)) return false;
            out.push_back(Utf8ToWide(ln));
        }
        return true;
    };
    if (!readLines(nwords, q.words)) return;
    if (!readLines(nexcl, q.excludes)) return;

    std::vector<Hit> hits;
    if (MemIndexSearch(q, false, hits, (DWORD)maxRows) == 0)
        MemIndexSearch(q, true, hits, (DWORD)maxRows);           // 名字零命中 → 路径兜底
    std::vector<Hit> diskHits;
    if ((int)hits.size() < maxRows) {   // mem 已拿满：磁盘层条目合并时必然全部丢弃，跳过 90MB blob 扫描
        if (DiskIndexSearch(q, false, diskHits, (DWORD)maxRows) == 0)
            DiskIndexSearch(q, true, diskHits, (DWORD)maxRows);
    }
    // åå¹¶å»éï¼å°å UTF-8 é®ï¼ï¼mem ä¼å
    std::unordered_set<std::string> seen;
    std::vector<Hit> merged;
    for (Hit& h : hits) {
        if (!seen.insert(WideToUtf8(ToLower(h.path))).second) continue;
        merged.push_back(std::move(h));
        if (merged.size() >= (size_t)maxRows) break;
    }
    if (merged.size() < (size_t)maxRows) {
        for (Hit& h : diskHits) {
            if (!seen.insert(WideToUtf8(ToLower(h.path))).second) continue;
            merged.push_back(std::move(h));
            if (merged.size() >= (size_t)maxRows) break;
        }
    }

    // 响应整体拼一块一次 send：此前逐条 send（200 命中 = 200+ 小包）叠加客户端
    // 逐字节 recv，是扫描之外最大的延迟项（扫描 35ms vs 整链 78-250ms）
    std::string resp;
    char head[64];
    snprintf(head, sizeof(head), "N %d\n", (int)merged.size());
    resp += head;
    for (const auto& h : merged) {
        resp += h.isFolder ? 'D' : 'F';
        resp += WideToUtf8(h.path);
        resp += '\n';
    }
    resp += "END\n";
    SendAll(s, resp.data(), (int)resp.size());
}

namespace dfdaemon {

void DaemonStatsReply(SOCKET s) {
    int memReady = MemIndexReady() ? 1 : 0;
    int memSize = (int)MemIndexSize();
    int diskReady = DiskIndexReady() ? 1 : 0;
    int diskRecords = (int)DiskIndexRecords();
    char line[128];
    snprintf(line, sizeof(line), "M %d %d\n", memReady, memSize);
    std::string resp = line;
    snprintf(line, sizeof(line), "D %d %d\n", diskReady, diskRecords);
    resp += line;
    resp += "END\n";
    SendAll(s, resp.data(), (int)resp.size());
}

// æå¡ä¸»å¾ªç¯ï¼select 60s è¶æ¶è½®è½¬ï¼ç»è®¡ç©ºé²ï¼ï¼ç©ºé² 30 åéèªå¨éåº
// 每连接一个短命线程的载体：SOCKET + 空闲计时指针（线程结束后刷新）
struct ConnTask {
    SOCKET s;
    std::atomic<ULONGLONG>* lastActive;
};

unsigned __stdcall ConnThreadProc(void* p) {
    std::unique_ptr<ConnTask> t((ConnTask*)p);
    DaemonHandleConn(t->s);
    closesocket(t->s);
    t->lastActive->store(GetTickCount64());
    return 0;
}

int Serve(const std::vector<std::wstring>& extraRoots) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { WSACleanup(); return 1; }
    // ç»å®åæ¢æµï¼ç«¯å£ä¸å·²æå®æ¤è¿ç¨å¨æå¡åç´æ¥éåºï¼ç¦æ­¢ SO_REUSEADDRââ
    // ä¸¤ä¸ªå®æ¤è¿ç¨å±äº«ç«¯å£ä¼äºç¸æ¢è¿æ¥ï¼ç¶ææ¥è¯¢æå°å¡æ­»çæ§å®ä¾ä¸ï¼
    {
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe != INVALID_SOCKET) {
            sockaddr_in pa{};
            pa.sin_family = AF_INET;
            pa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            pa.sin_port = htons((u_short)DaemonPort());
            if (connect(probe, (const sockaddr*)&pa, sizeof(pa)) == 0) {
                closesocket(probe);
                WSACleanup();
                return 0;   // 已有守护进程在跑，不重复启动
            }
            closesocket(probe);
        }
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)DaemonPort());
    if (bind(ls, (const sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(ls, 4) != 0) {
        closesocket(ls);
        WSACleanup();
        return 1;
    }
    MemIndexStart(extraRoots);
    DiskIndexStart();

    // 每连接一个短命线程：STAT/诊断连接不再排队在长查询（全 blob 扫描可达 1-3s+）
    // 后面——否则 UI 线程的建库状态轮询会被 2s 收包超时整个卡住。连接间不共享
    // 可变状态（并发保护在 MemIndexSearch/DiskIndexSearch 的内部锁），线程安全。
    std::atomic<ULONGLONG> lastActive{GetTickCount64()};
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        timeval tv{60, 0};
        int r = select((int)ls + 1, &rs, nullptr, nullptr, &tv);
        if (r <= 0) {
            if (GetTickCount64() - lastActive.load() > kDaemonIdleExitMs) break;   // 空闲退出
            continue;
        }
        sockaddr_in cli{};
        int cl = sizeof(cli);
        SOCKET cs = accept(ls, (sockaddr*)&cli, &cl);
        if (cs == INVALID_SOCKET) continue;
        BOOL nd = 1;   // 环回上 Nagle+延迟 ACK 对小包可叠加出百毫秒级 stall；协议已是整包发送，关掉无副作用
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        lastActive.store(GetTickCount64());
        auto* conn = new ConnTask{cs, &lastActive};
        HANDLE th = (HANDLE)_beginthreadex(nullptr, 0, ConnThreadProc, conn, 0, nullptr);
        if (th) CloseHandle(th);   // 关句柄只是解除持有，线程继续跑
        else {                     // 线程创建失败：退回串行处理，连接不丢
            DaemonHandleConn(cs);
            closesocket(cs);
        }
    }

    closesocket(ls);
    WSACleanup();
    return 0;
}  // namespace dfdaemon
}


// å®æ¤è¿ç¨æå¡ä¸»å¾ªç¯å¥å£ï¼flowtary-search --serve è¿ç¨è°ç¨ï¼
int SearchDaemonServe(const std::vector<std::wstring>& extraRoots) {
    MemPinyinStart();   // 毫秒层拼音索引（exe 同目录无 dict/ 时静默降级为纯名字匹配）
    return dfdaemon::Serve(extraRoots);
}

// 客户端：确保守护进程在线（不在线拉起同目录 flowtary-search.exe --serve 并等 ~1s）。
// UI 线程逐键调用：限频 3s 一次，就绪等待 1s 封顶——起不来的病态场景不能把按键卡成秒级。
void SearchDaemonStart() {
    int mr = 0, ms = 0, dr = 0, drs = 0;
    if (SearchDaemonStat(mr, ms, dr, drs)) return;   // 已在线
    // 拉起限频：exe 在但始终起不来时（端口被占/安全软件拦杀），每次按键都
    // CreateProcess + 白等会把 UI 线程反复卡住——3s 内只拉起一次。
    static std::atomic<ULONGLONG> s_lastSpawn{0};
    const ULONGLONG nowTick = GetTickCount64();
    ULONGLONG lastTick = s_lastSpawn.load(std::memory_order_relaxed);
    if (lastTick != 0 && nowTick - lastTick < 3000) return;
    if (!s_lastSpawn.compare_exchange_strong(lastTick, nowTick,
                                             std::memory_order_relaxed)) return;
    WCHAR self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
    std::wstring dir(self);
    size_t p = dir.find_last_of(L"\\/");
    std::wstring exe = (p == std::wstring::npos ? dir : dir.substr(0, p + 1)) + L"flowtary-search.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring cmd = L"\"" + exe + L"\" --serve";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 5; ++i) {   // 等 ~1s 就绪（bind 在建库线程之前，通常 ≤200ms）；
        Sleep(200);                 // 首查没赶上也无妨：fd/WS 兜底，下次按键已在线
        if (SearchDaemonStat(mr, ms, dr, drs)) return;
    }
}

// å®¢æ·ç«¯æ¥è¯¢ï¼è¿æ¥ â ååè®® â æ¶ç»æãå¤±è´¥è¿å falseï¼è°ç¨æ¹è½ fd/WS å±ï¼
bool SearchDaemonQuery(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows) {
    out.clear();
    auto dbg = [](const wchar_t* m, unsigned long long v) {
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0) {
            fwprintf(stderr, L"[qcli] %s %llu\n", m, (unsigned long long)v);
            fflush(stderr);
        }
    };
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool ok = false;
    do {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { dbg(L"socket failed", 0); break; }
        DWORD to = 30000;   // å¨ blob æ«æå¯è½ 1-3sãå·é¡µé¦¬¬æ´ä¹ï¼30s ä½é
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
        BOOL nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)dfdaemon::DaemonPort());
        if (connect(s, (const sockaddr*)&addr, sizeof(addr)) != 0) { dbg(L"connect failed", WSAGetLastError()); closesocket(s); break; }
        // 请求一次 send（头 + 词条 + 排除词）：此前逐条小包 send，叠加服务端逐字节
        // recv 是扫描之外最大的延迟项
        std::string req;
        char head[80];
        snprintf(head, sizeof(head), "Q %d %d %d %d %d\n", q.isFile ? 1 : 0, pathMode ? 1 : 0,
                 (int)maxRows, (int)q.words.size(), (int)q.excludes.size());
        req += head;
        for (const auto& w : q.words) { req += WideToUtf8(w); req += '\n'; }
        for (const auto& e : q.excludes) { req += WideToUtf8(e); req += '\n'; }
        if (!SendAll(s, req.data(), (int)req.size())) {
            dbg(L"send request failed", WSAGetLastError());
            closesocket(s);
            break;
        }

        // 整个响应用同一个 LineReader：缓冲式读取的块内数据跨行存活——
        // 头部和结果行各建一个 reader 的话，头部 reader 析构时缓冲里已收到的
        // 结果行会全部丢失（服务端整包 send 后，剩余数据只在对端 FIN 才醒）
        LineReader in(s);
        std::string acc;
        int n = -1;
        if (in.next(acc, 64)) {   // 读 "N count"。读不到合法头 = 协议错位/守护进程异常：整体按失败处理，
                                  // 调用方落 fd/WS 兜底（原先把垃圾当 0 命中且 ok=true，兜底被吞）
            int v = -1;
            if (sscanf(acc.c_str(), "N %d", &v) == 1 && v >= 0) n = v;
        }
        if (n < 0) {
            dbg(L"bad header, bytes:", acc.size());
            closesocket(s);
            break;   // ok 保持 false
        }
        int got = 0;
        std::unordered_set<std::string> seen;
        for (int i = 0; i < n && got < (int)maxRows; ++i) {
            std::string ln;
            if (!in.next(ln)) break;   // 对端断开/超时：收多少算多少
            if (ln.size() < 2) continue;
            bool isFolder = ln[0] == 'D';
            std::wstring path = Utf8ToWide(ln.substr(1));
            if (path.size() < 3 || path[1] != L':') continue;   // 守护进程只回 X:\ 绝对路径
            if (!seen.insert(ln.substr(1)).second) continue;
            out.push_back({path, isFolder});
            ++got;
        }
        ok = true;
        closesocket(s);
    } while (false);
    WSACleanup();
    return ok;
}

// å®¢æ·ç«¯ç¶ææ¥è¯¢ï¼wsquery --memstat/--diskstat ç¨ï¼
bool SearchDaemonStat(int& memReady, int& memSize, int& diskReady, int& diskRecords) {
    memReady = memSize = diskReady = diskRecords = 0;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool ok = false;
    do {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) break;
        DWORD to = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)dfdaemon::DaemonPort());
        if (connect(s, (const sockaddr*)&addr, sizeof(addr)) != 0) { closesocket(s); break; }
        if (send(s, "STAT\n", 5, 0) != 5) { closesocket(s); break; }
        std::string acc;
        int lines = 0;
        for (;;) {
            char b = 0;
            int r = recv(s, &b, 1, 0);
            if (r <= 0) break;
            if (b == '\n') {
                if (lines == 0 && sscanf(acc.c_str(), "M %d %d", &memReady, &memSize) == 2) ++lines;
                else if (lines == 1 && sscanf(acc.c_str(), "D %d %d", &diskReady, &diskRecords) == 2) ++lines;
                acc.clear();
                if (lines >= 2) { ok = true; break; }
                continue;
            }
            acc += b;
            if (acc.size() > 128) break;
        }
        closesocket(s);
    } while (false);
    WSACleanup();
    return ok;
}

}  // namespace df
