#pragma once
// Fixed, narrow notification sources. Poll() never enumerates or rebuilds data.
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace ft::apps {
class CatalogWatch {
    struct Watch { HANDLE event=nullptr; HKEY key=nullptr; };
    std::vector<Watch> watches;
    ULONG shellRegistration=0;
    bool dirty=false;
    ULONGLONG firstSignal=0,lastSignal=0,lastRefresh=0;
    void Changed(ULONGLONG now){if(!dirty)firstSignal=now;dirty=true;lastSignal=now;}
public:
    struct Failure { std::wstring source; LONG code; };
    std::vector<Failure> errors;
    CatalogWatch()=default;
    CatalogWatch(const CatalogWatch&)=delete;
    CatalogWatch& operator=(const CatalogWatch&)=delete;
    ~CatalogWatch(){Stop();}
    void Directory(const std::wstring& path) {
        if(watches.size()>=8)return;
        DWORD attr=GetFileAttributesW(path.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES || !(attr&FILE_ATTRIBUTE_DIRECTORY) || (attr&FILE_ATTRIBUTE_REPARSE_POINT))return;
        HANDLE event=FindFirstChangeNotificationW(path.c_str(),TRUE,FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE);
        if(event==INVALID_HANDLE_VALUE){errors.push_back({path,(LONG)GetLastError()});return;}
        watches.push_back({event,nullptr});
    }
    void Registration(HKEY root,const std::wstring& path,REGSAM view=0){
        if(watches.size()>=8)return;
        HKEY key=nullptr;
        LONG code=RegOpenKeyExW(root,path.c_str(),0,KEY_NOTIFY|view,&key);
        if(code!=ERROR_SUCCESS){if(code!=ERROR_FILE_NOT_FOUND)errors.push_back({path,code});return;}
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!event){errors.push_back({path,(LONG)GetLastError()});RegCloseKey(key);return;}
        code=RegNotifyChangeKeyValue(key,TRUE,REG_NOTIFY_CHANGE_NAME|REG_NOTIFY_CHANGE_LAST_SET,event,TRUE);
        if(code!=ERROR_SUCCESS){errors.push_back({path,code});CloseHandle(event);RegCloseKey(key);return;}
        watches.push_back({event,key});
    }
    void Start(HWND window=nullptr,UINT message=0){
        Stop();errors.clear();
        for(const KNOWNFOLDERID* id:{&FOLDERID_Programs,&FOLDERID_CommonPrograms}){
            PWSTR path=nullptr;
            if(SUCCEEDED(SHGetKnownFolderPath(*id,KF_FLAG_DONT_VERIFY,nullptr,&path)) && path)Directory(path);
            CoTaskMemFree(path);
        }
        const std::wstring appPaths=L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths";
        for(HKEY root:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}){
            Registration(root,appPaths,KEY_WOW64_64KEY);Registration(root,appPaths,KEY_WOW64_32KEY);
        }
        if(window && message){
            PIDLIST_ABSOLUTE folder=nullptr;
            if(SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder,KF_FLAG_DONT_VERIFY,nullptr,&folder))){
                SHChangeNotifyEntry entry{folder,FALSE};
                shellRegistration=SHChangeNotifyRegister(window,SHCNRF_ShellLevel|SHCNRF_NewDelivery,
                    SHCNE_CREATE|SHCNE_DELETE|SHCNE_RENAMEITEM|SHCNE_UPDATEITEM|SHCNE_UPDATEDIR,message,1,&entry);
                if(!shellRegistration)errors.push_back({L"AppsFolder notifications",(LONG)GetLastError()});
            }
            CoTaskMemFree(folder);
        }
        lastRefresh=GetTickCount64();
    }
    bool Poll(ULONGLONG now=GetTickCount64()){
        for(auto it=watches.begin();it!=watches.end();){
            DWORD wait=WaitForSingleObject(it->event,0);
            if(wait==WAIT_TIMEOUT){++it;continue;}
            bool valid=wait==WAIT_OBJECT_0;
            if(valid){
                Changed(now);
                if(it->key){ResetEvent(it->event);valid=RegNotifyChangeKeyValue(it->key,TRUE,REG_NOTIFY_CHANGE_NAME|REG_NOTIFY_CHANGE_LAST_SET,it->event,TRUE)==ERROR_SUCCESS;}
                else valid=FindNextChangeNotification(it->event)!=FALSE;
            }
            if(!valid){
                if(it->key){RegCloseKey(it->key);CloseHandle(it->event);}else FindCloseChangeNotification(it->event);
                it=watches.erase(it); // Retire a failed source, never retry/spin automatically.
            }else ++it;
        }
        return dirty && now-lastRefresh>=5000 && (now-lastSignal>=500 || now-firstSignal>=5000);
    }
    void AcknowledgeRefresh(ULONGLONG now=GetTickCount64()){dirty=false;lastRefresh=now;}
    bool ShellNotification(HANDLE change,DWORD processId){
        PIDLIST_ABSOLUTE* items=nullptr;LONG event=0;
        HANDLE locked=SHChangeNotification_Lock(change,processId,&items,&event);
        if(!locked)return false;
        Changed(GetTickCount64());SHChangeNotification_Unlock(locked);return true;
    }
    size_t SourceCount()const{return watches.size()+(shellRegistration?1:0);}
    void Stop(){
        if(shellRegistration){SHChangeNotifyDeregister(shellRegistration);shellRegistration=0;}
        for(auto& w:watches){if(w.key){RegCloseKey(w.key);CloseHandle(w.event);}else FindCloseChangeNotification(w.event);}
        watches.clear();dirty=false;
    }
};
} // namespace ft::apps
