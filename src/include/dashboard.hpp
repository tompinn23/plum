#pragma once
#include <QFrame>
#include <QLocale>
#include <QWidget>

#include "journal/journal_service.hpp"
#include "notifications.hpp"


class QLabel;
class QProgressBar;
class QTableWidget;
class QSplitter;
class QVBoxLayout;
class dashboard;


namespace journal {
    class history;
    class game_event;
    struct game_state;
}

namespace format {
    inline QString text(const std::optional<std::string> &value) {
        return value && !value->empty() ? QString::fromStdString(*value) : QStringLiteral("-");
    }

    inline QString number(std::int64_t value) {
        return QLocale(QLocale::English).toString(value);
    }

    inline QString number(const std::optional<std::int64_t> &value) {
        return value ? number(*value) : QStringLiteral("-");
    }

    inline QString credits(std::int64_t value) {
        return number(value) + QStringLiteral(" cr");
    }

    inline QString credits(const std::optional<std::int64_t> &value) {
        return value ? credits(*value) : QStringLiteral("-");
    }

    inline QString decimal(const std::optional<double> &value, int places, const QString &unit = {}) {
        if (!value) return QStringLiteral("-");
        QString s = QString::number(*value, 'f', places);
        return unit.isEmpty() ? s : s + QLatin1Char(' ') + unit;
    }
}

class panel : public QFrame {
    Q_OBJECT

public:
    explicit panel(const QString &title, const dashboard *owner, QWidget *parent = nullptr);

    void set_title(const QString &title) const;

    // Adds to the notification feed, through the dashboard this panel belongs to.
    void notify(notification n) const;

    [[nodiscard]] QLabel *add_field(const QString &name, const QString &value = QStringLiteral("-"),
                                    bool accent = false) const;

    [[nodiscard]] QProgressBar *add_bar(const QString &name, int value = 0) const;

    QLabel *add_section(const QString &text) const;

    // Takes the panel's spare height, for a list or table.
    void add_widget(QWidget *widget) const;

    // A read-only table filling the panel; the first column stretches, the rest fit their contents.
    [[nodiscard]] QTableWidget *add_table(const QStringList &columns) const;

private:
    QLabel *_title;
    QVBoxLayout *body;
    const dashboard *owner;
};

class dashboard : public QWidget {
    Q_OBJECT

public:
    explicit dashboard(QString title, const std::shared_ptr<journal::commander_feed> &feed, notifications &notes,
                       QWidget *parent = nullptr);

    [[nodiscard]] QString title() const { return _title; }

    // Not a journal event: sent once, to handlers that subscribe to it, when the feed has finished
    // reading history, so a page can paint from state() and history() whether or not the game is
    // running. A handler subscribed after that gets it straight away.
    static constexpr auto ready = "Ready";

    // Adds to the notification feed. Labelled with this feed's commander unless the poster says
    // otherwise.
    void notify(notification n) const;

protected:
    [[nodiscard]] panel *add_panel(const QString &title, int colidx) const;

    using handler_fn = std::function<void(const journal::game_state &, const journal::game_event &)>;

    void subscribe(std::set<std::string> events, handler_fn handler);

    [[nodiscard]] journal::commander_feed &feed() const { return *this->data; }

private:
    void become_ready();

    QString _title;
    QSplitter *columns;
    std::shared_ptr<journal::commander_feed> data;
    std::vector<journal::subscription> subs;
    std::vector<handler_fn> ready_handlers; // until the feed is ready
    bool is_ready = false;
    notifications &notes;
};
