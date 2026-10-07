#pragma once

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "journal/game_event.hpp"
#include "journal/game_state.hpp"
#include "journal/history.hpp"

namespace journal {
    // Where a journal line came from, and so how much the pipeline may do with it.
    //
    // Elite restates the whole session at the top of every journal file, so the current state is a
    // function of the newest file alone; older files only matter for facts the game never restates.
    enum class phase {
        // Replay of a journal older than the newest. Describes the past: nothing outside the line
        // itself may be consulted.
        history,
        // Replay of the newest journal up to where it currently ends. Rebuilds the present state,
        // under the same restrictions as history.
        catchup,
        // A line that has just been written. Sibling files are current and side effects are meaningful.
        live,
    };

    const char *to_string(phase p);

    // How far through its journal directory a feed has got.
    struct ingest_progress {
        journal::phase stage = journal::phase::history;
        int files_done = 0;
        int files_skipped = 0; // of files_done: already stored, or another commander's
        int files_total = 0;

        [[nodiscard]] double fraction() const {
            return files_total == 0 ? 1.0 : std::min(1.0, double(files_done) / files_total);
        }

        [[nodiscard]] bool caught_up() const { return stage == journal::phase::live; }
    };

    // Unsubscribes when destroyed. A listener may still be called once if it is already being
    // delivered to on the ingest thread when this goes away.
    class [[nodiscard]] subscription {
    public:
        subscription() = default;

        explicit subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        }

        ~subscription() { unsubscribe(); }

        subscription(subscription &&other) noexcept : cancel_(std::exchange(other.cancel_, {})) {
        }

        subscription &operator=(subscription &&other) noexcept {
            if (this != &other) {
                unsubscribe();
                cancel_ = std::exchange(other.cancel_, {});
            }
            return *this;
        }

        subscription(const subscription &) = delete;

        subscription &operator=(const subscription &) = delete;

        void unsubscribe() {
            if (const auto cancel = std::exchange(cancel_, {})) cancel();
        }

    private:
        std::function<void()> cancel_;
    };

    // Called on the service's ingest thread, which is shared by every feed: anything slow, and
    // anything that must reach another thread such as a UI toolkit, has to hand off rather than work
    // here. Exceptions are logged and swallowed.
    using event_listener = std::function<void(const game_event &, phase)>;
    using state_listener = std::function<void(std::shared_ptr<const game_state>)>;
    using progress_listener = std::function<void(const ingest_progress &)>;

    // One commander's journal directory: the historical scan, the startup catch-up and the live tail
    // behind a single handle.
    //
    // journal_service::open builds the feed and starts watching, but reads nothing until start().
    // Attach subscribers first: the scan publishes as it goes, so a subscriber attached afterwards
    // would miss it.
    class commander_feed {
    public:
        virtual ~commander_feed() = default;

        [[nodiscard]] virtual const std::string &id() const = 0;

        [[nodiscard]] virtual const std::filesystem::path &directory() const = 0;

        // The latest published snapshot; never the parser's working copy. Safe from any thread.
        [[nodiscard]] virtual std::shared_ptr<const game_state> state() const = 0;

        // Accumulated history, or a disabled history if the service has no history_config. Use this,
        // not a phase::history subscription, to ask what has happened over time: once a journal is
        // in the store it is never read again, so historical events are delivered exactly once ever.
        //
        // History is kept per commander, not per directory: each journal is recorded into the store
        // of whoever wrote it, so a directory shared by several commanders keeps all their
        // histories, and a commander who plays from several directories has one. This is the
        // history of whoever is playing here, following state() from one journal to the next, or
        // before anything is read, whoever this directory last saw. Disabled until anyone is known.
        [[nodiscard]] virtual history history() const = 0;

        // Events named in `events` (empty for all) during the given phases. The default is live
        // only, since replayed history would otherwise arrive as a flood at startup.
        virtual subscription subscribe(std::set<std::string> events, event_listener listener,
                                       std::set<phase> phases) = 0;

        subscription subscribe(const std::set<std::string> &events, const event_listener &listener) {
            return subscribe(events, listener, {phase::live});
        }


        // As the historical scan advances, once more on going live, and again each time the game
        // starts a new journal after that, with files_total counting it.
        virtual subscription on_progress(progress_listener listener) = 0;

        // Whether the game is running for the account that owns this directory, as the service's
        // game_probe last found it. Safe from any thread.
        //
        // While it is, the feed synthesises a StartUp event from state(): to every subscriber when it
        // goes live, and to each live subscriber that asks for StartUp (or for everything) when it
        // subscribes. It is the session as it stands, for anything that starts listening mid-session
        // and would otherwise wait for the next LoadGame. StartUp is never written to a journal or
        // recorded in history, and is not sent when the game is launched while being watched, since
        // the journal then restates the session itself.
        //
        // Likewise, when the game stops without having written Shutdown (it crashed or was killed),
        // the feed sends a Shutdown of its own to live subscribers, so nothing goes on showing a
        // session that has ended. Also never recorded.
        [[nodiscard]] virtual bool game_running() const = 0;

        // Delivers an event from outside the journal, such as a Companion API response, to live
        // subscribers in order with journal events. Like StartUp it is never parsed into state() or
        // recorded in history: both are rebuilt from journals alone. Safe from any thread; dropped if
        // the feed has stopped.
        virtual void inject(game_event event) = 0;

        // Begins the scan on the ingest thread and returns immediately. Later calls do nothing.
        virtual void start() = 0;
    };

    // Whether the game is running for whoever owns a journal directory. Called on the ingest thread,
    // when a directory is opened and every few seconds after, so it has to be quick.
    using game_probe = std::function<bool(const std::filesystem::path & directory)>;

    // The default probe: a running EliteDangerous64.exe (natively, or under Wine or Proton) owned by
    // the account that owns the directory. Once found, its pid is tracked, so the full process scan
    // only runs while the game is not up, and then at most every few seconds.
    game_probe process_game_probe();

    // The commander named by the newest journal in `directory` that names anyone, read straight from
    // the files. nullopt if none does, or the directory cannot be read.
    std::optional<std::string> latest_commander(const std::filesystem::path &directory);

    // A commander with a history store.
    struct commander_info {
        std::string key; // their FID, or "cmdr:<name>" for journals from before FIDs
        std::string name; // the latest name they played under
        std::filesystem::path store;
    };

    // Owns the single ingest thread and every feed.
    //
    // All feeds are multiplexed onto one thread. File notifications for every directory are handled
    // there, and each feed's historical scan advances one journal file at a time, taking turns with
    // the others, so a long backfill for one commander does not hold up live updates for another.
    // Each open journal is also re-read on a short timer, in case the platform's change notification
    // is late or missing.
    class journal_service {
    public:
        // An empty probe means the game is never considered running.
        explicit journal_service(std::optional<history_config> history = std::nullopt,
                                 game_probe probe = process_game_probe());

        ~journal_service();

        journal_service(const journal_service &) = delete;

        journal_service &operator=(const journal_service &) = delete;

        // Starts watching a directory and returns its feed. Re-opening an id returns the same feed.
        // Two ids may share a directory; each gets its own state. The id does not name any store.
        // Throws std::filesystem::filesystem_error if the directory cannot be watched.
        std::shared_ptr<commander_feed> open(const std::string &id, const std::filesystem::path &directory);

        // Stops and discards a feed. Safe for an id that was never opened.
        void close(const std::string &id);

        // Every commander with a history, and any one of their histories by key, whichever directory
        // they played from. Safe from any thread. Disabled if there is no such store.
        [[nodiscard]] std::vector<commander_info> commanders() const;

        [[nodiscard]] journal::history history(const std::string &key) const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace journal
