#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QDateTime>
#include <QHash>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;
class QVBoxLayout;
class game_windows;
class overlays;

namespace journal {
    class commander_feed;
}

struct notification {
    enum class level { info, good, warning };

    QString title;
    QString text;
    level kind = level::info;
    // Posts with the same key, about the same commander, close together merge into one with a
    // count. Empty: never merged.
    QString key;
    std::chrono::milliseconds lifetime = std::chrono::seconds(6);

    // Filled in by the poster's dashboard and by overlays, not by whoever writes the notification.
    QString source; // the commander it is about
    std::string feed; // the feed it came from, to show it over that commander's game; empty: every one
    QDateTime at;
    int count = 1;
};

// Where an overlay window sits over the game: a corner of the game's client area, how far in from
// it, and how big it is. All in Qt pixels. A height of 0 fits the window to its content.
struct overlay_placement {
    Qt::Corner corner = Qt::TopRightCorner;
    QPoint margin{24, 24};
    QSize size{340, 0};
};

// A frameless window over one feed's game, kept on its corner and just above the game in z-order
// as the game window moves, so whatever is in front of the game is in front of it too. Hidden
// while the game is minimised.
// Spawned empty by overlays: fill body() with whatever it should show.
class overlay_window : public QWidget {
    Q_OBJECT

public:
    [[nodiscard]] QVBoxLayout *body() const { return layout_; }

    [[nodiscard]] const std::string &feed_id() const { return feed; }

    // Whether it has anything to show. An inactive overlay stays hidden whatever the game does.
    void set_active(bool active);

    [[nodiscard]] bool is_active() const { return active; }

    void set_placement(const overlay_placement &placement);

    [[nodiscard]] const overlay_placement &placement() const { return where; }

    // Puts it back in place after its content changed size; only needed for a fitted height.
    void refit();

private:
    friend class overlays;

    // Click-through unless `interactive`; never takes keyboard focus either way.
    overlay_window(overlays &owner, std::string feed, const overlay_placement &placement, bool interactive);

    overlays &owner;
    std::string feed;
    overlay_placement where;
    QVBoxLayout *layout_;
    bool active = true;
};

// Every overlay, per feed: each feed's game window gets its own, so commanders played at once each
// see theirs over their own game. A feed's notifications show as timed cards in its top-right
// corner while its game runs: newest on top, each fading out after its lifetime, a repeat folded
// into the card already showing. Notifications about no feed in particular show over every game.
// Anything else is spawned on demand.
//
// Without a known game window (the game is starting, or this platform cannot track windows)
// overlays fall back to the primary screen while the game runs.
class overlays : public QObject {
    Q_OBJECT

public:
    explicit overlays(game_windows &windows, QObject *parent = nullptr);

    ~overlays() override;

    // Starts overlaying a feed's game, with a stack of its notifications.
    void add_feed(const std::shared_ptr<journal::commander_feed> &feed);

    // Closes every overlay of the feed.
    void remove_feed(const std::string &id);

    // A new, empty overlay over the feed's game. Stays until close() or the feed goes; null if the
    // feed is not known.
    overlay_window *spawn(const std::string &feed_id, const overlay_placement &placement, bool interactive = false);

    void close(overlay_window *window);

    // Every notification in the app comes through here, on the UI thread: dashboards and their
    // panels post.
    void post(notification n);

private:
    friend class overlay_window;

    struct card {
        QLabel *label = nullptr;
        QTimer *timer = nullptr;
    };

    // One feed's notification cards. The window is active only while there are cards.
    struct toast_stack {
        overlay_window *window = nullptr;
        QHash<QString, card> by_key; // live cards that later posts may merge into
    };

    struct feed_overlays {
        std::weak_ptr<journal::commander_feed> feed;
        std::vector<overlay_window *> windows;
        toast_stack toasts;
    };

    // Moves the feed's overlays to where its game window is, showing the ones that can be seen.
    void place(const std::string &id);

    void place(overlay_window *window);

    // A new notification, as a card over each game it is about.
    void toast(const notification &n);

    // A repeat: the card already showing says so, and stays a while longer.
    void bump(const notification &n);

    void add_card(feed_overlays &f, const notification &n);

    static void dismiss(toast_stack &stack, QLabel *label);

    game_windows &windows;
    std::map<std::string, feed_overlays> feeds;
    std::optional<notification> last; // the newest, for a repeat to merge into
};
