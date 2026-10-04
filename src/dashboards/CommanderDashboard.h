#pragma once

#include <QHash>

#include "Dashboard.h"

class QLabel;

class CommanderDashboard : public Dashboard {
    Q_OBJECT

public:
    explicit CommanderDashboard(QWidget *parent = nullptr);

    void showState(const journal::game_state &state) override;
    void showHistory(const journal::history &history) override;

private:
    Panel *m_cmdr;
    QLabel *m_balance, *m_system, *m_station, *m_ship, *m_mode, *m_activity;
    QLabel *m_locSystem, *m_body, *m_locStation, *m_population;
    QHash<QString, QLabel *> m_ranks;       // keyed by journal rank name
    QHash<QString, QLabel *> m_reputation;  // keyed by superpower
    QLabel *m_journals, *m_events, *m_visited, *m_missions, *m_earned, *m_net;
};
