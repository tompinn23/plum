#pragma once

#include "Dashboard.h"

class QLabel;

class ShipDashboard : public Dashboard {
    Q_OBJECT

public:
    explicit ShipDashboard(QWidget *parent = nullptr);

    void showState(const journal::game_state &state) override;

private:
    QLabel *m_type, *m_name, *m_ident, *m_jump, *m_mass, *m_modules;
    Panel *m_cargo;
    QLabel *m_used, *m_limpets, *m_kinds;
    QLabel *m_hull, *m_moduleValue, *m_rebuy;
    QLabel *m_fuelMain, *m_fuelReserve;
};
