#include "overlays.hpp"

#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QLabel>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <ranges>

#include <journal/journal_service.hpp>
#include <spdlog/spdlog.h>

#include "game_windows.hpp"

namespace {
    constexpr overlay_placement toast_placement{.corner = Qt::TopRightCorner, .margin = {24, 24}, .size = {340, 0}};
    constexpr int max_cards = 4;
    constexpr int fade_ms = 180;
    constexpr qint64 merge_window_seconds = 10;

    const char *kind_name(notification::level kind) {
        switch (kind) {
            case notification::level::good: return "good";
            case notification::level::warning: return "warning";
            case notification::level::info: break;
        }
        return "info";
    }

    // Fades a card's opacity, then runs `done`.
    void fade(QWidget *card, double from, double to, std::function<void()> done = {}) {
        auto *effect = qobject_cast<QGraphicsOpacityEffect *>(card->graphicsEffect());
        if (!effect) return;
        auto *animation = new QPropertyAnimation(effect, "opacity", card);
        animation->setDuration(fade_ms);
        animation->setStartValue(from);
        animation->setEndValue(to);
        if (done) QObject::connect(animation, &QPropertyAnimation::finished, card, std::move(done));
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    bool about(const notification &n, const std::string &feed) { return n.feed.empty() || n.feed == feed; }

    QString merge_key(const notification &n) {
        return n.key.isEmpty() ? QString() : n.source + QLatin1Char('\x1f') + n.key;
    }

    QString text_of(const notification &n) {
        QString title = n.title.toHtmlEscaped();
        if (n.count > 1) title += QStringLiteral("  ×%1").arg(n.count);
        QString html = QStringLiteral("<b>%1</b>").arg(title);
        if (!n.source.isEmpty())
            html += QStringLiteral(" <span style='color:#8a8a8a'>%1</span>").arg(n.source.toHtmlEscaped());
        // Line breaks in the text are kept; rich text would otherwise fold them into spaces.
        if (!n.text.isEmpty())
            html += QStringLiteral("<br>%1").arg(
                n.text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        return html;
    }

    template<typename Card>
    void forget(QHash<QString, Card> &by_key, const QLabel *label) {
        for (auto it = by_key.begin(); it != by_key.end();) {
            it = it->label == label ? by_key.erase(it) : std::next(it);
        }
    }

    // The rect a placement asks for inside `area`, with `height` for a fitted one.
    QRect at_corner(const QRect &area, const overlay_placement &p, const int height) {
        const QSize size(p.size.width(), p.size.height() > 0 ? p.size.height() : height);
        const bool left = p.corner == Qt::TopLeftCorner || p.corner == Qt::BottomLeftCorner;
        const bool top = p.corner == Qt::TopLeftCorner || p.corner == Qt::TopRightCorner;
        const int x = left ? area.left() + p.margin.x() : area.right() + 1 - p.margin.x() - size.width();
        const int y = top ? area.top() + p.margin.y() : area.bottom() + 1 - p.margin.y() - size.height();
        return {QPoint(x, y), size};
    }
} // namespace

overlay_window::overlay_window(overlays &owner, std::string feed, const overlay_placement &placement,
                               const bool interactive)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus |
                       (interactive ? Qt::WindowFlags() : Qt::WindowTransparentForInput)),
      owner(owner), feed(std::move(feed)), where(placement), layout_(new QVBoxLayout(this)) {
    setObjectName("overlay");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    layout_->setContentsMargins(0, 0, 0, 0);
    game_windows::mark_overlay(this);
}

void overlay_window::set_active(const bool active) {
    if (this->active == active) return;
    this->active = active;
    owner.place(this);
}

void overlay_window::set_placement(const overlay_placement &placement) {
    where = placement;
    owner.place(this);
}

void overlay_window::refit() { owner.place(this); }

overlays::overlays(game_windows &windows, QObject *parent) : QObject(parent), windows(windows) {
    connect(&windows, &game_windows::changed, this, qOverload<const std::string &>(&overlays::place));
}

overlays::~overlays() {
    for (const auto &f: feeds | std::views::values)
        for (const auto *w: f.windows) delete w;
}

void overlays::add_feed(const std::shared_ptr<journal::commander_feed> &feed) {
    const std::string id = feed->id();
    if (feeds.contains(id)) return;
    feeds[id].feed = feed;
    spdlog::debug("[{}] overlays added", id);

    auto *toasts = spawn(id, toast_placement);
    toasts->setObjectName("toasts");
    toasts->body()->setSpacing(6);
    toasts->body()->addStretch(); // cards stay packed at the top
    toasts->set_active(false);
    feeds[id].toasts.window = toasts;
}

void overlays::remove_feed(const std::string &id) {
    const auto it = feeds.find(id);
    if (it == feeds.end()) return;
    spdlog::debug("[{}] removing {} overlays", id, it->second.windows.size());
    for (const auto *w: it->second.windows) delete w;
    feeds.erase(it);
}

overlay_window *overlays::spawn(const std::string &feed_id, const overlay_placement &placement, const bool interactive) {
    const auto it = feeds.find(feed_id);
    if (it == feeds.end()) return nullptr;
    spdlog::debug("[{}] overlay spawned{}", feed_id, interactive ? ", interactive" : "");
    auto *window = new overlay_window(*this, feed_id, placement, interactive);
    it->second.windows.push_back(window);
    place(window);
    return window;
}

void overlays::close(overlay_window *window) {
    if (!window) return;
    const auto it = feeds.find(window->feed_id());
    if (it == feeds.end()) return;
    if (std::erase(it->second.windows, window)) delete window;
}

void overlays::place(const std::string &id) {
    const auto it = feeds.find(id);
    if (it == feeds.end()) return;
    for (auto *w: it->second.windows) place(w);
}

void overlays::place(overlay_window *window) {
    // Only changes are logged: this runs every time anything moves over the game.
    const auto hide = [window](const char *why) {
        if (window->isVisible())
            spdlog::debug("[{}] overlay {} hidden: {}", window->feed_id(), window->objectName().toStdString(), why);
        window->hide();
    };

    const auto it = feeds.find(window->feed_id());
    const auto feed = it != feeds.end() ? it->second.feed.lock() : nullptr;
    if (!window->is_active()) return hide("inactive");
    if (!feed || !feed->game_running()) return hide("game not running");

    // The game's client area if its window is known; otherwise the primary screen, where the game
    // usually is, so overlays still work where windows cannot be tracked.
    QRect area;
    const auto game = windows.find(window->feed_id());
    if (game) {
        if (game->minimised) return hide("game minimised");
        area = game->geometry;
    } else if (const auto *screen = QGuiApplication::primaryScreen()) {
        area = screen->availableGeometry();
    } else {
        return hide("no screen");
    }

    const int width = window->placement().size.width();
    const int fitted = window->body()->hasHeightForWidth()
                           ? window->body()->totalHeightForWidth(width)
                           : window->body()->totalSizeHint().height();
    const QRect rect = at_corner(area, window->placement(), fitted);
    if (!window->isVisible() || window->geometry() != rect)
        spdlog::debug("[{}] overlay {} at {}x{}+{}+{}{}", window->feed_id(), window->objectName().toStdString(),
                      rect.width(), rect.height(), rect.x(), rect.y(), game ? "" : " (game window unknown)");
    window->setGeometry(rect);
    window->show();
    game_windows::stack_over(window, game ? game->handle : 0);
}

void overlays::post(notification n) {
    n.at = QDateTime::currentDateTimeUtc();

    // A burst of the same thing (a bounty spree, say) becomes one card with a count.
    if (!n.key.isEmpty() && last && last->key == n.key && last->source == n.source &&
        last->at.secsTo(n.at) < merge_window_seconds) {
        ++last->count;
        last->at = n.at;
        last->text = n.text;
        spdlog::debug("notification [{}] {} x{}", last->source.toStdString(), last->title.toStdString(), last->count);
        bump(*last);
        return;
    }

    spdlog::info("notification [{}] {}: {}", n.source.toStdString(), n.title.toStdString(), n.text.toStdString());
    last = std::move(n);
    toast(*last);
}

void overlays::toast(const notification &n) {
    for (auto &[id, f]: feeds) {
        if (!about(n, id)) continue;
        // Over the game only while it runs; otherwise the notification just collects in the hub.
        if (const auto feed = f.feed.lock(); !feed || !feed->game_running()) {
            spdlog::trace("[{}] toast {} not shown: game not running", id, n.title.toStdString());
            continue;
        }
        add_card(f, n);
    }
}

void overlays::bump(const notification &n) {
    for (auto &[id, f]: feeds) {
        if (!about(n, id)) continue;
        const auto it = f.toasts.by_key.constFind(merge_key(n));
        if (it == f.toasts.by_key.cend()) {
            // Its card has already gone; show it afresh.
            if (const auto feed = f.feed.lock(); feed && feed->game_running()) add_card(f, n);
            continue;
        }
        it->label->setText(text_of(n));
        it->timer->start(n.lifetime);
        f.toasts.window->refit();
    }
}

void overlays::add_card(feed_overlays &f, const notification &n) {
    auto &stack = f.toasts;
    auto *cards = stack.window->body();
    spdlog::debug("[{}] toast {}", stack.window->feed_id(), n.title.toStdString());

    // Too many at once: the oldest goes now rather than pushing the stack off screen.
    if (cards->count() - 1 >= max_cards) {
        if (auto *oldest = qobject_cast<QLabel *>(cards->itemAt(cards->count() - 2)->widget())) dismiss(stack, oldest);
    }

    auto *label = new QLabel(text_of(n));
    label->setObjectName("toast");
    label->setProperty("kind", kind_name(n.kind));
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setGraphicsEffect(new QGraphicsOpacityEffect(label));
    cards->insertWidget(0, label); // newest on top

    // The stack outlives its cards: they go with its window, before the feed's entry does.
    auto *timer = new QTimer(label);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, label, [&stack, label] {
        forget(stack.by_key, label); // a repeat from here on gets a card of its own
        fade(label, 1.0, 0.0, [&stack, label] { dismiss(stack, label); });
    });
    timer->start(n.lifetime);

    if (const auto key = merge_key(n); !key.isEmpty()) stack.by_key.insert(key, {label, timer});

    stack.window->set_active(true);
    stack.window->refit();
    fade(label, 0.0, 1.0);
}

void overlays::dismiss(toast_stack &stack, QLabel *label) {
    auto *cards = stack.window->body();
    spdlog::trace("[{}] toast dismissed; {} left", stack.window->feed_id(), cards->count() - 2);
    forget(stack.by_key, label);
    cards->removeWidget(label);
    label->deleteLater();
    if (cards->count() <= 1) stack.window->set_active(false); // only the stretch is left
    else stack.window->refit();
}
