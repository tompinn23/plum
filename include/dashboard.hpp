#pragma once
#include <QFrame>
#include <QLocale>
#include <QWidget>

#include "feed_subscriber.hpp"
#include "journal/journal_service.hpp"
#include "overlays.hpp"


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

class dashboard : public QWidget, public feed_subscriber {
    Q_OBJECT

public:
    explicit dashboard(QString title, const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                       QWidget *parent = nullptr);

    [[nodiscard]] QString title() const { return _title; }

    // Adds to the notification feed. Labelled with this feed's commander unless the poster says
    // otherwise.
    void notify(notification n) const;

protected:
    [[nodiscard]] panel *add_panel(const QString &title, int colidx) const;

private:
    QString _title;
    QSplitter *columns;
    overlays &notes; // where notifications go
};
