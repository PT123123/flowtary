#pragma once
// Small launch catalog: known Start Menu roots, App Paths registration and
// AppsFolder. Never walks installation trees or resolves shortcut targets.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ft::apps {
struct Entry { std::wstring name, path, target; bool startMenu=false; };
struct Limits {
    size_t maxEntries=4096, maxInspected=8192, maxDirectories=256;
    unsigned maxDepth=6;
    ULONGLONG maxMilliseconds=1500;
    size_t maxEstimatedBytes=8*1024*1024;
};
struct Stats {
    size_t inspected=0, directories=0, registrationKeys=0, shellItems=0, duplicates=0;
    ULONGLONG elapsedMilliseconds=0;
    size_t estimatedBytes=0;
    bool truncated=false, cancelled=false;
    std::vector<std::pair<std::wstring,LONG>> errors;
};
struct Result { std::vector<Entry> entries; Stats stats; };
inline std::wstring Lower(std::wstring s) { for(auto& c:s)c=(wchar_t)towlower(c); return s; }
inline bool EndsExe(const std::wstring& s) { return s.size()>=4 && Lower(s.substr(s.size()-4))==L".exe"; }
struct RegKey {
    HKEY value=nullptr;
    ~RegKey(){if(value)RegCloseKey(value);}
    RegKey()=default; RegKey(const RegKey&)=delete; RegKey& operator=(const RegKey&)=delete;
};
template<class T> struct Com {
    T* value=nullptr;
    ~Com(){if(value)value->Release();}
    Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
};
struct Find {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Find(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);}
};

class Builder {
    Limits limits;
    const std::atomic<bool>* stop;
    ULONGLONG started=GetTickCount64();
    std::unordered_set<std::wstring> names;
    bool memoryLimitReached=false;
public:
    Result result;
    explicit Builder(Limits l={},const std::atomic<bool>* cancelled=nullptr):limits(l),stop(cancelled){}
    bool Continue() {
        if(stop && stop->load(std::memory_order_acquire)) { result.stats.cancelled=true; return false; }
        if(memoryLimitReached || result.stats.estimatedBytes>=limits.maxEstimatedBytes || result.entries.size()>=limits.maxEntries || result.stats.inspected>=limits.maxInspected ||
           GetTickCount64()-started>=limits.maxMilliseconds) { result.stats.truncated=true; return false; }
        return true;
    }
    void Error(const std::wstring& source,LONG code) {
        if(result.stats.errors.size()<32)result.stats.errors.emplace_back(source,code);
    }
    void Add(Entry e) {
        if(!Continue() || e.name.empty() || e.path.empty())return;
        auto key=Lower(e.name);
        if(names.count(key)){++result.stats.duplicates;return;}
        size_t estimate=sizeof(Entry)+(e.name.capacity()+e.path.capacity()+e.target.capacity()+key.capacity())*sizeof(wchar_t)+64;
        if(estimate>limits.maxEstimatedBytes-result.stats.estimatedBytes){memoryLimitReached=true;result.stats.truncated=true;return;}
        names.insert(std::move(key));result.stats.estimatedBytes+=estimate;
        result.entries.push_back(std::move(e));
    }
    void StartMenu(const std::wstring& root,unsigned depth=0) {
        if(!Continue())return;
        if(depth>limits.maxDepth || result.stats.directories>=limits.maxDirectories){result.stats.truncated=true;return;}
        // A known root may itself have been replaced with a junction.
        DWORD attr=GetFileAttributesW(root.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES)return;
        if(!(attr&FILE_ATTRIBUTE_DIRECTORY) || (attr&FILE_ATTRIBUTE_REPARSE_POINT))return;
        ++result.stats.directories;
        WIN32_FIND_DATAW data{};
        Find handle; handle.value=FindFirstFileExW((root+L"\\*").c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,0);
        if(handle.value==INVALID_HANDLE_VALUE){
            DWORD code=GetLastError(); if(code!=ERROR_FILE_NOT_FOUND && code!=ERROR_PATH_NOT_FOUND)Error(root,code); return;
        }
        do {
            if(!Continue())break;
            std::wstring name=data.cFileName;
            if(name==L"." || name==L"..")continue;
            ++result.stats.inspected;
            if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)continue;
            std::wstring full=root+L"\\"+name;
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)StartMenu(full,depth+1);
            else if(name.size()>4 && Lower(name.substr(name.size()-4))==L".lnk")
                Add({name.substr(0,name.size()-4),std::move(full),{},true});
        }while(FindNextFileW(handle.value,&data));
    }
    static std::wstring RegistrationPath(std::wstring path,DWORD type) {
        if(type!=REG_SZ && type!=REG_EXPAND_SZ)return {};
        if(type==REG_EXPAND_SZ){
            DWORD n=ExpandEnvironmentStringsW(path.c_str(),nullptr,0);
            if(!n || n>32768)return {};
            std::vector<wchar_t> buffer(n);
            if(ExpandEnvironmentStringsW(path.c_str(),buffer.data(),n)!=n)return {};
            path.assign(buffer.data());
        }
        auto begin=path.find_first_not_of(L" \t\r\n"); if(begin==std::wstring::npos)return {};
        path=path.substr(begin,path.find_last_not_of(L" \t\r\n")-begin+1);
        if(path.size()>=2 && path.front()==L'"' && path.back()==L'"')path=path.substr(1,path.size()-2);
        // App Paths is a path, not a shell command. No target existence probe.
        if(path.size()<7 || !iswalpha(path[0]) || path[1]!=L':' || path[2]!=L'\\' || !EndsExe(path) || path.find(L'"')!=std::wstring::npos)return {};
        return path;
    }
    void AppPaths(HKEY root,const std::wstring& subkey,REGSAM view=0) {
        if(!Continue())return;
        RegKey key;
        LONG rc=RegOpenKeyExW(root,subkey.c_str(),0,KEY_READ|view,&key.value);
        if(rc!=ERROR_SUCCESS){if(rc!=ERROR_FILE_NOT_FOUND)Error(subkey,rc);return;}
        for(DWORD i=0;Continue();++i){
            wchar_t name[256]{};DWORD count=256;
            rc=RegEnumKeyExW(key.value,i,name,&count,nullptr,nullptr,nullptr,nullptr);
            if(rc==ERROR_NO_MORE_ITEMS)break;
            ++result.stats.inspected;
            if(rc!=ERROR_SUCCESS){Error(subkey,rc);continue;}
            ++result.stats.registrationKeys;
            if(!EndsExe(name))continue;
            RegKey app;
            rc=RegOpenKeyExW(key.value,name,0,KEY_QUERY_VALUE,&app.value);
            if(rc!=ERROR_SUCCESS){Error(name,rc);continue;}
            DWORD type=0,bytes=0;
            rc=RegQueryValueExW(app.value,nullptr,nullptr,&type,nullptr,&bytes);
            if(rc!=ERROR_SUCCESS || bytes<sizeof(wchar_t) || bytes>65536 || bytes%sizeof(wchar_t))continue;
            std::vector<wchar_t> path(bytes/sizeof(wchar_t)+1,0);
            rc=RegQueryValueExW(app.value,nullptr,nullptr,&type,(BYTE*)path.data(),&bytes);
            if(rc!=ERROR_SUCCESS)continue;
            auto target=RegistrationPath(path.data(),type);
            if(!target.empty())Add({std::wstring(name,count-4),target,target,false});
        }
    }
    void AppsFolder(const std::function<void(const char*,size_t)>& observer={}) {
        if(!Continue())return;
        Com<IShellItem> folder;
        HRESULT hr=SHCreateItemFromParsingName(L"shell:AppsFolder",nullptr,IID_PPV_ARGS(&folder.value));
        if(FAILED(hr)){Error(L"AppsFolder",hr);return;}
        if(observer)observer("folder",0);
        Com<IEnumShellItems> items;
        hr=folder.value->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(&items.value));
        if(FAILED(hr)){Error(L"AppsFolder enumeration",hr);return;}
        if(observer)observer("bind",0);
        while(Continue()){
            {
                Com<IShellItem> item;
                hr=items.value->Next(1,&item.value,nullptr);
                if(hr==S_FALSE)break;
                if(FAILED(hr)){Error(L"AppsFolder next",hr);break;}
                ++result.stats.inspected; ++result.stats.shellItems;
                if(observer && result.stats.shellItems==1)observer("next",1);
                PWSTR name=nullptr,path=nullptr;
                HRESULT nr=item.value->GetDisplayName(SIGDN_NORMALDISPLAY,&name);
                if(observer && result.stats.shellItems==1)observer("display_name",1);
                HRESULT pr=item.value->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&path);
                if(observer && result.stats.shellItems==1)observer("parsing_name",1);
                if(SUCCEEDED(nr) && SUCCEEDED(pr) && name && path)Add({name,path,{},false});
                CoTaskMemFree(name);CoTaskMemFree(path);
            }
            if(observer && (result.stats.shellItems<=40 || result.stats.shellItems%32==0))observer("released",result.stats.shellItems);
        }
    }
    Result Finish(){result.stats.elapsedMilliseconds=GetTickCount64()-started;return std::move(result);}
};
inline void CollectStartMenu(Builder& builder) {
    for(const KNOWNFOLDERID* id:{&FOLDERID_Programs,&FOLDERID_CommonPrograms}){
        if(!builder.Continue())break;
        PWSTR root=nullptr;
        HRESULT hr=SHGetKnownFolderPath(*id,KF_FLAG_DONT_VERIFY,nullptr,&root);
        if(SUCCEEDED(hr) && root)builder.StartMenu(root);
        else builder.Error(L"Start Menu known folder",hr);
        CoTaskMemFree(root);
    }
}
inline void CollectAppPaths(Builder& builder) {
    const std::wstring registrations=L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    builder.AppPaths(HKEY_CURRENT_USER,registrations,KEY_WOW64_64KEY);
    builder.AppPaths(HKEY_CURRENT_USER,registrations,KEY_WOW64_32KEY);
    builder.AppPaths(HKEY_LOCAL_MACHINE,registrations,KEY_WOW64_64KEY);
    builder.AppPaths(HKEY_LOCAL_MACHINE,registrations,KEY_WOW64_32KEY);
}
inline void CollectAppsFolder(Builder& builder,const std::function<void(const char*,size_t)>& observer={}) {
    builder.AppsFolder(observer);
}
inline Result Collect(Limits limits={},const std::atomic<bool>* cancelled=nullptr) {
    Builder builder(limits,cancelled);
    CollectStartMenu(builder);
    CollectAppPaths(builder);
    CollectAppsFolder(builder);
    return builder.Finish();
}
} // namespace ft::apps
