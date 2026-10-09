#include "ship_dashboard.h"

#include <numeric>

#include <QLabel>

#include "format.h"

ship_dashboard::ship_dashboard(QWidget *parent) : dashboard("Ship", parent) {
    auto *ship = add_panel("VESSEL", 0);
    type = ship->add_field("Type", "-", true);
    name = ship->add_field("Name");
    ident = ship->add_field("Ident");
    jump = ship->add_field("Jump range");
    mass = ship->add_field("Unladen mass");
    modules = ship->add_field("Modules");

    cargo = add_panel("CARGO", 0);
    used = cargo->add_field("Used");
    limpets = cargo->add_field("Limpets");
    kinds = cargo->add_field("Commodities");

    auto *value = add_panel("VALUE", 1);
    hull = value->add_field("Hull");
    module_value = value->add_field("Modules");
    rebuy = value->add_field("Rebuy", "-", true);

    auto *fuel = add_panel("FUEL", 1);
    fuel_main = fuel->add_field("Main tank");
    fuel_reserve = fuel->add_field("Reserve");
}

void ship_dashboard::show_state(const journal::game_state &s) {
    type->setText(fmt_ui::text(s.ship_localised ? s.ship_localised : s.ship_type));
    name->setText(fmt_ui::text(s.ship_name));
    ident->setText(fmt_ui::text(s.ship_ident));
    jump->setText(fmt_ui::decimal(s.max_jump_range, 2, "ly"));
    mass->setText(fmt_ui::decimal(s.unladen_mass, 1, "t"));
    modules->setText(s.modules.empty() ? QStringLiteral("-") : QString::number(s.modules.size()));

    const int loaded = std::accumulate(s.cargo.begin(), s.cargo.end(), 0,
                                     [](int sum, const auto &item) { return sum + item.second; });
    cargo->set_title(s.cargo_capacity
                           ? QStringLiteral("CARGO  %1 / %2 t").arg(loaded).arg(*s.cargo_capacity)
                           : QStringLiteral("CARGO"));
    used->setText(QStringLiteral("%1 t").arg(loaded));
    const auto drones = s.cargo.find("drones");
    limpets->setText(QString::number(drones == s.cargo.end() ? 0 : drones->second));
    kinds->setText(QString::number(s.cargo.size()));

    hull->setText(fmt_ui::credits(s.hull_value));
    module_value->setText(fmt_ui::credits(s.modules_value));
    rebuy->setText(fmt_ui::credits(s.rebuy));

    fuel_main->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->main, 0, "t") : QStringLiteral("-"));
    fuel_reserve->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->reserve, 2, "t") : QStringLiteral("-"));
}
