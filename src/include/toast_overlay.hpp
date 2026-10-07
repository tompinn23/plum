#pragma once

#include <functional>

#include <QHash>
#include <QWidget>

#include "notifications.hpp"

class QLabel;
class QTimer;
class QVBoxLayout;

// Notifications as cards over the game: a frameless, always-on-top window that mouse clicks pass
// straight through, so it can sit over Elite (in borderless windowed mode) without getting in the
// way. Newest card on top; each fades out after its lifetime, and the window hides when empty.
class toast_overlay : public QWidget {
    Q_OBJECT

public:
    // `showing` says whether cards should appear at all, e.g. only while the game is running.
    // Notifications posted while it is false are left to the feed's other listeners.
    toast_overlay(notifications &notes, std::function<bool()> showing);

private:
    struct card {
        QLabel *label = nullptr;
        QTimer *timer = nullptr;
    };

    void add(const notification &n);

    void bump(const notification &n);

    void forget(const QLabel *label);

    void dismiss(QLabel *label);

    void place();

    static QString text_of(const notification &n);

    static QString merge_key(const notification &n);

    std::function<bool()> m_showing;
    QVBoxLayout *m_cards;
    QHash<QString, card> m_by_key; // live cards that later posts may merge into
};
