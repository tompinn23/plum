#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QPointer>

#include "journal/journal_service.hpp"

class QObject;

namespace journal {
    class game_event;
    struct game_state;
}

// Events from a commander's feed, delivered on the UI thread. For anything that listens to a feed:
// dashboards, and models or windows that are not.
//
// Not a QObject, so it can sit beside whichever Qt class the listener is. List it after that base,
// and pass `this` as the context: handlers are dropped once the context is destroyed, so one still
// in flight from the journal thread never reaches a half-destroyed object.
class feed_subscriber {
public:
    // Not a journal event: sent once, to handlers that subscribe to it, when the feed has finished
    // reading history, so a listener can start from state() and history() whether or not the game
    // is running. A handler subscribed after that gets it straight away.
    static constexpr auto ready = "Ready";

    feed_subscriber(QObject *context, std::shared_ptr<journal::commander_feed> feed);

    virtual ~feed_subscriber() = default;

    feed_subscriber(const feed_subscriber &) = delete;

    feed_subscriber &operator=(const feed_subscriber &) = delete;

protected:
    using handler_fn = std::function<void(const journal::game_state &, const journal::game_event &)>;

    // Live events with these names, or every event for an empty set. Unsubscribed with this object.
    void subscribe(std::set<std::string> events, handler_fn handler);

    [[nodiscard]] const std::shared_ptr<journal::commander_feed> &feed() const { return data; }

private:
    void become_ready();

    QPointer<QObject> context;
    std::shared_ptr<journal::commander_feed> data;
    std::vector<journal::subscription> subs;
    std::vector<handler_fn> ready_handlers; // until the feed is ready
    bool is_ready = false;
};
