// Queries the existing Windows Search catalog only. No filesystem enumeration or indexing.
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
#include <cwctype>
#include <cstdio>
#include <string>
#include <vector>


#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace df {
namespace {

constexpr DWORD kWsFetchRows = 400;  // çå®æ¥è¯¢æåä¸éï¼è¿æ»¤/æåºååªå±ç¤ºå 10ï¼

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

// Balance each apartment before its thread exits.
struct ThreadComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ThreadComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Cache OLE DB interfaces only in the apartment that created them.
struct WsCache {
    ThreadComScope apartment;
    IDBInitialize* init = nullptr;
    IDBCreateCommand* cmdFactory = nullptr;
    ~WsCache() { reset(); }
    bool ready() const { return init && cmdFactory; }
    void reset() {
        if (cmdFactory) { cmdFactory->Release(); cmdFactory = nullptr; }
        if (init) { init->Release(); init = nullptr; }
    }
};
WsCache& ThreadWsCache() {
    static thread_local WsCache cache;
    return cache;
}

void WsStep(std::wstring* diag, const wchar_t* stage, HRESULT hr) {
    if (diag) {
        wchar_t buf[64];
        swprintf_s(buf, L"0x%08lX", (unsigned long)hr);
        *diag += std::wstring(stage) + L"=" + buf + L" ";
    }
}

// å»ºä¼è¯ï¼è°ç¨æ¹å·²æéï¼ãè¿å false æ¶ *hrOut ä¸ºé¦ä¸ªå¤±è´¥ç¹ã
bool WsBuildSession(std::wstring* diag, HRESULT* hrOut) {
    auto& cache = ThreadWsCache();
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"Search.CollatorDSO", &clsid);
    WsStep(diag, L"CLSID", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IDBInitialize,
                          (void**)&cache.init);
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
        bool setOk = SUCCEEDED(cache.init->QueryInterface(IID_IDBProperties, (void**)&props.p)) &&
                     SUCCEEDED(props->SetProperties(1, &set));
        VariantClear(&prop.vValue);
        if (!setOk) { WsStep(diag, L"PROP", E_FAIL); *hrOut = E_FAIL; return false; }
    }
    hr = cache.init->Initialize();
    WsStep(diag, L"INIT", hr);
    if (FAILED(hr)) { *hrOut = hr; return false; }
    CI<IDBCreateSession> sessFactory;
    if (FAILED(cache.init->QueryInterface(IID_IDBCreateSession, (void**)&sessFactory.p))) {
        WsStep(diag, L"QI", E_FAIL); *hrOut = E_FAIL; return false;
    }
    hr = sessFactory->CreateSession(nullptr, IID_IDBCreateCommand, (IUnknown**)&cache.cmdFactory);
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
    const std::wstring sql = L"SELECT TOP " + std::to_wstring(maxRows) +
                             L" System.ItemUrl, System.FileName, System.ItemType"
                             L" FROM SystemIndex WHERE " + where;
    // Ensure COM outlives this thread's cached provider/session.
    auto& cache = ThreadWsCache();
    int result = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {  // ä¼è¯å¤±æï¼æå¡åéå¯ç­ï¼ï¼éå»ºåéè¯ä¸æ¬¡
        // ä¼è¯ç¼å­ï¼provider/ä¼è¯åªå»ºä¸æ¬¡ï¼æ¥è¯¢å¤±è´¥éå»ºä¸æ¬¡åè¯ï¼æå¡éå¯ç­æåµï¼
        HRESULT hr = S_OK;
        HRESULT buildHr = S_OK;
        if (!cache.ready()) {
            cache.reset();
            if (!WsBuildSession(diag, &buildHr)) { cache.reset(); break; }
        }
        CI<ICommandText> cmd;
        hr = cache.cmdFactory->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)&cmd.p);
        WsStep(diag, L"CMD", hr);
        if (FAILED(hr)) { cache.reset(); continue; }   // éå»ºä¼è¯åèµ°ä¸é
        hr = cmd->SetCommandText(DBGUID_DBSQL, sql.c_str());
        WsStep(diag, L"SETTEXT", hr);
        if (FAILED(hr)) break;

        DBROWCOUNT affected = 0;
        CI<IUnknown> rsUnk;
        hr = cmd->Execute(nullptr, IID_IRowset, nullptr, &affected, &rsUnk.p);
        WsStep(diag, L"EXEC", hr);
        if (FAILED(hr)) {
            if (!diag) { cache.reset(); continue; }    // éé»éè¯ä¸æ¬¡ï¼æå¡å¯è½åéå¯ï¼
            break;
        }
        CI<IRowset> rs;
        if (FAILED(rsUnk->QueryInterface(IID_IRowset, (void**)&rs.p))) { WsStep(diag, L"RS", E_FAIL); break; }

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

        struct BoundRow {
            alignas(void*) unsigned char data[ROW_BUF]{};
            ~BoundRow() {
                // DBMEMOWNER_CLIENTOWNED | BYREF: the consumer owns all returned strings.
                for (size_t offset : {size_t(OFF_URL), size_t(OFF_NAME), size_t(OFF_TYPE)})
                    CoTaskMemFree(*reinterpret_cast<void**>(data + offset));
            }
        };
        HROW* prow = nullptr;   // GetNextRows åºåï¼æå¡ç«¯å¡«è¡å¥ææ°ç»
        bool rowsOk = true;
        for (;;) {
            DBCOUNTITEM got = 0;
            prow = nullptr;
            hr = rs->GetNextRows(DB_NULL_HCHAPTER, 0, 64, &got, &prow);
            if (FAILED(hr) || got == 0) {
                if (got && prow) rs->ReleaseRows(got, prow, nullptr, nullptr, nullptr);
                CoTaskMemFree(prow);
                rowsOk = SUCCEEDED(hr);
                break;
            }
            for (DBCOUNTITEM i = 0; i < got; ++i) {
                BoundRow row;
                auto* rowBuf = row.data;
                if (FAILED(rs->GetData(prow[i], hacc, rowBuf))) { rowsOk = false; break; }
                // BYREF ç»å®ï¼å¼ä½æ¯ wchar_t* æéï¼ReleaseRows åå¿é¡»åèµ°
                const wchar_t* url = *(wchar_t* const*)(rowBuf + OFF_URL);
                const wchar_t* name = *(wchar_t* const*)(rowBuf + OFF_NAME);
                const wchar_t* type = *(wchar_t* const*)(rowBuf + OFF_TYPE);
                auto colOk = [&](unsigned short statusOff) {
                    DBSTATUS s = *reinterpret_cast<DBSTATUS*>(rowBuf + statusOff);
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
            CoTaskMemFree(prow);
            if (!rowsOk || out.size() >= maxRows) break;
        }
        if (hacc) acc->ReleaseAccessor(hacc, nullptr);
        if (!rowsOk) out.clear();
        result = rowsOk ? (int)out.size() : -1;
        break;   // æåï¼ä¸åéè¯
    }

    return result;
}

} // namespace

bool WindowsSearchAvailable() {
    Query q; q.words = {L"a"};
    std::vector<Hit> hits;
    return WsQueryImpl(q, false, hits, 1, nullptr) >= 0;
}

std::wstring DiagProbe() {
    Query q; q.words = {L"a"};
    std::vector<Hit> hits; std::wstring diag;
    WsQueryImpl(q, false, hits, 1, &diag);
    return diag;
}

int WindowsSearchQuery(const Query& q, bool pathMode, std::vector<Hit>& out) {
    return WsQueryImpl(q, pathMode, out, kWsFetchRows, nullptr);
}

} // namespace df
