// One-off allocation/work benchmark; see RESULTS.md. Not linked into the player.
#include "osu_player/collections.hpp"
#include "osu_player/library_view.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <thread>
#include <sys/stat.h>

static std::atomic<bool> counting{};
static std::atomic<std::size_t> allocations{}, allocated_bytes{};
void* operator new(std::size_t size) {
    if(counting) { ++allocations; allocated_bytes += size; }
    if(auto* p=std::malloc(size ? size : 1)) {return p;} throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
template<class F> void bench(const char* name, F f) {
    allocations=0; allocated_bytes=0; counting=true;
    auto start=std::chrono::steady_clock::now();
    const auto result=f();
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    counting=false;
    std::cout<<name<<", allocations="<<allocations<<", requested_bytes="<<allocated_bytes<<", ms="<<ms<<", result="<<result<<'\n';
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const std::filesystem::path root=argv[1];
    std::filesystem::create_directories(root/"private"); chmod((root/"private").c_str(),0700);
    std::string hash(64,'a');
    {std::ofstream out{root/"private/lists.json"};out<<"{\"version\":1,\"favorites\":[{\"id\":\"0\",\"hash\":\""<<hash<<"\"}],\"playlists\":[]}";}
    chmod((root/"private/lists.json").c_str(),0600);
    osu_player::CollectionsStore store{root/"private/lists.json"};
    while(!store.status().ready) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    auto data=store.status().data;
    std::vector<osu_player::Track> tracks;
    for(int i=0;i<50000;++i) {
        osu_player::Track t;t.stable_id=std::to_string(i);t.beatmap_set_id=t.stable_id;t.audio_hash=hash;
        t.artist="Synthetic long artist for search";t.title="Synthetic long title "+std::to_string(i);tracks.push_back(std::move(t));
    }
    osu_player::Library library{std::move(tracks)};
    bench("favorite_marker_10000",[&]{std::size_t hits=0;for(int i=0;i<10000;++i)hits+=data->favorite(library.tracks()[0]);return hits;});
    bench("resolve_one_in_50000",[&]{return data->resolve(library,true,"").size();});
    osu_player::LibraryView view{library}; view.set_scope(std::vector<std::size_t>{0,1,2});
    bench("scoped_search_20",[&]{std::size_t hits=0;for(int i=0;i<20;++i){view.set_query(i%2?"artist":"title");hits+=view.matches().size();}return hits;});
}
