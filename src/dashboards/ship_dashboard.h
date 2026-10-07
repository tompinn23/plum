#pragma once

#include "dashboard.h"

class QLabel;

class ship_dashboard : public dashboard {
    Q_OBJECT

public:
    explicit ship_dashboard(QWidget *parent = nullptr);

    void show_state(const journal::game_state &state) override;

private:
    QLabel *m_type, *m_name, *m_ident, *m_jump, *m_mass, *m_modules;
    panel *m_cargo;
    QLabel *m_used, *m_limpets, *m_kinds;
    QLabel *m_hull, *m_module_value, *m_rebuy;
    QLabel *m_fuel_main, *m_fuel_reserve;
};
