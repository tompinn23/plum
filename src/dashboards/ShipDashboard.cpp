#include "ShipDashboard.h"

#include <numeric>

#include <QLabel>

#include "Format.h"

ShipDashboard::ShipDashboard(QWidget *parent) : Dashboard("Ship", parent) {
    auto *ship = addPanel("VESSEL", 0);
    m_type = ship->addField("Type", "-", true);
    m_name = ship->addField("Name");
    m_ident = ship->addField("Ident");
    m_jump = ship->addField("Jump range");
    m_mass = ship->addField("Unladen mass");
    m_modules = ship->addField("Modules");

    m_cargo = addPanel("CARGO", 0);
    m_used = m_cargo->addField("Used");
    m_limpets = m_cargo->addField("Limpets");
    m_kinds = m_cargo->addField("Commodities");

    auto *value = addPanel("VALUE", 1);
    m_hull = value->addField("Hull");
    m_moduleValue = value->addField("Modules");
    m_rebuy = value->addField("Rebuy", "-", true);

    auto *fuel = addPanel("FUEL", 1);
    m_fuelMain = fuel->addField("Main tank");
    m_fuelReserve = fuel->addField("Reserve");
}

void ShipDashboard::showState(const journal::game_state &s) {
    m_type->setText(fmt_ui::text(s.ship_localised ? s.ship_localised : s.ship_type));
    m_name->setText(fmt_ui::text(s.ship_name));
    m_ident->setText(fmt_ui::text(s.ship_ident));
    m_jump->setText(fmt_ui::decimal(s.max_jump_range, 2, "ly"));
    m_mass->setText(fmt_ui::decimal(s.unladen_mass, 1, "t"));
    m_modules->setText(s.modules.empty() ? QStringLiteral("-") : QString::number(s.modules.size()));

    const int used = std::accumulate(s.cargo.begin(), s.cargo.end(), 0,
                                     [](int sum, const auto &item) { return sum + item.second; });
    m_cargo->setTitle(s.cargo_capacity ? QStringLiteral("CARGO  %1 / %2 t").arg(used).arg(*s.cargo_capacity)
                                       : QStringLiteral("CARGO"));
    m_used->setText(QStringLiteral("%1 t").arg(used));
    const auto drones = s.cargo.find("drones");
    m_limpets->setText(QString::number(drones == s.cargo.end() ? 0 : drones->second));
    m_kinds->setText(QString::number(s.cargo.size()));

    m_hull->setText(fmt_ui::credits(s.hull_value));
    m_moduleValue->setText(fmt_ui::credits(s.modules_value));
    m_rebuy->setText(fmt_ui::credits(s.rebuy));

    m_fuelMain->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->main, 0, "t") : QStringLiteral("-"));
    m_fuelReserve->setText(s.fuel_capacity ? fmt_ui::decimal(s.fuel_capacity->reserve, 2, "t") : QStringLiteral("-"));
}
