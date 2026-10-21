#include "osu_player/collections.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace osu_player;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) { throw std::runtime_error{message}; } }
template<class Function> void until(Function function)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!function()) { check(std::chrono::steady_clock::now() < end, "collection worker timeout"); std::this_thread::sleep_for(2ms); }
}
struct Temporary {
    std::filesystem::path root;
    Temporary() {
        const auto base = std::filesystem::is_directory("/tmp/opencode") ? std::filesystem::path{"/tmp/opencode"} : std::filesystem::temp_directory_path();
        const auto pattern = (base / "osu-collections-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(),pattern.end()); buffer.push_back('\0');
        const auto* path = mkdtemp(buffer.data()); check(path != nullptr, "fixture creation failed"); root = path;
    }
    ~Temporary() { std::filesystem::remove_all(root); }
    std::filesystem::path path() const { return root / "private/lists.json"; }
};
TrackReference ref(const std::string& id) { return {id, std::string(64, 'a')}; }
void ready(CollectionsStore& store) { until([&] { return store.status().ready; }); }
void send(CollectionsStore& store, CollectionEdit edit)
{
    check(store.submit(std::move(edit)), "edit rejected unexpectedly");
    until([&] { return store.status().pending == 0; });
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream file{path}; return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
void write(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path()); chmod(path.parent_path().c_str(),0700);
    { std::ofstream file{path}; file << content; }
    chmod(path.c_str(),0600);
}
void lifecycle()
{
    Temporary temp;
    {
        CollectionsStore store{temp.path()}; ready(store);
        check(store.status().writable, "new store writable");
        send(store, {CollectionAction::favorite, {}, {}, ref("one")});
        send(store, {CollectionAction::create, "日本 favorites", {}, {}});
        send(store, {CollectionAction::add, "日本 favorites", {}, ref("two")});
        send(store, {CollectionAction::add, "日本 favorites", {}, ref("one")});
        send(store, {CollectionAction::add, "日本 favorites", {}, ref("one")});
        check(store.status().data->playlists[0].tracks.size() == 2, "duplicate adds idempotent");
        send(store, {CollectionAction::earlier, "日本 favorites", {}, ref("one")});
        check(store.status().data->playlists[0].tracks[0] == ref("one"), "playlist reordered");
        send(store, {CollectionAction::rename, "日本 favorites", "Renamed", {}});
        send(store, {CollectionAction::create, "Other", {}, {}});
        send(store, {CollectionAction::rename, "Renamed", "Other", {}});
        check(store.status().data->playlists[0].name == "Renamed", "duplicate rename rolled back");
        send(store, {CollectionAction::create, "bad\nname", {}, {}});
        check(store.status().data->playlists.size() == 2, "unsafe name rejected");
        struct stat info{}; stat(temp.path().c_str(), &info); check((info.st_mode & 0777) == 0600, "private file mode");
        stat(temp.path().parent_path().c_str(), &info); check((info.st_mode & 0777) == 0700, "private directory mode");
        CollectionsStore other{temp.path()}; ready(other);
        check(!other.status().writable && !other.submit({CollectionAction::create, "Clobber", {}, {}}), "single writer lock");
        // Accepted commands are drained by destruction, not silently lost on quit.
        check(store.submit({CollectionAction::add, "Renamed", {}, ref("missing")}), "final edit accepted");
    }
    {
        CollectionsStore store{temp.path()}; ready(store);
        check(store.status().data->favorites == std::vector<TrackReference>{ref("one")}, "favorites restored");
        check(store.status().data->playlists[0].tracks.size() == 3, "shutdown edit restored");
        Track one; one.stable_id = "one"; one.audio_hash = std::string(64,'a'); one.artist = "Artist"; one.title = "One"; one.beatmap_set_id = "1";
        Track two = one; two.stable_id = "two"; two.title = "Two"; two.beatmap_set_id = "2";
        Library library{{two,one}};
        check(store.status().data->favorite(one), "favorite lookup");
        check(store.status().data->resolve(library,false,"Renamed") == std::vector<std::size_t>({0,1}), "saved order resolved, missing references retained");
        one.audio_hash = std::string(64,'b');
        check(!store.status().data->favorite(one), "replaced audio does not inherit favorite");
        send(store,{CollectionAction::remove,"Renamed",{},ref("two")});
        send(store,{CollectionAction::erase,"Other",{}, {}});
        send(store,{CollectionAction::favorite,{}, {},ref("one")});
        check(store.status().data->favorites.empty() && store.status().data->playlists.size()==1, "remove/delete/toggle");
    }
}
void safety()
{
    for (const auto* content : {"", "{bad", "{\"version\":2}", "{\"version\":1,\"favorites\":{},\"playlists\":[]}"}) {
        Temporary temp; write(temp.path(),content);
        CollectionsStore store{temp.path()}; ready(store);
        check(!store.status().writable, "malformed/newer files block writes");
        check(read(temp.path()) == content, "malformed file preserved");
    }
    {
        Temporary temp; CollectionsStore store{temp.path()}; ready(store);
        send(store,{CollectionAction::favorite,{}, {},ref("one")});
        const auto saved=read(temp.path());
        write(temp.path(),"{\"version\":2}");
        send(store,{CollectionAction::favorite,{}, {},ref("two")});
        check(store.status().data->favorites.size()==1 && read(temp.path())=="{\"version\":2}", "external edits preserved and failed save rolled back");
        check(!saved.empty(), "successful initial save");
    }
    for (const bool hard : {false,true}) {
        Temporary temp;
        write(temp.root/"outside/file","preserve");
        std::filesystem::create_directory(temp.path().parent_path()); chmod(temp.path().parent_path().c_str(),0700);
        if (hard) { std::filesystem::create_hard_link(temp.root/"outside/file",temp.path()); }
        else { std::filesystem::create_symlink(temp.root/"outside/file",temp.path()); }
        CollectionsStore store{temp.path()}; ready(store);
        check(!store.status().writable && read(temp.root/"outside/file")=="preserve", "unsafe links refused without writes");
    }
    {
        Temporary temp; CollectionsStore store{temp.path()}; ready(store);
        std::filesystem::rename(temp.path().parent_path(),temp.root/"old");
        std::filesystem::create_directory(temp.path().parent_path()); chmod(temp.path().parent_path().c_str(),0700);
        send(store,{CollectionAction::favorite,{}, {},ref("one")});
        check(store.status().data->favorites.empty() && !std::filesystem::exists(temp.path()), "directory replacement cannot redirect writes");
    }
}
void limits()
{
    Temporary temp; CollectionsStore store{temp.path()}; ready(store);
    for (int i=0; i<128; ++i) { send(store,{CollectionAction::create,"List "+std::to_string(i),{}, {}}); }
    send(store,{CollectionAction::create,"Overflow",{}, {}});
    check(store.status().data->playlists.size()==128,"playlist limit is transactional");
    send(store,{CollectionAction::create,std::string(129,'x'),{}, {}});
    check(store.status().data->playlists.size()==128,"long names rejected");
    send(store,{CollectionAction::favorite,{}, {},{"bad", "invalid-hash"}});
    check(store.status().data->favorites.empty(),"invalid references not persisted");
}
void snapshot_and_noop()
{
    Temporary temp;
    CollectionsStore store{temp.path()}; ready(store);
    send(store, {CollectionAction::favorite, {}, {}, ref("日本")});
    send(store, {CollectionAction::favorite, {}, {}, ref("a")});
    send(store, {CollectionAction::create, "Ordered", {}, {}});
    send(store, {CollectionAction::add, "Ordered", {}, ref("日本")});
    send(store, {CollectionAction::add, "Ordered", {}, ref("a")});
    const auto snapshot = store.status().data;
    Collections copy = *snapshot;
    Track track; track.stable_id = "日本"; track.audio_hash = ref("").hash;
    check(copy.favorite(track), "copied favorite index owns positions, not borrowed strings");
    Track other = track; other.stable_id = "a"; other.title = "A"; other.beatmap_set_id = "a";
    track.title = "Z"; track.beatmap_set_id = "z";
    Library library{{track, other}};
    check(snapshot->resolve(library, false, "Ordered") == std::vector<std::size_t>({1,0}), "resolver preserves saved order rather than library order");
    check(snapshot->resolve(library, false, "absent").empty(), "unknown playlist is empty");
    track.audio_hash = std::string(64, 'b');
    check(snapshot->resolve(Library{{track,other}}, true, "") == std::vector<std::size_t>({0}), "resolver requires both ID and audio hash");
    struct stat before{}, after{};
    check(stat(temp.path().c_str(), &before) == 0, "stat before no-op");
    send(store, {CollectionAction::add, "Ordered", {}, ref("a")});
    send(store, {CollectionAction::earlier, "Ordered", {}, ref("日本")});
    check(stat(temp.path().c_str(), &after) == 0 && before.st_ino == after.st_ino &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec,
        "idempotent edits do not replace or touch file");
    check(store.status().data == snapshot && store.status().message == "Collections unchanged", "no-op preserves immutable snapshot");
    send(store, {CollectionAction::favorite, {}, {}, ref("日本")});
    track.audio_hash = ref("").hash;
    check(snapshot->favorite(track) && copy.favorite(track) && !store.status().data->favorite(track), "old snapshots survive mutation");
    write(temp.path(), "{\"version\":2}");
    send(store, {CollectionAction::add, "Ordered", {}, ref("a")});
    check(store.status().message.find("changed externally") != std::string::npos && read(temp.path()) == "{\"version\":2}",
        "no-op still checks external edits");
}
void utf8_validation()
{
    Temporary temp;
    CollectionsStore store{temp.path()}; ready(store);
    send(store, {CollectionAction::create, "Valid 日本 🎵", {}, {}});
    check(store.status().data->playlists.size() == 1, "valid multibyte names accepted");
    const auto saved = read(temp.path());
    for (const auto& bad : std::vector<std::string>{"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe3\x81", "\xc2x"}) {
        send(store, {CollectionAction::create, bad, {}, {}});
        send(store, {CollectionAction::rename, "Valid 日本 🎵", bad, {}});
        send(store, {CollectionAction::favorite, {}, {}, ref(bad)});
        send(store, {CollectionAction::add, "Valid 日本 🎵", {}, ref(bad)});
        check(read(temp.path()) == saved && store.status().data->favorites.empty(), "invalid UTF-8 edits rejected transactionally");
    }
    for (const auto& entries : {std::string{"[{\"id\":\"one\",\"hash\":\""} + ref("").hash + "\"},{\"id\":\"one\",\"hash\":\"" + ref("").hash + "\"}]",
                               std::string{"[{\"id\":\"bad\",\"hash\":\"invalid\"}]"}}) {
        Temporary malformed;
        write(malformed.path(), "{\"version\":1,\"favorites\":[],\"playlists\":[{\"name\":\"Bad\",\"tracks\":" + entries + "}]}");
        CollectionsStore invalid{malformed.path()}; ready(invalid);
        check(!invalid.status().writable, "model validation rejects duplicate/invalid decoded playlist members");
    }
}
}
int main()
{
    try { lifecycle(); safety(); limits(); snapshot_and_noop(); utf8_validation(); std::cout << "Collections private storage, background edits, restart, ordering, missing references and safety tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
