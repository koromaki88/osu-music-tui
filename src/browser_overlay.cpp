#include "osu_player/browser_overlay.hpp"
#include "overlay_config.hpp"

#include <json-c/json.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace osu_player {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view host = "127.0.0.1:7270";
constexpr std::string_view origin = "http://127.0.0.1:7270";
constexpr std::string_view page = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>osu! music player — OBS overlay</title>
<style>
html,body{margin:0;background:transparent;color:white;font-family:system-ui,sans-serif}
#card{box-sizing:border-box;width:100%;min-height:180px;padding:20px 24px;background:rgba(16,18,24,.85);border-radius:14px}
#card{display:flex;align-items:center;gap:20px}#content{flex:1;min-width:0}
#thumbnail{width:112px;height:112px;object-fit:cover;flex:none;border-radius:8px}#thumbnail[hidden]{display:none}
#title{font-size:32px;font-weight:650}#artist{font-size:23px;opacity:.85;margin-top:4px}
#state{font-size:16px;opacity:.7;margin-top:10px}#title,#artist{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
progress{display:block;width:100%;height:5px;margin-top:12px;accent-color:#ff78ae}progress[hidden]{display:none}
</style>
<div id="card"><img id="thumbnail" alt="" referrerpolicy="no-referrer" hidden><div id="content"><div id="title">Player disconnected</div><div id="artist"></div><div id="state"></div><progress id="progress" hidden></progress></div></div>
<script>
'use strict';
const title=document.getElementById('title'),artist=document.getElementById('artist'),
state=document.getElementById('state'),progress=document.getElementById('progress'),thumbnail=document.getElementById('thumbnail');
const showProgress=new URLSearchParams(location.search).get('progress')!=='0';
let configuredProgress=true,configuredThumbnail=true,nextStyle=0,coverUrl='',failedCover='';
thumbnail.onload=()=>{thumbnail.hidden=!configuredThumbnail||!coverUrl};
thumbnail.onerror=()=>{failedCover=coverUrl;thumbnail.hidden=true};
function updateCover(url){
  const next=configuredThumbnail&&typeof url==='string'&&/^https:\/\/assets\.ppy\.sh\/beatmaps\/[1-9][0-9]*\/covers\/list@2x\.jpg$/.test(url)?url:'';
  if(next===coverUrl)return;
  coverUrl=next;thumbnail.hidden=true;thumbnail.removeAttribute('src');
  if(next&&next!==failedCover){thumbnail.src=next}
}
function applyStyle(c){
  const card=document.getElementById('card');
  card.style.maxWidth=c.width===null?'none':c.width+'px';card.style.minHeight=c.height+'px';
  card.style.fontFamily='"'+c.font+'", system-ui, sans-serif';card.style.color=c.text_color;
  card.style.borderRadius=c.corner_radius+'px';
  const rgb=[1,3,5].map(i=>parseInt(c.background.slice(i,i+2),16));
  card.style.backgroundColor='rgba('+rgb.join(',')+','+c.background_opacity+')';
  title.style.fontSize=c.title_size+'px';artist.style.fontSize=c.artist_size+'px';
  progress.style.accentColor=c.accent_color;state.hidden=!c.show_time;configuredProgress=c.show_progress;
  configuredThumbnail=c.show_thumbnail;thumbnail.style.width=c.thumbnail_size+'px';thumbnail.style.height=c.thumbnail_size+'px';
}
const time=ms=>{const s=Math.floor(Math.max(0,ms)/1000);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
async function update(){
  const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),1500);
  try{
    const response=await fetch('/state',{cache:'no-store',signal:controller.signal});
    if(!response.ok)throw new Error('Disconnected');
    const data=await response.json();
    if(Date.now()>=nextStyle){
      nextStyle=Date.now()+2000;
      try{const style=await fetch('/style',{cache:'no-store',signal:controller.signal});
        if(style.ok)applyStyle(await style.json());}catch(_){} // Keep last valid appearance.
    }
    title.textContent=data.state==='idle'?'Nothing playing':data.title;
    artist.textContent=data.artist;
    updateCover(data.state==='idle'?null:data.thumbnail_url);
    state.textContent=data.state==='idle'?'':(data.state==='paused'?'Paused · ':'')+time(data.position_ms)+(data.duration_ms?' / '+time(data.duration_ms):'');
    progress.hidden=!showProgress||!configuredProgress||!data.duration_ms;progress.max=data.duration_ms||1;progress.value=Math.min(data.position_ms,progress.max);
  }catch(_){title.textContent='Player disconnected';artist.textContent='';state.textContent='';progress.hidden=true;updateCover(null)}
  finally{clearTimeout(timer);setTimeout(update,500)}
}
update();
</script></html>)HTML";

struct Socket {
    int fd{-1};
    explicit Socket(int value = -1) : fd{value} { }
    ~Socket() { if (fd >= 0) { close(fd); } }
    Socket(Socket&& other) noexcept : fd{std::exchange(other.fd, -1)} { }
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other) { if (fd >= 0) { close(fd); } fd = std::exchange(other.fd, -1); }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
struct Display {
    std::string title, artist;
    std::string thumbnail_url;
    std::int64_t position{}, duration{};
    bool active{}, playing{};
};
std::string limited(const std::string& text)
{
    auto length = std::min(text.size(), std::size_t{4096});
    // Metadata is validated UTF-8; avoid cutting through its last codepoint.
    while (length < text.size() && length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) { --length; }
    return text.substr(0, length);
}
std::string state_json(const Display& display, const bool show_thumbnail)
{
    std::unique_ptr<json_object, decltype(&json_object_put)> root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "state", json_object_new_string(!display.active ? "idle" : display.playing ? "playing" : "paused"));
    json_object_object_add(root.get(), "title", json_object_new_string_len(display.title.data(), static_cast<int>(display.title.size())));
    json_object_object_add(root.get(), "artist", json_object_new_string_len(display.artist.data(), static_cast<int>(display.artist.size())));
    json_object_object_add(root.get(), "position_ms", json_object_new_int64(std::max(std::int64_t{0}, display.position)));
    json_object_object_add(root.get(), "duration_ms", display.duration > 0 ? json_object_new_int64(display.duration) : nullptr);
    json_object_object_add(root.get(), "thumbnail_url", show_thumbnail && !display.thumbnail_url.empty() ? json_object_new_string(display.thumbnail_url.c_str()) : nullptr);
    return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
std::string response(const int code, const std::string_view type, const std::string_view body)
{
    return "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK\r\n" : " Rejected\r\n") +
        "Content-Type: " + std::string{type} + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; img-src https://assets.ppy.sh; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n\r\n" + std::string{body};
}
std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) { value.remove_prefix(1); }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) { value.remove_suffix(1); }
    return value;
}
// Exact Host plus Origin checks prevent DNS-rebinding and cross-origin reads.
// No CORS, file serving, arbitrary URLs, commands, or mutating routes.
std::string route(const std::string& request, const Display& display, const std::string& style)
{
    const auto first_end = request.find("\r\n");
    const auto first = std::string_view{request}.substr(0, first_end);
    bool host_seen{}, origin_seen{};
    for (auto offset = first_end + 2; offset < request.size() - 2;) {
        const auto end = request.find("\r\n", offset);
        if (end == std::string::npos || end == offset) { break; }
        const auto line = std::string_view{request}.substr(offset, end - offset);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) { return response(400, "text/plain", "Bad request"); }
        std::string name{line.substr(0, colon)};
        for (auto& byte : name) { byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte))); }
        const auto value = trim(line.substr(colon + 1));
        if (name == "host") {
            if (host_seen || value != host) { return response(403, "text/plain", "Forbidden host"); }
            host_seen = true;
        } else if (name == "origin") {
            if (origin_seen || value != origin) { return response(403, "text/plain", "Forbidden origin"); }
            origin_seen = true;
        } else if (name == "sec-fetch-site" && value != "same-origin" && value != "none") {
            return response(403, "text/plain", "Forbidden site");
        }
        offset = end + 2;
    }
    if (!host_seen) { return response(403, "text/plain", "Host required"); }
    if (!first.starts_with("GET ")) { return response(405, "text/plain", "GET required"); }
    const auto path_end = first.find(' ', 4);
    if (path_end == std::string_view::npos || first.substr(path_end + 1) != "HTTP/1.1") { return response(400, "text/plain", "Bad request"); }
    const auto path = first.substr(4, path_end - 4);
    if (path == "/overlay" || path == "/overlay?progress=0" || path == "/") { return response(200, "text/html; charset=utf-8", page); }
    if (path == "/state") {
        std::unique_ptr<json_object, decltype(&json_object_put)> config{json_tokener_parse(style.c_str()), json_object_put};
        json_object* enabled{};
        json_object_object_get_ex(config.get(), "show_thumbnail", &enabled);
        return response(200, "application/json; charset=utf-8", state_json(display, json_object_get_boolean(enabled) != 0));
    }
    if (path == "/style") { return response(200, "application/json; charset=utf-8", style); }
    return response(404, "text/plain", "Not found");
}
struct Client {
    Socket socket;
    Clock::time_point deadline{Clock::now() + std::chrono::seconds{2}};
    std::string input, output;
    std::size_t sent{};
};
} // namespace

struct BrowserOverlay::Impl {
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool enabled{}, closing{};
    std::uint64_t generation{};
    std::string message{"off"};
    std::shared_ptr<const Display> display{std::make_shared<Display>()};
    const std::filesystem::path config_path;
    std::jthread worker;
    explicit Impl(std::filesystem::path path) : config_path{std::move(path)}, worker{[this] { run(); }} { }
    ~Impl()
    {
        { std::lock_guard lock{mutex}; closing = true; }
        condition.notify_all();
        // worker declared last, so join precedes destruction of mailbox members.
    }
    void run() noexcept
    {
        try {
            auto style = overlay_detail::default_style();
            for (;;) {
                std::uint64_t current{};
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [&] { return closing || enabled; });
                    if (closing) { return; }
                    current = generation;
                }
                Socket listener{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
                const int reuse = 1;
                if (listener.fd >= 0) { setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); }
                sockaddr_in address{};
                address.sin_family = AF_INET; address.sin_port = htons(7270);
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                const bool ready = listener.fd >= 0 && bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                    listen(listener.fd, 8) == 0;
                {
                    std::unique_lock lock{mutex};
                    if (closing) { return; }
                    if (!enabled || generation != current) { continue; }
                    message = ready ? "http://127.0.0.1:7270/overlay" : "unavailable (cannot bind 127.0.0.1:7270; toggle O to retry)";
                    if (!ready) {
                        condition.wait(lock, [&] { return closing || generation != current; });
                        if (closing) { return; }
                        continue;
                    }
                }
                std::vector<Client> clients;
                clients.reserve(8);
                auto next_style_check = Clock::now();
                bool initialize_style = true;
                for (;;) {
                    std::shared_ptr<const Display> latest;
                    {
                        std::lock_guard lock{mutex};
                        if (closing) { return; }
                        if (!enabled || generation != current) { break; }
                        latest = display;
                    }
                    if (Clock::now() >= next_style_check) {
                        std::string warning;
                        try {
                            if (initialize_style) { overlay_detail::create_style_if_missing(config_path); }
                            style = overlay_detail::load_style(config_path);
                        } catch (const std::exception& error) { warning = error.what(); }
                        initialize_style = false;
                        next_style_check = Clock::now() + std::chrono::seconds{1};
                        std::lock_guard lock{mutex};
                        if (enabled && generation == current) {
                            message = "http://127.0.0.1:7270/overlay" + (warning.empty() ? std::string{} : " | style warning: " + warning);
                        }
                    }
                    std::array<pollfd, 9> descriptors{};
                    descriptors[0] = {listener.fd, POLLIN, 0};
                    for (std::size_t i = 0; i < clients.size(); ++i) {
                        descriptors[i + 1] = {clients[i].socket.fd, static_cast<short>(clients[i].output.empty() ? POLLIN : POLLOUT), 0};
                    }
                    const auto count = clients.size();
                    const int result = poll(descriptors.data(), static_cast<nfds_t>(count + 1), 100);
                    if (result < 0 && errno != EINTR) { break; }
                    for (std::size_t i = count; i-- > 0;) {
                        auto& client = clients[i];
                        const auto events = descriptors[i + 1].revents;
                        bool done = Clock::now() >= client.deadline || (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
                        if (!done && (events & POLLIN) != 0) {
                            char buffer[1024];
                            const auto bytes = recv(client.socket.fd, buffer, sizeof(buffer), 0);
                            if (bytes > 0) {
                                client.input.append(buffer, static_cast<std::size_t>(bytes));
                                if (client.input.size() > 4096) { done = true; }
                                else if (client.input.find("\r\n\r\n") != std::string::npos) { client.output = route(client.input, *latest, style); }
                            } else if (bytes == 0 || (errno != EAGAIN && errno != EINTR)) { done = true; }
                        }
                        if (!done && (events & POLLOUT) != 0) {
                            const auto bytes = send(client.socket.fd, client.output.data() + client.sent, client.output.size() - client.sent, MSG_NOSIGNAL);
                            if (bytes > 0) { client.sent += static_cast<std::size_t>(bytes); done = client.sent == client.output.size(); }
                            else if (bytes == 0 || (errno != EAGAIN && errno != EINTR)) { done = true; }
                        }
                        if (done) { clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i)); }
                    }
                    if ((descriptors[0].revents & POLLIN) != 0) {
                        Socket peer{accept4(listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
                        if (peer.fd >= 0 && clients.size() < 8) { clients.push_back({std::move(peer), Clock::now() + std::chrono::seconds{2}, {}, {}, 0}); }
                    }
                }
            }
        } catch (...) {
            std::lock_guard lock{mutex};
            message = "unavailable (overlay worker failed; restart player)";
        }
    }
};
BrowserOverlay::BrowserOverlay(std::filesystem::path config_path) : impl_{std::make_unique<Impl>(std::move(config_path))} { }
BrowserOverlay::~BrowserOverlay() = default;
void BrowserOverlay::configure(const bool enabled)
{
    { std::lock_guard lock{impl_->mutex}; impl_->enabled = enabled; ++impl_->generation; impl_->message = enabled ? "starting" : "off"; }
    impl_->condition.notify_one();
}
void BrowserOverlay::publish(const std::optional<PlaybackSnapshot>& snapshot)
{
    auto display = std::make_shared<Display>();
    if (snapshot) {
        display->active = true; display->playing = snapshot->playing;
        display->title = limited(snapshot->track.title_unicode.empty() ? snapshot->track.title : snapshot->track.title_unicode);
        display->artist = limited(snapshot->track.artist_unicode.empty() ? snapshot->track.artist : snapshot->track.artist_unicode);
        display->position = snapshot->position.count();
        display->duration = snapshot->duration ? snapshot->duration->count() : 0;
        if (snapshot->track.online_set_id && *snapshot->track.online_set_id > 0) {
            display->thumbnail_url = "https://assets.ppy.sh/beatmaps/" + std::to_string(*snapshot->track.online_set_id) + "/covers/list@2x.jpg";
        }
    }
    std::lock_guard lock{impl_->mutex}; impl_->display = std::move(display);
}
std::string BrowserOverlay::status() const { std::lock_guard lock{impl_->mutex}; return impl_->message; }
} // namespace osu_player
