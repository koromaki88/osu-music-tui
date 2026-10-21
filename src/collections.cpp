#include "collections_file.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace osu_player {
struct CollectionsStore::Impl {
    std::mutex mutex;
    std::condition_variable condition;
    CollectionsStatus state{std::make_shared<Collections>(), false, false, 0, "Collections loading"};
    std::deque<CollectionEdit> edits;
    bool closing{};
    std::jthread worker;
    explicit Impl(std::filesystem::path path) : worker{[this, path = std::move(path)] { run(path); }} { }
    ~Impl()
    {
        { std::lock_guard lock{mutex}; closing = true; }
        condition.notify_all(); // Join after accepted edits have been processed.
    }
    void run(const std::filesystem::path& path)
    {
        using namespace collection_detail;
        try {
            CollectionFile storage{path};
            auto previous = storage.read();
            auto data = std::make_shared<Collections>(decode_collections(previous));
            { std::lock_guard lock{mutex}; state = {data, true, true, 0, "Collections ready"}; }
            for (;;) {
                CollectionEdit command;
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [&] { return closing || !edits.empty(); });
                    if (edits.empty() && closing) { return; }
                    command = std::move(edits.front()); edits.pop_front();
                }
                try {
                    auto candidate = std::make_shared<Collections>(*data);
                    apply_collection_edit(*candidate, command);
                    auto content = encode_collections(*candidate);
                    if (content == previous) {
                        // Preserve external-edit detection even for duplicate adds
                        // or boundary reorders, but do not rewrite/fsync the file.
                        if (storage.read() != previous) { throw std::runtime_error{"collections changed externally; restart to reload"}; }
                        std::lock_guard lock{mutex};
                        --state.pending; state.message = "Collections unchanged";
                        continue;
                    }
                    storage.write(content, previous);
                    previous = std::move(content); data = std::move(candidate);
                    std::lock_guard lock{mutex};
                    state.data = data; --state.pending; state.message = "Collections saved";
                } catch (const std::exception& error) {
                    std::lock_guard lock{mutex}; --state.pending;
                    state.message = "Collections not saved: " + std::string{error.what()};
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock{mutex}; state.ready = true; state.writable = false;
            state.message = "Collections unavailable: " + std::string{error.what()};
        }
    }
};
CollectionsStore::CollectionsStore(std::filesystem::path path) : impl_{std::make_unique<Impl>(std::move(path))} { }
CollectionsStore::~CollectionsStore() = default;
CollectionsStatus CollectionsStore::status() const { std::lock_guard lock{impl_->mutex}; return impl_->state; }
bool CollectionsStore::submit(CollectionEdit edit)
{
    std::lock_guard lock{impl_->mutex};
    if (!impl_->state.ready || !impl_->state.writable || impl_->closing) { return false; }
    if (impl_->state.pending >= collection_limits::pending_edits) { impl_->state.message = "Collections busy: edit not accepted; retry"; return false; }
    impl_->edits.push_back(std::move(edit)); ++impl_->state.pending; impl_->state.message = "Collections saving";
    impl_->condition.notify_one(); return true;
}
} // namespace osu_player
