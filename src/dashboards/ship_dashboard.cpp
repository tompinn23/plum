#include "ship_dashboard.h"

#include <numeric>

#include <QLabel>

#include "format.h"

ship_dashboard::ship_dashboard(QWidget *parent) : dashboard("Ship", parent) {
    auto *ship = add_panel("VESSEL", 0);
    m_type = ship->add_field("Type", "-", true);
    m_name = ship->add_field("Name");
    m_ident = ship->add_field("Ident");
    m_jump = ship->add_field("Jump range");
    m_mass = ship->add_field("Unladen mass");
    m_modules = ship->add_field("Modules");

    m_cargo = add_panel("CARGO", 0);
    m_used = m_cargo->add_field("Used");
    m_limpets = m_cargo->add_field("Limpets");
    m_kinds = m_cargo->add_field("Commodities");

    auto *value = add_panel("VALUE", 1);
    m_hull = value->add_field("Hull");
    m_module_value = value->add_field("Modules");
    m_rebuy = value->add_field("Rebuy", "-", true);

    auto *fuel = add_panel("FUEL", 1);
    m_fuel_main = fuel->add_field("Main tank");
    m_fuel_reserve = fuel->add_field("Reserve");
}

void ship_dashboard::show_state(const journal::game_state &s) {
    m_type->setText(fmt_ui::text(s.ship_localised ? s.ship_localised : s.ship_type));
    m_name->setText(fmt_ui::text(s.ship_name));
    m_ident->setText(fmt_ui::text(s.ship_ident));
    m_jump->setText(fmt_ui::decimal(s.max_jump_range, 2, "ly"));
    m_mass->setText(fmt_ui::decimal(s.unladen_mass, 1, "t"));
    m_modules->setText(s.modules.empty() ? QStringLiteral("-") : QString::number(s.modules.size()));

    const int used = std::accumulate(s.cargo.begin(), s.cargo.end(), 0,
                                     [](int sum, const auto &item) { return sum + item.second; });
    m_cargo->set_title(s.cargo_capacity
                           ? QStringLiteral("CARGO  %1 / %2 t").arg(used).arg(*s.cargo_capacity)
                           : QStringLiteral("CARGO"));
    m_used->setText(QStringLiteral("%1 t").arg(used));
    const auto drones = s.cargo.find("drones");
    m_limpets->setText(QString::number(drones == s.cargo.end() ? 0 : drones->second));
    m_kinds->setText(QString::number(s.cargo.size()));

    m_hull->setText(fmt_ui::credits(s.hull_value));
    m_module_value->setText(fmt_ui::credits(s.modules_value));
    m_rebuy->setText(fmt_ui::credits(s.rebuy));

    m_fuel_main->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->main, 0, "t") : QStringLiteral("-"));
    m_fuel_reserve->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->reserve, 2, "t") : QStringLiteral("-"));
}
