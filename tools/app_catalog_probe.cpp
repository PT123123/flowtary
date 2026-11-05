// Use the real production matching code with synthetic launch metadata only.
// wWinMain is compiled but never called: no real app discovery, hooks, UI or service.
#include "../src/main.cpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <winternl.h>

// Test-only per-process handle-type tracing. This deliberately snapshots only
// the probe's own handle table; it never requests the system-wide handle table.
using NtQueryInformationProcessFn=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
using NtQueryObjectFn=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
struct ProcessHandleEntryInfo { HANDLE handle; ULONG_PTR handleCount,pointerCount; ULONG grantedAccess,objectTypeIndex,handleAttributes,reserved; };
struct ProcessHandleSnapshotInfo { ULONG_PTR numberOfHandles,reserved; ProcessHandleEntryInfo handles[1]; };
struct HandleTypeSnapshot { std::map<std::string,long long> counts; ULONG informationClass=51,requiredBytes=0; NTSTATUS probeStatus=0,snapshotStatus=0; ULONG_PTR handles=0; size_t typeQueries=0,typeSuccesses=0; };
struct NamedHandleTypeSnapshot { std::string stage; HandleTypeSnapshot snapshot; };

static HandleTypeSnapshot HandleTypeCounts(){
    auto ntdll=GetModuleHandleW(L"ntdll.dll");
    auto queryProcess=(NtQueryInformationProcessFn)GetProcAddress(ntdll,"NtQueryInformationProcess");
    auto queryObject=(NtQueryObjectFn)GetProcAddress(ntdll,"NtQueryObject");
    HandleTypeSnapshot result;
    if(!queryProcess || !queryObject){result.probeStatus=(NTSTATUS)0xC0000002;return result;}
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,GetCurrentProcessId());
    if(!process){result.probeStatus=(NTSTATUS)(0xC0000000u|GetLastError());return result;}
    ULONG bytes=1024*1024,returned=0;std::vector<unsigned char> snapshot(bytes);
    NTSTATUS status=queryProcess(process,result.informationClass,snapshot.data(),bytes,&returned);
    result.probeStatus=status;result.snapshotStatus=status;result.requiredBytes=returned;
    CloseHandle(process);
    if(status<0)return result;
    auto info=(const ProcessHandleSnapshotInfo*)snapshot.data();
    result.handles=info->numberOfHandles;
    if(info->numberOfHandles>(snapshot.size()-offsetof(ProcessHandleSnapshotInfo,handles))/sizeof(ProcessHandleEntryInfo))return result;
    for(ULONG_PTR i=0;i<info->numberOfHandles;++i){
        ULONG needed=0;HANDLE handle=info->handles[i].handle;
        status=queryObject(handle,2,nullptr,0,&needed);
        if((ULONG)status!=0xC0000004 && (ULONG)status!=0xC0000023)continue;
        if(needed<sizeof(UNICODE_STRING) || needed>64*1024)continue;
        std::vector<unsigned char> typeInfo(needed);
        status=queryObject(handle,2,typeInfo.data(),needed,&needed);
        if(status<0)continue;
        ++result.typeSuccesses;
        auto typeName=(const UNICODE_STRING*)typeInfo.data();
        if(!typeName->Buffer || !typeName->Length || typeName->Length%sizeof(wchar_t))continue;
        std::string type;
        type.reserve(typeName->Length/sizeof(wchar_t));
        for(USHORT j=0;j<typeName->Length/sizeof(wchar_t);++j){
            wchar_t c=typeName->Buffer[j];type.push_back(c>=0x20 && c<0x7f?(char)c:'?');
        }
        ++result.counts[type];
        ++result.typeQueries;
    }
    return result;
}
static void WriteHandleTypes(std::ostream& out,const std::map<std::string,long long>& counts){
    out<<'{';bool first=true;
    for(const auto& item:counts){if(!first)out<<',';first=false;out<<'"'<<item.first<<"\":"<<item.second;}
    out<<'}';
}
static std::map<std::string,long long> HandleTypeDelta(const HandleTypeSnapshot& before,const HandleTypeSnapshot& after){
    auto delta=after.counts;
    for(const auto& item:before.counts)delta[item.first]-=item.second;
    for(auto it=delta.begin();it!=delta.end();)if(!it->second)it=delta.erase(it);else ++it;
    return delta;
}
static void WriteHandleTypeSnapshot(std::ostream& out,const HandleTypeSnapshot& snapshot){
    out<<"{\"information_class\":"<<snapshot.informationClass<<",\"probe_status\":"<<(ULONG)snapshot.probeStatus
       <<",\"required_bytes\":"<<snapshot.requiredBytes<<",\"snapshot_status\":"<<(ULONG)snapshot.snapshotStatus
       <<",\"handles\":"<<snapshot.handles<<",\"object_types_read\":"<<snapshot.typeQueries
       <<",\"object_type_queries_succeeded\":"<<snapshot.typeSuccesses<<",\"counts\":";
    WriteHandleTypes(out,snapshot.counts);out<<'}';
}

static ULONGLONG UnixMs(){FILETIME time;GetSystemTimeAsFileTime(&time);ULARGE_INTEGER ticks;ticks.LowPart=time.dwLowDateTime;ticks.HighPart=time.dwHighDateTime;return(ticks.QuadPart-116444736000000000ULL)/10000;}
static std::wstring Arg(int argc,wchar_t** argv,const wchar_t* flag,const wchar_t* fallback=L""){
    for(int i=1;i+1<argc;++i)if(wcscmp(argv[i],flag)==0)return argv[i+1];return fallback;
}
int wmain(int argc,wchar_t** argv){
    std::wstring output=Arg(argc,argv,L"--output"),begin=Arg(argc,argv,L"--start-event"),end=Arg(argc,argv,L"--stop-event");
    unsigned count=(unsigned)_wtoi(Arg(argc,argv,L"--entries",L"1000").c_str());
    bool discover=false;
    for(int i=1;i<argc;++i)if(wcscmp(argv[i],L"--discover-real-catalog")==0)discover=true;
    if(output.empty() || begin.empty() || end.empty() || count<100 || count>4096){fputs("explicit output/start/stop events and 100..4096 entries required\n",stderr);return 1;}
    HANDLE started=OpenEventW(SYNCHRONIZE,FALSE,begin.c_str()),stopped=OpenEventW(SYNCHRONIZE,FALSE,end.c_str());
    if(!started || !stopped){if(started)CloseHandle(started);if(stopped)CloseHandle(stopped);return 2;}
    if(WaitForSingleObject(started,20000)!=WAIT_OBJECT_0){CloseHandle(started);CloseHandle(stopped);return 3;}
    auto buildBegin=UnixMs();LARGE_INTEGER frequency,t0,t1;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&t0);
    ft::apps::Result catalog;
    std::vector<DWORD> stageHandles;
    struct ShellHandleSample { std::string stage; size_t item; DWORD handles; };
    std::vector<ShellHandleSample> shellHandleSamples;
    std::vector<NamedHandleTypeSnapshot> handleTypeStages;
    auto handleTypesBefore=discover?HandleTypeCounts():HandleTypeSnapshot{};
    auto recordHandleTypes=[&](const char* stage){handleTypeStages.push_back({stage,HandleTypeCounts()});};
    auto sampleHandles=[&](){DWORD n=0;if(GetProcessHandleCount(GetCurrentProcess(),&n))stageHandles.push_back(n);};
    HRESULT comResult=E_UNEXPECTED;
    if(discover){
        sampleHandles();
        std::thread discoveryThread([&](){
            comResult=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            sampleHandles();
            // Sample the production sources independently, then repeat the full
            // bounded refresh twice to distinguish source cost from per-refresh growth.
            ft::apps::Builder builder;
            ft::apps::CollectStartMenu(builder);sampleHandles();recordHandleTypes("after_start_menu");
            ft::apps::CollectAppPaths(builder);sampleHandles();recordHandleTypes("after_app_paths");
            ft::apps::CollectAppsFolder(builder,[&](const char* stage,size_t item){
                DWORD n=0;if(GetProcessHandleCount(GetCurrentProcess(),&n))shellHandleSamples.push_back({stage,item,n});
                if(stage==std::string("next") && item==1)recordHandleTypes("after_first_apps_folder_next");
            });
            sampleHandles();recordHandleTypes("after_first_apps_folder_walk");
            catalog=builder.Finish();sampleHandles();
            for(unsigned round=0;round<2;++round){catalog=ft::apps::Collect();sampleHandles();recordHandleTypes(round==0?"after_refresh_1":"after_refresh_2");}
            if(SUCCEEDED(comResult)){CoUninitialize();sampleHandles();recordHandleTypes("after_com_uninitialize");}
        });
        discoveryThread.join(); // Match the production worker's COM/thread lifetime.
        sampleHandles();
        recordHandleTypes("after_worker_exit");
    }else{
        ft::apps::Builder builder;
        for(unsigned i=0;i<count;++i){
            wchar_t name[64];swprintf_s(name,L"FixtureApp-%04u",i);
            std::wstring target=L"C:\\Flowtary-Unopened-Fixture\\"+std::wstring(name)+L".exe";
            builder.Add({name,target,target,false});
        }
        catalog=builder.Finish();
    }
    auto handleTypesAfter=discover?HandleTypeCounts():HandleTypeSnapshot{};
    auto discoveryStats=catalog.stats;
    ProgramBuildReply reply;CopyAppCatalog(std::move(catalog),reply);g.programs=std::move(reply.programs);
    QueryPerformanceCounter(&t1);double buildMs=1000.0*(t1.QuadPart-t0.QuadPart)/frequency.QuadPart;
    if(!discover && g.programs.size()!=count){CloseHandle(started);CloseHandle(stopped);return 4;}
    g.weightEnabled=g.historyEnabled=g.learnEnabled=false;g.startingUp=false;
    auto queryBegin=UnixMs();std::vector<double> times;auto deadline=GetTickCount64()+5000;
    while(!g.programs.empty() && GetTickCount64()<deadline && times.size()<100000){
        wchar_t word[64]{};
        if(discover)wcscpy_s(word,g.programs[times.size()%g.programs.size()].name.c_str());
        else swprintf_s(word,L"FixtureApp-%04u",(unsigned)((times.size()*41)%count));
        g.items.clear();QueryPerformanceCounter(&t0);SearchPrograms(word);QueryPerformanceCounter(&t1);
        if(g.items.empty())break;
        times.push_back(1000000.0*(t1.QuadPart-t0.QuadPart)/frequency.QuadPart);
    }
    auto queryEnd=UnixMs();std::sort(times.begin(),times.end());
    std::ofstream report{std::filesystem::path(output)};
    report<<std::setprecision(12)<<"{\n\"schema\":1,\"mode\":\""<<(discover?"production-real-app-catalog-discovery":"production-app-query-synthetic-no-enumeration")<<"\",\"pid\":"<<GetCurrentProcessId()
        <<",\"entries\":"<<g.programs.size()<<",\"metadata_build_ms\":"<<buildMs<<",\"queries\":"<<times.size()
        <<",\"query_metrics_available\":"<<(times.empty()?"false":"true")
        <<",\"query_p50_us\":";
    if(times.empty())report<<"null";else report<<times[(times.size()-1)/2];
    report<<",\"query_p95_us\":";
    if(times.empty())report<<"null";else report<<times[(times.size()-1)*95/100];
    report<<",\"query_max_us\":";
    if(times.empty())report<<"null";else report<<times.back();
    report<<",\"build_begin_unix_ms\":"<<buildBegin<<",\"query_begin_unix_ms\":"<<queryBegin<<",\"query_end_unix_ms\":"<<queryEnd
        <<",\"discovery_inspected\":"<<discoveryStats.inspected<<",\"discovery_directories\":"<<discoveryStats.directories
        <<",\"discovery_registration_keys\":"<<discoveryStats.registrationKeys<<",\"discovery_shell_items\":"<<discoveryStats.shellItems
        <<",\"discovery_elapsed_ms\":"<<discoveryStats.elapsedMilliseconds<<",\"discovery_estimated_bytes\":"<<discoveryStats.estimatedBytes
        <<",\"discovery_truncated\":"<<(discoveryStats.truncated?"true":"false")<<",\"com_init_result\":"<<comResult
        <<",\"discovery_handle_samples\":[";
    for(size_t i=0;i<stageHandles.size();++i){if(i)report<<',';report<<stageHandles[i];}
    report<<']'
        <<",\"apps_folder_handle_samples\":[";
    for(size_t i=0;i<shellHandleSamples.size();++i){if(i)report<<',';
        report<<"[\""<<shellHandleSamples[i].stage<<"\","<<shellHandleSamples[i].item<<','<<shellHandleSamples[i].handles<<']';}
    report<<']'
        <<",\"handle_type_snapshot_before\":";
    WriteHandleTypeSnapshot(report,handleTypesBefore);
    report<<",\"handle_type_snapshot_after\":";
    WriteHandleTypeSnapshot(report,handleTypesAfter);
    report<<",\"handle_type_delta\":";
    WriteHandleTypes(report,HandleTypeDelta(handleTypesBefore,handleTypesAfter));
    report<<",\"handle_type_stages\":[";
    for(size_t i=0;i<handleTypeStages.size();++i){if(i)report<<',';
        report<<"{\"stage\":\""<<handleTypeStages[i].stage<<"\",\"snapshot\":";
        WriteHandleTypeSnapshot(report,handleTypeStages[i].snapshot);report<<'}';}
    report<<']';
    report
        <<",\"raw_volume_opens\":0,\"icons_requested\":0,\"ui_paint_measured\":false}\n";
    report.close();
    bool written=!report.fail();
    bool done=WaitForSingleObject(stopped,20000)==WAIT_OBJECT_0;
    CloseHandle(started);CloseHandle(stopped);
    return written && done ? 0:6;
}
