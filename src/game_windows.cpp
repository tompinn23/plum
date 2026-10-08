#include "game_windows.hpp"

#include <QGuiApplication>
#include <QWidget>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <cwchar>
#include <format>
#include <future>
#include <set>
#include <utility>

#include <journal/journal_service.hpp>
#include <spdlog/spdlog.h>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dwmapi.h>

#include <unordered_map>
#include <unordered_set>
#endif

namespace {
    // How often feeds are asked for their game's pids. The probe behind them only looks every few
    // seconds, so this need not be quick.
    constexpr int pid_interval_ms = 1000;
} // namespace

#ifdef Q_OS_WIN

namespace {
    constexpr UINT add_pid_message = WM_APP + 1;
    constexpr UINT remove_pid_message = WM_APP + 2;

    // Elite's main window. Anything else a client opens (a crash reporter, say) is a fallback.
    constexpr wchar_t game_class[] = L"FrontierDevelopmentsAppWinClass";

    HWND as_hwnd(const quintptr handle) { return reinterpret_cast<HWND>(handle); }

    DWORD pid_of(HWND window) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        return pid;
    }

    // What is drawn: GetWindowRect also counts the invisible resize borders around the frame.
    RECT bounds(HWND window) {
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
            GetWindowRect(window, &r);
        return r;
    }

    constexpr wchar_t overlay_property[] = L"plum.overlay";

    // The client area, in physical screen pixels.
    RECT client_rect(HWND window) {
        RECT r{};
        GetClientRect(window, &r);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&r), 2);
        return r;
    }

    // A client's top-level window: its main window if it has one, else its largest.
    HWND find_window(const DWORD pid) {
        struct search {
            DWORD pid;
            HWND best = nullptr;
            LONG best_area = -1;
            bool best_is_game = false;
        } s{pid};

        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            auto &s = *reinterpret_cast<search *>(param);
            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || pid_of(window) != s.pid) return TRUE;

            wchar_t name[64]{};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            const bool is_game = std::wcscmp(name, game_class) == 0;
            const RECT r = bounds(window);
            const LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (is_game > s.best_is_game || (is_game == s.best_is_game && area > s.best_area)) {
                s.best = window;
                s.best_area = area;
                s.best_is_game = is_game;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&s));
        return s.best;
    }

    // Win32 works in physical pixels; Qt in pixels scaled per screen, each screen keeping its
    // physical top-left.
    const QScreen *screen_at(const QPoint native) {
        for (const auto *screen: QGuiApplication::screens()) {
            const QRect g = screen->geometry();
            if (QRect(g.topLeft(), g.size() * screen->devicePixelRatio()).contains(native)) return screen;
        }
        return QGuiApplication::primaryScreen();
    }

    std::string describe(const QRect &r) {
        return std::format("{}x{} at {},{}", r.width(), r.height(), r.x(), r.y());
    }

    QRect to_qt(const QRect &native) {
        const auto *screen = screen_at(native.topLeft());
        if (!screen) return native;
        const qreal ratio = screen->devicePixelRatio();
        const QPoint corner = screen->geometry().topLeft();
        return {corner + (native.topLeft() - corner) / ratio, native.size() / ratio};
    }
} // namespace

// Everything here runs on the pump thread: the hooks are out of context, so Windows delivers them
// through the message loop of the thread that set them.
class game_windows_pump {
public:
    struct game {
        HWND window = nullptr;
        std::optional<game_windows::native_window> last; // as last posted
    };

    struct state {
        game_windows *owner;
        std::unordered_map<DWORD, game> games;
        // Windows being dragged or resized by the user: settled once, when they are let go.
        std::unordered_set<HWND> moving;
    };

    static inline thread_local state *current = nullptr;

    // Hands the UI thread news, waking it once however much piles up before it looks.
    template<class F>
    static void post(F &&write) {
        auto *owner = current->owner;
        {
            std::lock_guard lock(owner->outbox_mutex);
            write(owner->pending);
        }
        if (!owner->drain_queued.exchange(true))
            QMetaObject::invokeMethod(owner, [owner] { owner->drain(); }, Qt::QueuedConnection);
    }

    static std::optional<game_windows::native_window> snapshot(const DWORD pid, HWND window) {
        if (!window) return std::nullopt;
        const RECT r = client_rect(window);
        wchar_t title[256]{};
        const int length = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        return game_windows::native_window{
            .pid = pid,
            .handle = reinterpret_cast<quintptr>(window),
            .title = QString::fromWCharArray(title, length),
            .client = QRect(r.left, r.top, r.right - r.left, r.bottom - r.top),
            .minimised = IsIconic(window) != FALSE,
            .foreground = GetForegroundWindow() == window,
        };
    }

    // Finds a client's window again if the one it had is gone, and posts it if anything changed.
    static void update(const DWORD pid, game &g) {
        const bool kept = g.window && IsWindow(g.window) && IsWindowVisible(g.window) && pid_of(g.window) == pid;
        if (!kept) g.window = find_window(pid);
        auto now = snapshot(pid, g.window);
        const bool same = now && g.last && now->handle == g.last->handle && now->client == g.last->client &&
                          now->minimised == g.last->minimised && now->foreground == g.last->foreground &&
                          now->title == g.last->title;
        if (same || (!now && !g.last)) return;

        if (!now) {
            spdlog::info("game pid {}: window gone", pid);
        } else if (!g.last || g.last->handle != now->handle) {
            spdlog::info("game pid {}: window {:#x} \"{}\", {}", pid, now->handle, now->title.toStdString(),
                         describe(now->client));
        } else {
            spdlog::debug("game pid {}: {}{}{}", pid, describe(now->client), now->minimised ? ", minimised" : "",
                          now->foreground ? ", foreground" : "");
        }
        g.last = now;
        post([&](game_windows::outbox &o) { o.windows[pid] = std::move(now); });
    }

    static void CALLBACK on_event(HWINEVENTHOOK, const DWORD event, HWND window, const LONG object, const LONG child,
                                  DWORD, DWORD) {
        // Only top-level windows themselves: not carets, cursors or controls inside them.
        if (!window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        if (IsWindow(window) && GetAncestor(window, GA_ROOT) != window) return;

        // A drag moves the window on every frame; only where it ends up matters.
        switch (event) {
            case EVENT_SYSTEM_MOVESIZESTART:
                current->moving.insert(window);
                return;
            case EVENT_SYSTEM_MOVESIZEEND:
            case EVENT_OBJECT_DESTROY:
                current->moving.erase(window);
                break;
            case EVENT_OBJECT_LOCATIONCHANGE:
                if (current->moving.contains(window)) return;
                break;
            default:
                break;
        }

        auto &games = current->games;
        if (games.empty()) return;

        // Focus moving anywhere can take it from, or give it to, any game window.
        if (event == EVENT_SYSTEM_FOREGROUND) {
            for (auto &[pid, g]: games) update(pid, g);
        } else {
            // By window as well as by pid: a destroyed window no longer says whose it was.
            const DWORD pid = pid_of(window);
            for (auto &[game_pid, g]: games)
                if (game_pid == pid || g.window == window) update(game_pid, g);
        }
    }

    static void run(game_windows *owner, std::promise<DWORD> started) {
        state s{.owner = owner};
        current = &s;
        // Posted messages are lost until the thread has a queue; asking for one makes it.
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        started.set_value(GetCurrentThreadId());

        // Narrow ranges: every event in them is marshalled to this thread.
        constexpr std::pair<DWORD, DWORD> ranges[] = {
            {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
            {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
            {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
            {EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER}, // destroy, show, hide, reorder
            {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
            {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
        };
        std::vector<HWINEVENTHOOK> hooks;
        for (const auto [first, last]: ranges) {
            if (const auto hook = SetWinEventHook(first, last, nullptr, on_event, 0, 0, WINEVENT_OUTOFCONTEXT))
                hooks.push_back(hook);
            else
                spdlog::warn("cannot hook window events {:#x}-{:#x}: error {}", first, last, GetLastError());
        }
        spdlog::debug("window tracking started with {} of {} hooks", hooks.size(), std::size(ranges));

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            const auto pid = static_cast<DWORD>(msg.wParam);
            switch (msg.message) {
                case add_pid_message:
                    spdlog::debug("tracking windows of game pid {}", pid);
                    // Reported straight away, rather than waiting for the window to do something.
                    update(pid, s.games[pid]);
                    if (!s.games[pid].window) spdlog::info("game pid {}: no window yet", pid);
                    break;
                case remove_pid_message:
                    spdlog::debug("no longer tracking windows of game pid {}", pid);
                    if (s.games.erase(pid)) post([pid](game_windows::outbox &o) { o.windows[pid] = std::nullopt; });
                    break;
                default:
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
            }
        }
        for (auto *hook: hooks) UnhookWinEvent(hook);
        current = nullptr;
        spdlog::debug("window tracking stopped");
    }
};

game_windows::game_windows(QObject *parent) : QObject(parent), pid_timer(new QTimer(this)) {
    std::promise<DWORD> started;
    auto id = started.get_future();
    pump = std::thread(game_windows_pump::run, this, std::move(started));
    pump_thread = id.get();

    pid_timer->setInterval(pid_interval_ms);
    connect(pid_timer, &QTimer::timeout, this, &game_windows::check_pids);
    pid_timer->start();
}

// A drain still queued for this object is dropped with it, so stopping the pump is enough.
game_windows::~game_windows() {
    PostThreadMessageW(pump_thread, WM_QUIT, 0, 0);
    pump.join();
}

std::optional<game_windows::window> game_windows::find(const std::string &id) const {
    const auto it = feeds.find(id);
    if (it == feeds.end()) return std::nullopt;
    for (const auto pid: it->second.pids) {
        const auto w = windows.find(pid);
        if (w == windows.end()) continue;
        const auto &n = w->second;
        return window{
            .pid = n.pid,
            .handle = n.handle,
            .title = n.title,
            .geometry = to_qt(n.client),
            .minimised = n.minimised,
            .foreground = n.foreground,
        };
    }
    return std::nullopt;
}

// A window property rather than a list here, so it can be read straight from Windows.
void game_windows::mark_overlay(QWidget *window) {
    SetPropW(reinterpret_cast<HWND>(window->winId()), overlay_property, reinterpret_cast<HANDLE>(1));
}

// Not owned by the game, which would keep it above the game for free: Windows destroys owned
// windows with their owner, and ties the two processes' input together.
void game_windows::stack_over(QWidget *overlay, const quintptr game) {
    const HWND self = reinterpret_cast<HWND>(overlay->winId());
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!game) {
        SetWindowPos(self, HWND_TOPMOST, 0, 0, 0, 0, flags);
        return;
    }
    if (GetWindowLongPtrW(self, GWL_EXSTYLE) & WS_EX_TOPMOST) SetWindowPos(self, HWND_NOTOPMOST, 0, 0, 0, 0, flags);

    // Already there if only overlays are between it and the game; otherwise under the first other
    // window above the game, or at the top if there is none, or only always-on-top ones.
    HWND above = GetWindow(as_hwnd(game), GW_HWNDPREV);
    for (; above && GetPropW(above, overlay_property); above = GetWindow(above, GW_HWNDPREV))
        if (above == self) return;
    const bool topmost = above && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SetWindowPos(self, above && !topmost ? above : HWND_TOP, 0, 0, 0, 0, flags);
}

namespace {
    void post_to_pump(const std::uint32_t thread, const UINT message, const std::uint32_t pid) {
        if (!PostThreadMessageW(thread, message, pid, 0))
            spdlog::warn("cannot hand game pid {} to window tracking: error {}", pid, GetLastError());
    }
} // namespace

#else

// No window tracking here yet: nothing is ever found.
game_windows::game_windows(QObject *parent) : QObject(parent), pid_timer(new QTimer(this)) {
}

game_windows::~game_windows() = default;

std::optional<game_windows::window> game_windows::find(const std::string &) const { return std::nullopt; }

void game_windows::mark_overlay(QWidget *) {
}

void game_windows::stack_over(QWidget *, quintptr) {
}

namespace {
    void post_to_pump(std::uint32_t, unsigned, std::uint32_t) {
    }

    constexpr unsigned add_pid_message = 0;
    constexpr unsigned remove_pid_message = 0;
} // namespace

#endif

void game_windows::track(const std::string &id, const std::shared_ptr<journal::commander_feed> &feed) {
    feeds[id].feed = feed;
    check_pids();
}

void game_windows::untrack(const std::string &id) {
    const auto it = feeds.find(id);
    if (it == feeds.end()) return;
    it->second.feed.reset();
    check_pids(); // lets go of its pids
    feeds.erase(id);
    emit changed(id);
}

// Hands the pump the pids that appeared since last time, and takes back the ones no feed has now.
// A feed whose game stopped hears it here: by the time the pump confirms, the pid is no longer its.
void game_windows::check_pids() {
    std::set<std::uint32_t> before, after;
    std::vector<std::string> lost;
    for (auto &[id, t]: feeds) {
        before.insert(t.pids.begin(), t.pids.end());
        const auto feed = t.feed.lock();
        auto pids = feed ? feed->game_pids() : std::vector<std::uint32_t>{};
        const bool had_window = std::ranges::any_of(t.pids, [&](auto pid) { return windows.contains(pid); });
        if (pids != t.pids) {
            spdlog::info("[{}] game pids {}", id, journal::pid_list(pids));
            if (had_window) lost.push_back(id);
        }
        t.pids = std::move(pids);
        after.insert(t.pids.begin(), t.pids.end());
    }
    for (const auto pid: after)
        if (!before.contains(pid)) post_to_pump(pump_thread, add_pid_message, pid);
    for (const auto pid: before) {
        if (after.contains(pid)) continue;
        post_to_pump(pump_thread, remove_pid_message, pid);
        windows.erase(pid);
    }
    for (const auto &id: lost) emit changed(id);
}

void game_windows::drain() {
    outbox news;
    {
        std::lock_guard lock(outbox_mutex);
        news = std::exchange(pending, {});
        drain_queued = false;
    }

    std::set<std::uint32_t> touched;
    for (auto &[pid, w]: news.windows) {
        // A report already on its way when the pid was taken back is stale.
        const bool wanted = std::ranges::any_of(feeds, [&](const auto &f) {
            return std::ranges::find(f.second.pids, pid) != f.second.pids.end();
        });
        if (w && wanted) windows[pid] = std::move(*w);
        else windows.erase(pid);
        touched.insert(pid);
    }

    for (const auto &[id, t]: feeds) {
        if (std::ranges::any_of(t.pids, [&](auto pid) { return touched.contains(pid); })) emit changed(id);
    }
}
