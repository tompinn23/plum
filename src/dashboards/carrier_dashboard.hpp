#pragma once

#include "dashboard.hpp"

class QLabel;
class QTableWidget;

// The commander's fleet carrier: what the journals and history know, filled out by the Companion
// API's /fleetcarrier while the account is linked.
class carrier_dashboard : public dashboard {
    Q_OBJECT

public:
    carrier_dashboard(const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                      QWidget *parent = nullptr);

private:
    void paint_history() const;

    void paint_cargo(const journal::game_event &e) const;

    panel *carrier;
    QLabel *name, *callsign, *system, *body, *docking, *fuel, *balance;
    QLabel *destination, *departs;
    QLabel *free_space, *crew, *cargo_for_sale, *cargo_not_for_sale, *reserved_space, *ship_packs, *module_packs;
    QLabel *reserved_balance, *upkeep, *jumps, *distance;
    QLabel *cargo; // the CAPACITY panel's cargo heading, with the total
    QTableWidget *manifest;
};
