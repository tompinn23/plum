#pragma once

#include "../include/dashboard.hpp"

class QTableWidget;

// What has just happened, newest first, one line per event worth reading. Live events only: it
// starts empty each run.
class event_log_dashboard : public dashboard {
    Q_OBJECT

public:
    event_log_dashboard(const std::shared_ptr<journal::commander_feed> &feed, notifications &notes,
                        QWidget *parent = nullptr);

private:
    void add_line(const journal::game_event &e, const QString &text, const QColor &colour) const;

    QTableWidget *log;
};
