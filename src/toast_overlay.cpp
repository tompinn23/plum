#include "include/toast_overlay.hpp"

#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QLabel>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

namespace {
    constexpr int column_width = 340;
    constexpr int edge_margin = 24;
    constexpr int max_cards = 4;
    constexpr int fade_ms = 180;

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
} // namespace

toast_overlay::toast_overlay(notifications &notes, std::function<bool()> showing)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                       Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
      m_showing(std::move(showing)), m_cards(new QVBoxLayout(this)) {
    setObjectName("toasts");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    m_cards->setContentsMargins(0, 0, 0, 0);
    m_cards->setSpacing(6);
    m_cards->addStretch(); // cards stay packed at the top

    connect(&notes, &notifications::posted, this, &toast_overlay::add);
    connect(&notes, &notifications::merged, this, &toast_overlay::bump);
}

QString toast_overlay::merge_key(const notification &n) {
    return n.key.isEmpty() ? QString() : n.source + QLatin1Char('\x1f') + n.key;
}

QString toast_overlay::text_of(const notification &n) {
    QString title = n.title.toHtmlEscaped();
    if (n.count > 1) title += QStringLiteral("  ×%1").arg(n.count);
    QString html = QStringLiteral("<b>%1</b>").arg(title);
    if (!n.source.isEmpty())
        html += QStringLiteral(" <span style='color:#8a8a8a'>%1</span>").arg(
            n.source.toHtmlEscaped());
    // Line breaks in the text are kept; rich text would otherwise fold them into spaces.
    if (!n.text.isEmpty())
        html += QStringLiteral("<br>%1").arg(n.text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    return html;
}

void toast_overlay::add(const notification &n) {
    if (m_showing && !m_showing()) return;

    // Too many at once: the oldest goes now rather than pushing the stack off screen.
    if (m_cards->count() - 1 >= max_cards) {
        if (auto *oldest = qobject_cast<QLabel *>(m_cards->itemAt(m_cards->count() - 2)->widget())) dismiss(oldest);
    }

    auto *label = new QLabel(text_of(n));
    label->setObjectName("toast");
    label->setProperty("kind", kind_name(n.kind));
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setGraphicsEffect(new QGraphicsOpacityEffect(label));
    m_cards->insertWidget(0, label); // newest on top

    auto *timer = new QTimer(label);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, label, [this, label] {
        forget(label); // a repeat from here on gets a card of its own
        fade(label, 1.0, 0.0, [this, label] { dismiss(label); });
    });
    timer->start(n.lifetime);

    if (const auto key = merge_key(n); !key.isEmpty()) m_by_key.insert(key, {label, timer});

    place();
    show();
    fade(label, 0.0, 1.0);
}

// A repeat: the card already showing says so, and stays a while longer.
void toast_overlay::bump(const notification &n) {
    const auto it = m_by_key.constFind(merge_key(n));
    if (it == m_by_key.cend()) {
        add(n); // its card has already gone; show it afresh
        return;
    }
    it->label->setText(text_of(n));
    it->timer->start(n.lifetime);
}

void toast_overlay::forget(const QLabel *label) {
    for (auto it = m_by_key.begin(); it != m_by_key.end();) {
        it = it->label == label ? m_by_key.erase(it) : std::next(it);
    }
}

void toast_overlay::dismiss(QLabel *label) {
    forget(label);
    m_cards->removeWidget(label);
    label->deleteLater();
    if (m_cards->count() <= 1) hide(); // only the stretch is left
}

// The top-right corner of the primary screen, where the game usually is.
void toast_overlay::place() {
    const auto *screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    setGeometry(area.right() - column_width - edge_margin, area.top() + edge_margin, column_width, area.height() / 2);
}
