#pragma once
// One lazy worker, a bounded queue, and owned results. Shell calls never hold
// the UI mutex. A stopped worker owns only its shared state, never UI objects.
#include <windows.h>
#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace ft::apps {
struct IconDeleter { void operator()(HICON icon)const{if(icon)DestroyIcon(icon);} };
using Icon=std::unique_ptr<std::remove_pointer_t<HICON>,IconDeleter>;
struct IconResult { std::wstring path; unsigned long long generation=0; Icon icon; };
class AsyncIcons {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::wstring> requests;
        std::vector<IconResult> results;
        std::unordered_set<std::wstring> pending;
        std::function<HICON(const std::wstring&)> extract;
        unsigned long long generation=0;
        bool stopped=false,started=false;
    };
    std::shared_ptr<State> state=std::make_shared<State>();
    static void Work(std::shared_ptr<State> s) {
        HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
        for(;;){
            std::wstring path; unsigned long long generation;
            {
                std::unique_lock<std::mutex> lock(s->mutex);
                s->changed.wait(lock,[&]{return s->stopped || !s->requests.empty();});
                if(s->stopped)break;
                path=std::move(s->requests.front());s->requests.pop_front();generation=s->generation;
            }
            Icon icon;
            if(s->extract)icon.reset(s->extract(path));
            else if(SUCCEEDED(initialized)){
                SHFILEINFOW info{};
                if(SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_LARGEICON))icon.reset(info.hIcon);
            }
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if(!s->stopped && generation==s->generation)
                    s->results.push_back({std::move(path),generation,std::move(icon)});
            }
        }
        if(SUCCEEDED(initialized))CoUninitialize();
    }
public:
    static constexpr size_t kPendingLimit=64;
    explicit AsyncIcons(std::function<HICON(const std::wstring&)> extract={}){state->extract=std::move(extract);}
    ~AsyncIcons(){Stop();}
    void Reset(unsigned long long generation) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->generation=generation;state->requests.clear();state->results.clear();state->pending.clear();
    }
    bool Request(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->stopped)return false;
        if(state->pending.count(path))return true;
        if(state->pending.size()>=kPendingLimit)return false;
        if(!state->started){
            try{std::thread(Work,state).detach();state->started=true;}
            catch(const std::system_error&){return false;}
        }
        state->pending.insert(path);state->requests.push_back(path);state->changed.notify_one();return true;
    }
    std::vector<IconResult> Drain() {
        std::lock_guard<std::mutex> lock(state->mutex);
        std::vector<IconResult> out;out.swap(state->results);
        for(const auto& r:out)state->pending.erase(r.path);
        return out;
    }
    void Stop() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopped=true;state->requests.clear();state->results.clear();state->pending.clear();state->changed.notify_one();
    }
};
} // namespace ft::apps
