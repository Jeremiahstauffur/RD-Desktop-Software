#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Minimal background task helper.
//
//   Async::run([] { /* worker thread */ return heavyResult; },
//              [](auto result) { /* main (UI) thread */ });
//
// The continuation is queued and executed from Async::pump(), which the
// UI loop calls once per frame, so UI state is only ever touched on the
// main thread.
class Async {
public:
    template <typename Work, typename Done>
    static void run(Work&& work, Done&& done) {
        pending().fetch_add(1);
        std::thread([work = std::forward<Work>(work), done = std::forward<Done>(done)]() mutable {
            auto result = work();
            post([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
            pending().fetch_sub(1);
        }).detach();
    }

    static void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex());
        queue().push_back(std::move(fn));
    }

    static void pump() {
        std::vector<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lock(mutex());
            local.swap(queue());
        }
        for (auto& fn : local) fn();
    }

    static bool busy() { return pending().load() > 0; }

private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::vector<std::function<void()>>& queue() { static std::vector<std::function<void()>> q; return q; }
    static std::atomic<int>& pending() { static std::atomic<int> p{0}; return p; }
};
