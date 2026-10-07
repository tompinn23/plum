#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QRect>
#include <QString>

class QTimer;
class QWidget;

namespace journal {
    class commander_feed;
}

// Each feed's game window, followed by the pids its feed reports, so several clients played at
// once are each tracked against their own feed.
//
// On Windows a thread of its own runs a message loop with WinEvent hooks on top-level windows; pids
// are handed to it as thread messages. It works out on its own thread which events matter, and
// only wakes the UI thread when a game window changed. Elsewhere no window is ever found.
class game_windows : public QObject {
    Q_OBJECT

public:
    struct window {
        std::uint32_t pid = 0;
        quintptr handle = 0;
        QString title;
        QRect geometry; // the client area, in Qt's screen coordinates
        bool minimised = false;
        bool foreground = false; // the window the user is typing into
    };

    explicit game_windows(QObject *parent = nullptr);

    ~game_windows() override;

    // Follows the window of a feed's game. Tracking an id again replaces its feed.
    void track(const std::string &id, const std::shared_ptr<journal::commander_feed> &feed);

    void untrack(const std::string &id);

    // nullopt while the feed's game is not running or has no window yet.
    [[nodiscard]] std::optional<window> find(const std::string &id) const;

    // Marks one of Plum's overlays, so stacking one over the game passes over the others.
    static void mark_overlay(QWidget *window);

    // Puts one of Plum's overlays just above the game window in z-order, so whatever is in front
    // of the game is in front of the overlay too. Without a game window (0) it goes back to
    // staying on top of everything.
    static void stack_over(QWidget *overlay, quintptr game);

signals:
    // Something that may change find() for this feed: its window appeared, closed, moved, was
    // minimised or restored, or gained or lost the foreground.
    void changed(const std::string &id);

private:
    friend class game_windows_pump;

    // A game window as the pump sees it, before any Qt coordinates: physical pixels.
    struct native_window {
        std::uint32_t pid = 0;
        quintptr handle = 0;
        QString title;
        QRect client;
        bool minimised = false;
        bool foreground = false;
    };

    struct tracked {
        std::weak_ptr<journal::commander_feed> feed;
        std::vector<std::uint32_t> pids;
    };

    // What the pump has to say since the UI thread last looked; written on the pump thread.
    struct outbox {
        std::map<std::uint32_t, std::optional<native_window> > windows; // nullopt: gone
    };

    void check_pids();

    // UI thread: takes what the pump posted and tells the feeds it concerns.
    void drain();

    QTimer *pid_timer;
    std::map<std::string, tracked> feeds;
    std::map<std::uint32_t, native_window> windows; // per pid, as last reported

    std::mutex outbox_mutex;
    outbox pending;
    std::atomic<bool> drain_queued{false};

    std::thread pump;
    std::uint32_t pump_thread = 0; // its Win32 thread id, for posting to
};
