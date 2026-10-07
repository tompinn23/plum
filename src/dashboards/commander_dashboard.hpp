#pragma once

#include <QHash>

#include "../include/dashboard.hpp"

class QLabel;
class QTableWidget;

class commander_dashboard : public dashboard {
    Q_OBJECT

public:
    commander_dashboard(const std::shared_ptr<journal::commander_feed> &feed, notifications &notes,
                        QWidget *parent = nullptr);

private:
    panel *cmdr;
    QLabel *balance, *system, *station, *ship, *mode, *activity;
    QLabel *loc_system, *body, *loc_station, *population;
    QHash<QString, QLabel *> ranks; // keyed by journal rank name
    QHash<QString, QLabel *> reputation; // keyed by superpower
    QLabel *journals, *events, *visited, *missions, *earned, *net;
    QLabel *cargo; // the CMDR panel's cargo heading, with the hold's totals
    QTableWidget *manifest;
};
