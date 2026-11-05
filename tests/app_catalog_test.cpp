#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app_catalog.h"
#include "app_icons.h"
#include "app_catalog_watch.h"
#include <atomic>
#include <iostream>
#include <thread>

static void Check(bool ok,const char* message){if(!ok){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
static void Touch(const std::wstring& path){HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(file!=INVALID_HANDLE_VALUE,"create fixture shortcut");CloseHandle(file);}
template<class F>static void Until(F test,const char* message="asynchronous fixture completes"){auto end=GetTickCount64()+3000;while(!test() && GetTickCount64()<end)Sleep(1);Check(test(),message);}
int main(){
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    std::wstring root=std::wstring(temp)+L"Flowtary-AppCatalog-Test-"+std::to_wstring(GetCurrentProcessId());
    Check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"create explicit fixture root");
    Check(CreateDirectoryW((root+L"\\nested").c_str(),nullptr)!=FALSE,"create fixture subdirectory");
    // Invalid, empty .lnk files must still be indexed: discovery never opens/resolves them.
    Touch(root+L"\\Alpha.lnk");Touch(root+L"\\ignored.exe");Touch(root+L"\\nested\\Beta.LNK");
    ft::apps::Builder builder;builder.StartMenu(root);
    builder.Add({L"ALPHA",L"C:\\unopened\\alpha.exe",{},false});
    auto result=builder.Finish();
    Check(result.entries.size()==2 && result.stats.duplicates==1,"known-root names and source priority");
    Check(result.stats.directories==2 && result.entries[0].target.empty(),"bounded folder enumeration does not resolve shortcut targets");
    ft::apps::Limits limited;limited.maxDirectories=1;
    ft::apps::Builder shallow(limited);shallow.StartMenu(root);auto shortResult=shallow.Finish();
    Check(shortResult.stats.truncated && shortResult.stats.directories==1,"directory cap prevents entering descendants");
    std::atomic<bool> stop{true};ft::apps::Builder cancelled({},&stop);cancelled.StartMenu(root);
    auto stopped=cancelled.Finish();Check(stopped.stats.cancelled && stopped.stats.directories==0,"cancellation precedes directory access");
    limited={};limited.maxEntries=1;ft::apps::Builder capped(limited);capped.StartMenu(root);
    auto cap=capped.Finish();Check(cap.entries.size()==1 && cap.stats.truncated,"result cap is explicit");
    limited={};limited.maxEstimatedBytes=128;
    ft::apps::Builder memoryLimited(limited);memoryLimited.Add({L"Oversized",std::wstring(200,L'x'),{},false});
    auto memoryCap=memoryLimited.Finish();Check(memoryCap.entries.empty() && memoryCap.stats.truncated,"catalog allocation estimate is bounded");
    using B=ft::apps::Builder;
    Check(B::RegistrationPath(L"\"C:\\Apps\\Alpha.exe\"",REG_SZ)==L"C:\\Apps\\Alpha.exe","quoted registered path");
    Check(B::RegistrationPath(L"\\\\server\\share\\Alpha.exe",REG_SZ).empty(),"registered network targets are not probed");
    Check(B::RegistrationPath(L"C:\\Apps\\Alpha.exe --flag",REG_SZ).empty(),"registration value is not a shell command");
    Check(B::RegistrationPath(L"relative.exe",REG_SZ).empty(),"registration must be absolute");
    ft::apps::CatalogWatch watcher;watcher.Directory(root);
    Check(watcher.SourceCount()==1,"one directory notification for the explicit fixture subtree");
    Touch(root+L"\\changed.lnk");
    auto now=GetTickCount64();
    Until([&]{now+=1000;return watcher.Poll(now);},"directory change is observed and coalesced");
    watcher.AcknowledgeRefresh(now);
    Check(!watcher.Poll(now+100),"directory bursts coalesce instead of repeated rebuilds");
    watcher.Stop();Check(watcher.SourceCount()==0,"directory notification is closed");
    // A private volatile registration fixture never touches actual installed-app keys.
    std::wstring reg=L"Software\\Flowtary-AppCatalog-Test-"+std::to_wstring(GetCurrentProcessId());
    HKEY key=nullptr,entry=nullptr;DWORD disposition=0;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER,reg.c_str(),0,nullptr,REG_OPTION_VOLATILE,KEY_ALL_ACCESS,nullptr,&key,&disposition)==ERROR_SUCCESS,"create registration fixture");
    Check(RegCreateKeyExW(key,L"Gamma.exe",0,nullptr,REG_OPTION_VOLATILE,KEY_ALL_ACCESS,nullptr,&entry,&disposition)==ERROR_SUCCESS,"create app fixture");
    const wchar_t path[]=L"C:\\Missing-Fixture-Target\\Gamma.exe";
    Check(RegSetValueExW(entry,nullptr,0,REG_SZ,(const BYTE*)path,sizeof(path))==ERROR_SUCCESS,"set nonexistent target fixture");RegCloseKey(entry);
    ft::apps::Builder registered;registered.AppPaths(HKEY_CURRENT_USER,reg);auto registrations=registered.Finish();
    Check(registrations.entries.size()==1 && registrations.entries[0].target==path,"registration discovery does not require file existence");
    DWORD handlesBefore=0,handlesAfter=0;GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);
    for(int round=0;round<20;++round){
        ft::apps::CatalogWatch registrationWatch;registrationWatch.Registration(HKEY_CURRENT_USER,reg);
        Check(registrationWatch.SourceCount()==1,"private registration notification is armed");
        std::wstring updated=L"C:\\Missing-Fixture-Target\\Other-"+std::to_wstring(round)+L".exe";
        HKEY changed=nullptr;RegOpenKeyExW(key,L"Gamma.exe",0,KEY_SET_VALUE,&changed);
        Check(RegSetValueExW(changed,nullptr,0,REG_SZ,(const BYTE*)updated.c_str(),(DWORD)((updated.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"change registration fixture");RegCloseKey(changed);
        auto tick=GetTickCount64();
        Until([&]{tick+=1000;return registrationWatch.Poll(tick);},"registration change is observed and coalesced");
        registrationWatch.Stop();
    }
    GetProcessHandleCount(GetCurrentProcess(),&handlesAfter);
    Check(handlesAfter<=handlesBefore+2,"repeated registration watcher lifecycle does not retain process handles");
    RegCloseKey(key);RegDeleteTreeW(HKEY_CURRENT_USER,reg.c_str());
    // Hold the sole extractor to prove bounded pending work, generation rejection,
    // and safe stop while an extraction is in flight. No real Shell icon I/O.
    std::atomic<bool> entered{false},release{false},finished{false};
    ft::apps::AsyncIcons icons([&](const std::wstring&)->HICON{entered=true;while(!release)Sleep(1);finished=true;return CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));});
    icons.Reset(1);Check(icons.Request(L"old"),"queue first icon");Until([&]{return entered.load();});
    for(size_t i=1;i<ft::apps::AsyncIcons::kPendingLimit;++i)Check(icons.Request(std::to_wstring(i)),"queue within icon limit");
    Check(!icons.Request(L"overflow"),"icon queue has a hard pending cap");
    icons.Reset(2);release=true;Until([&]{return finished.load();});Sleep(10);
    Check(icons.Drain().empty(),"stale generation icon is discarded");
    Check(icons.Request(L"new"),"new generation can request icons");
    std::vector<ft::apps::IconResult> fresh;
    Until([&]{if(fresh.empty())fresh=icons.Drain();return !fresh.empty();});
    Check(fresh[0].path==L"new" && fresh[0].generation==2 && !!fresh[0].icon,"owned result belongs to current generation");
    icons.Stop();Check(!icons.Request(L"after-stop"),"stopped worker rejects new work");
    Check(DeleteFileW((root+L"\\Alpha.lnk").c_str())!=FALSE,"remove fixture shortcut");
    DeleteFileW((root+L"\\ignored.exe").c_str());DeleteFileW((root+L"\\nested\\Beta.LNK").c_str());
    DeleteFileW((root+L"\\changed.lnk").c_str());
    RemoveDirectoryW((root+L"\\nested").c_str());RemoveDirectoryW(root.c_str());
    std::cout<<"PASS: bounded launch catalog, registration policy, no shortcut resolution, bounded asynchronous icons\n";
}
