#pragma once

#include "dashboard.h"

class QLabel;

class ship_dashboard : public dashboard {
    Q_OBJECT

public:
    explicit ship_dashboard(QWidget *parent = nullptr);

    void show_state(const journal::game_state &state) override;

private:
    QLabel *type, *name, *ident, *jump, *mass, *modules;
    panel *cargo;
    QLabel *used, *limpets, *kinds;
    QLabel *hull, *module_value, *rebuy;
    QLabel *fuel_main, *fuel_reserve;
};
