// Platform-independent watcher members. Included at the end of each backend .cpp,
// after that backend has defined watcher::Impl with:
//   explicit Impl(Handler);  // Handler is called on the backend's worker thread
//   std::error_code add(const std::filesystem::path&);
//   std::error_code remove(const std::filesystem::path&);
//
// Not `inline`: these are defined in exactly one translation unit and called from others,
// so they must be emitted here.

#include <condition_variable>
#include <deque>
#include <mutex>

namespace notify {

struct watcher::Queue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<result> items;

    void push(result r) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            items.push_back(std::move(r));
        }
        cv.notify_one();
    }

    result pop_locked() {
        result r = std::move(items.front());
        items.pop_front();
        return r;
    }
};

watcher::watcher() : queue_(std::make_unique<Queue>()) {
    // The queue lives on the heap, so this pointer stays valid if the watcher is moved.
    impl_ = std::make_unique<Impl>([q = queue_.get()](result r) { q->push(std::move(r)); });
}

// impl_ is destroyed (worker joined) before queue_, as members die in reverse order.
watcher::~watcher() = default;

watcher::watcher(watcher&&) noexcept = default;

watcher& watcher::operator=(watcher&&) noexcept = default;

void watcher::watch(const std::filesystem::path& path) {
    std::error_code ec;
    const auto normal = detail::normalize(path, ec);
    if (!ec) ec = impl_->add(normal);
    if (ec) throw std::filesystem::filesystem_error("notify: watch failed", path, ec);
}

void watcher::unwatch(const std::filesystem::path& path) {
    std::error_code ec;
    const auto normal = detail::normalize(path, ec);
    if (!ec) ec = impl_->remove(normal);
    if (ec) throw std::filesystem::filesystem_error("notify: unwatch failed", path, ec);
}

result watcher::receive() const {
    std::unique_lock lock(queue_->mutex);
    queue_->cv.wait(lock, [&] { return !queue_->items.empty(); });
    return queue_->pop_locked();
}

std::optional<result> watcher::receive_for(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(queue_->mutex);
    if (!queue_->cv.wait_for(lock, timeout, [&] { return !queue_->items.empty(); }))
        return std::nullopt;
    return queue_->pop_locked();
}

}  // namespace notify
