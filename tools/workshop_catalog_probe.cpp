// Bounded production-path probe for Flowtary's C:\workshop\*.exe rule.
#define NOMINMAX
#include "../src/workshop_catalog.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cwchar>
#include <vector>

static std::wstring Arg(int argc,wchar_t** argv,const wchar_t* flag){
    for(int i=1;i+1<argc;++i)if(wcscmp(argv[i],flag)==0)return argv[i+1];
    return {};
}
static double Milliseconds(LARGE_INTEGER begin,LARGE_INTEGER end,LARGE_INTEGER frequency){
    return 1000.0*(end.QuadPart-begin.QuadPart)/frequency.QuadPart;
}
static void WriteTimes(std::ostream& out,const std::vector<double>& times){
    auto sorted=times;std::sort(sorted.begin(),sorted.end());
    auto percentile=[&](double p){return sorted.empty()?0.0:sorted[std::min(sorted.size()-1,(size_t)((sorted.size()-1)*p))];};
    out<<"{\"queries\":"<<times.size()<<",\"p50_us\":"<<percentile(.50)*1000.0
       <<",\"p95_us\":"<<percentile(.95)*1000.0<<",\"max_us\":"<<(sorted.empty()?0.0:sorted.back()*1000.0)<<'}';
}
static bool IsWorkshopExeHit(const df::Hit& hit){
    constexpr wchar_t prefix[]=L"C:\\workshop\\";
    return !hit.isFolder && hit.workshop && hit.path.size()>std::size(prefix)-1 &&
        CompareStringOrdinal(hit.path.data(),(int)(std::size(prefix)-1),prefix,(int)(std::size(prefix)-1),TRUE)==CSTR_EQUAL &&
        hit.path.size()>=4 && _wcsicmp(hit.path.c_str()+hit.path.size()-4,L".exe")==0;
}
int wmain(int argc,wchar_t** argv){
    const std::wstring output=Arg(argc,argv,L"--output");
    const std::wstring startName=Arg(argc,argv,L"--start-event");
    const std::wstring stopName=Arg(argc,argv,L"--stop-event");
    if(output.empty()||startName.empty()||stopName.empty())return 1;
    HANDLE start=OpenEventW(SYNCHRONIZE,FALSE,startName.c_str());
    HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,stopName.c_str());
    if(!start||!stop){if(start)CloseHandle(start);if(stop)CloseHandle(stop);return 2;}
    if(WaitForSingleObject(start,20000)!=WAIT_OBJECT_0){CloseHandle(start);CloseHandle(stop);return 3;}

    LARGE_INTEGER frequency,t0,t1;QueryPerformanceFrequency(&frequency);
    DWORD handlesBefore=0,handlesAfterWarmup=0,handlesAfterFirstSearch=0,handlesPeak=0;
    GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);handlesPeak=handlesBefore;
    QueryPerformanceCounter(&t0);const df::WorkshopWarmupSummary warmup=df::WorkshopCatalogWarmup();QueryPerformanceCounter(&t1);
    const double warmupCallMs=Milliseconds(t0,t1,frequency);
    GetProcessHandleCount(GetCurrentProcess(),&handlesAfterWarmup);handlesPeak=std::max(handlesPeak,handlesAfterWarmup);
    df::Query query;query.isFile=true;query.words={L"exe"};
    std::vector<df::Hit> hits;
    QueryPerformanceCounter(&t0);int coldResult=df::WorkshopCatalogQuery(query,hits);QueryPerformanceCounter(&t1);
    const double firstSearchMs=Milliseconds(t0,t1,frequency);
    GetProcessHandleCount(GetCurrentProcess(),&handlesAfterFirstSearch);handlesPeak=std::max(handlesPeak,handlesAfterFirstSearch);
    const size_t firstHits=hits.size();

    df::Query wildcardQuery;wildcardQuery.isFile=true;wildcardQuery.words={L"C:\\workshop\\*.exe"};
    QueryPerformanceCounter(&t0);int wildcardResult=df::WorkshopCatalogQuery(wildcardQuery,hits);QueryPerformanceCounter(&t1);
    const double wildcardFirstMs=Milliseconds(t0,t1,frequency);
    const size_t wildcardHits=hits.size();
    bool wildcardExact=wildcardResult>=0 && std::all_of(hits.begin(),hits.end(),IsWorkshopExeHit);
    std::vector<double> wildcardTimes;wildcardTimes.reserve(10000);
    for(size_t i=0;i<10000;++i){
        QueryPerformanceCounter(&t0);const int result=df::WorkshopCatalogQuery(wildcardQuery,hits);QueryPerformanceCounter(&t1);
        if(result!=wildcardResult || !std::all_of(hits.begin(),hits.end(),IsWorkshopExeHit))wildcardExact=false;
        wildcardTimes.push_back(Milliseconds(t0,t1,frequency));
    }

    std::vector<double> warmTimes;size_t warmResult=-1;const ULONGLONG deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline && warmTimes.size()<100000){
        QueryPerformanceCounter(&t0);warmResult=(size_t)df::WorkshopCatalogQuery(query,hits);QueryPerformanceCounter(&t1);
        warmTimes.push_back(Milliseconds(t0,t1,frequency));
        if((warmTimes.size()&255)==0){DWORD now=0;if(GetProcessHandleCount(GetCurrentProcess(),&now))handlesPeak=std::max(handlesPeak,now);}
    }
    DWORD handlesEnd=0;GetProcessHandleCount(GetCurrentProcess(),&handlesEnd);handlesPeak=std::max(handlesPeak,handlesEnd);

    std::ofstream report{std::filesystem::path(output)};
    report<<std::setprecision(12)<<"{\"schema\":1,\"mode\":\"production-workshop-catalog-real-root\",\"pid\":"<<GetCurrentProcessId()
          <<",\"root\":\"C:\\\\workshop\",\"rule\":\"descendants with .exe extension only\""
          <<",\"startup_warmup\":{\"complete\":"<<(warmup.complete?"true":"false")<<",\"directories\":"<<warmup.directories
          <<",\"exe_paths\":"<<warmup.entries<<",\"elapsed_ms\":"<<warmup.elapsedMilliseconds
          <<",\"measured_call_ms\":"<<warmupCallMs<<",\"error\":"<<warmup.error<<'}'
          <<",\"first_search_after_warmup\":{\"result\":"<<coldResult<<",\"hits\":"<<firstHits<<",\"elapsed_ms\":"<<firstSearchMs<<'}'
          <<",\"explicit_workshop_wildcard\":{\"pattern\":\"C:\\\\workshop\\\\*.exe\",\"result\":"<<wildcardResult
          <<",\"hits\":"<<wildcardHits<<",\"elapsed_ms\":"<<wildcardFirstMs<<",\"all_hits_within_rule\":"<<(wildcardExact?"true":"false")
          <<",\"warm_latency\":";
    WriteTimes(report,wildcardTimes);
    report<<"},\"warm_query\":";
    WriteTimes(report,warmTimes);
    report<<",\"warm_query_result\":"<<warmResult<<",\"warm_hit_count\":"<<hits.size()
          <<",\"process_handles\":{\"before\":"<<handlesBefore<<",\"after_warmup\":"<<handlesAfterWarmup
          <<",\"after_first_search\":"<<handlesAfterFirstSearch<<",\"peak\":"<<handlesPeak<<",\"end\":"<<handlesEnd<<'}'
          <<",\"raw_volume_opens\":0,\"file_contents_read\":0,\"reparse_points_followed\":0}\n";
    report.close();bool written=!report.fail();
    const bool released=WaitForSingleObject(stop,20000)==WAIT_OBJECT_0;
    CloseHandle(start);CloseHandle(stop);
    return written&&released&&warmup.complete&&coldResult>=0&&wildcardResult==coldResult&&wildcardExact?0:4;
}
