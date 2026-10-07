#include "feed_subscriber.hpp"

#include <QCoreApplication>

#include <utility>

#include <spdlog/spdlog.h>

feed_subscriber::feed_subscriber(QObject *context, std::shared_ptr<journal::commander_feed> feed)
    : context(context), data(std::move(feed)) {
    // Going live is reported again each time the game starts a new journal; only the first counts.
    // The subscriber itself is not a QObject, so the context stands guard for it.
    subs.push_back(data->on_progress([this, guard = this->context](const journal::ingest_progress &progress) {
        if (!progress.caught_up()) return;
        QMetaObject::invokeMethod(qApp, [this, guard] {
            if (guard) become_ready();
        }, Qt::QueuedConnection);
    }));
}

void feed_subscriber::become_ready() {
    if (is_ready) return;
    is_ready = true;
    const auto state = data->state();
    spdlog::debug("[{}] ready for {} handlers", data->id(), ready_handlers.size());
    if (!state) return;
    const journal::game_event event(journal::json{{"event", ready}});
    for (const auto &handler: std::exchange(ready_handlers, {})) handler(*state, event);
}

void feed_subscriber::subscribe(std::set<std::string> events, handler_fn handler) {
    if (events.erase(ready)) {
        if (!is_ready) {
            ready_handlers.push_back(handler);
        } else if (const auto state = data->state()) {
            QMetaObject::invokeMethod(qApp, [guard = context, handler, state] {
                if (guard) handler(*state, journal::game_event(journal::json{{"event", ready}}));
            }, Qt::QueuedConnection);
        }
        // Only Ready was asked for; an empty set would mean every event.
        if (events.empty()) return;
    }

    subs.push_back(data->subscribe(events,
                                   [guard = context, feed = std::weak_ptr(data), handler = std::move(handler)](
                               const journal::game_event &e, journal::phase) {
                                       const auto f = feed.lock();
                                       if (!f) return;
                                       auto state = f->state();
                                       QMetaObject::invokeMethod(qApp, [guard, handler, state = std::move(state), e] {
                                           if (guard) handler(*state, e);
                                       }, Qt::QueuedConnection);
                                   }));
}
