#pragma once

#include "dashboard.hpp"

class QLabel;
class QProgressBar;
class QTableWidget;

// Massacre stacks: the open missions and how many kills each still needs, and past sessions with
// what they paid. Everything comes from the massacre projection.
class massacre_dashboard : public dashboard {
    Q_OBJECT

public:
    massacre_dashboard(const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                       QWidget *parent = nullptr);

private:
    void paint_history() const;

    QLabel *target, *system, *missions, *kills_left, *value;
    QProgressBar *progress;
    QTableWidget *stack, *sessions;
};
