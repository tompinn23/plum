#include "carrier_dashboard.hpp"

#include <QDateTime>
#include <QLabel>
#include <QTableWidget>

#include <map>

#include <journal/projections.hpp>
#include <spdlog/spdlog.h>

#include "commodities.hpp"

namespace {
    // The Companion API sends some numbers as strings.
    std::optional<std::int64_t> capi_number(const journal::json &row, const std::string_view key) {
        if (const auto value = journal::field::opt_long(row, key)) return value;
        if (const auto value = journal::field::opt_double(row, key)) return static_cast<std::int64_t>(*value);
        if (const auto text = journal::field::opt_str(row, key)) {
            bool ok = false;
            if (const auto value = QString::fromStdString(*text).toLongLong(&ok); ok) return value;
        }
        return std::nullopt;
    }

    QString tonnes(const std::optional<std::int64_t> &value) {
        return value ? format::number(*value) + QStringLiteral(" t") : QStringLiteral("-");
    }

    // The Companion API hex-encodes the name the commander gave the carrier.
    QString vanity_name(const journal::json &names) {
        const auto hex = journal::field::opt_str(names, "vanityName");
        return hex ? QString::fromUtf8(QByteArray::fromHex(QByteArray::fromStdString(*hex))).trimmed() : QString();
    }
} // namespace

carrier_dashboard::carrier_dashboard(const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                                     QWidget *parent)
    : ::dashboard("Carrier", feed, notes, parent) {
    carrier = add_panel("CARRIER", 0);
    name = carrier->add_field("Name", "-", true);
    callsign = carrier->add_field("Callsign");
    system = carrier->add_field("System", "-", true);
    body = carrier->add_field("Body");
    docking = carrier->add_field("Docking");
    fuel = carrier->add_field("Tritium");
    balance = carrier->add_field("Balance", "-", true);

    auto *jump_panel = add_panel("JUMP", 0);
    destination = jump_panel->add_field("Destination", "-", true);
    departs = jump_panel->add_field("Departs");

    auto *capacity_panel = add_panel("CAPACITY", 1);
    free_space = capacity_panel->add_field("Free space", "-", true);
    crew = capacity_panel->add_field("Crew");
    cargo_for_sale = capacity_panel->add_field("Cargo for sale");
    cargo_not_for_sale = capacity_panel->add_field("Cargo not for sale");
    reserved_space = capacity_panel->add_field("Reserved");
    ship_packs = capacity_panel->add_field("Ship packs");
    module_packs = capacity_panel->add_field("Module packs");
    cargo = capacity_panel->add_section("Cargo");
    manifest = capacity_panel->add_table({"Commodity", "Category", "Qty", "Value"});

    auto *finance_panel = add_panel("FINANCE", 1);
    reserved_balance = finance_panel->add_field("Reserved");
    upkeep = finance_panel->add_field("Weekly upkeep");
    jumps = finance_panel->add_field("Jumps");
    distance = finance_panel->add_field("Distance jumped");

    // The carrier lives in history rather than state: the journals only mention it now and then.
    subscribe({
                  ready, "StartUp", "CarrierBuy", "CarrierStats", "CarrierJumpRequest", "CarrierJumpCancelled",
                  "CarrierLocation"
              },
              [this](const journal::game_state &, const journal::game_event &) { paint_history(); });

    subscribe({"CarrierJumpRequest"}, [this](const journal::game_state &, const journal::game_event &e) {
        const auto target = QString::fromStdString(e.str("SystemName"));
        const auto at = QString::fromStdString(e.str("Body"));
        destination->setText(at.isEmpty() || at == target ? target : at);
        const auto when = QDateTime::fromString(QString::fromStdString(e.str("DepartureTime")), Qt::ISODate);
        departs->setText(when.isValid() ? when.toLocalTime().toString("ddd HH:mm") : QStringLiteral("-"));
    });

    subscribe({"CarrierJumpCancelled", "CarrierLocation"},
              [this](const journal::game_state &, const journal::game_event &) {
                  destination->setText(QStringLiteral("-"));
                  departs->setText(QStringLiteral("-"));
              });

    subscribe({"CAPIFleetcarrier"}, [this](const journal::game_state &, const journal::game_event &e) {
        const auto &names = e.object("name");
        if (const auto vanity = vanity_name(names); !vanity.isEmpty()) name->setText(vanity);
        if (const auto sign = journal::field::opt_str(names, "callsign")) {
            callsign->setText(QString::fromStdString(*sign));
            carrier->set_title(QStringLiteral("CARRIER %1").arg(QString::fromStdString(*sign)));
        }
        if (const auto where = e.opt_str("currentStarSystem")) system->setText(QString::fromStdString(*where));
        if (const auto access = e.opt_str("dockingAccess")) docking->setText(QString::fromStdString(*access));
        if (const auto tritium = capi_number(e.payload(), "fuel")) fuel->setText(tonnes(tritium));
        if (const auto credits = capi_number(e.payload(), "balance")) balance->setText(format::credits(credits));

        const auto &capacity = e.object("capacity");
        free_space->setText(tonnes(capi_number(capacity, "freeSpace")));
        crew->setText(tonnes(capi_number(capacity, "crew")));
        cargo_for_sale->setText(tonnes(capi_number(capacity, "cargoForSale")));
        cargo_not_for_sale->setText(tonnes(capi_number(capacity, "cargoNotForSale")));
        reserved_space->setText(tonnes(capi_number(capacity, "cargoSpaceReserved")));
        ship_packs->setText(tonnes(capi_number(capacity, "shipPacks")));
        module_packs->setText(tonnes(capi_number(capacity, "modulePacks")));

        const auto &finance = e.object("finance");
        reserved_balance->setText(format::credits(capi_number(finance, "bankReservedBalance")));
        upkeep->setText(format::credits(capi_number(finance, "maintenance")));
        jumps->setText(format::number(capi_number(finance, "numJumps")));
        const auto jumped = journal::field::opt_double(e.object("itinerary"), "totalDistanceJumpedLY");
        distance->setText(format::decimal(jumped, 0, "ly"));

        paint_cargo(e);
    });
}

void carrier_dashboard::paint_history() const {
    const auto h = feed()->history();
    if (!h.enabled()) return;

    std::optional<journal::carrier_projection::carrier> info;
    try {
        info = journal::carrier_projection::info(h);
    } catch (const std::exception &e) {
        // Tables appear once the store has been opened on the ingest thread.
        spdlog::debug("history not ready: {}", e.what());
        return;
    }
    if (!info) return;

    carrier->set_title(info->callsign
                           ? QStringLiteral("CARRIER %1").arg(QString::fromStdString(*info->callsign))
                           : QStringLiteral("CARRIER"));
    name->setText(format::text(info->name));
    callsign->setText(format::text(info->callsign));
    system->setText(format::text(info->system));
    body->setText(format::text(info->body));
    docking->setText(format::text(info->docking));
    fuel->setText(tonnes(info->fuel));
    balance->setText(format::credits(info->balance));
}

// The API lists a commodity once per stack: stolen and mission cargo apart from the rest.
void carrier_dashboard::paint_cargo(const journal::game_event &e) const {
    struct stack {
        commodity info;
        QString note;
        std::int64_t count = 0;
        std::int64_t value = 0;
    };
    std::map<QString, stack> stacks;
    std::int64_t total = 0;
    for (const auto &row: e.rows("cargo")) {
        const auto symbol = QString::fromStdString(journal::field::str(row, "commodity"));
        const QString note = journal::field::flag(row, "stolen")
                                 ? QStringLiteral("stolen")
                                 : journal::field::flag(row, "mission")
                                       ? QStringLiteral("mission")
                                       : QString();
        auto &s = stacks[symbol.toLower() + '|' + note];
        if (s.info.name.isEmpty()) {
            s.info = find_commodity(symbol);
            if (const auto shown = journal::field::opt_str(row, "locName"); shown && !shown->empty())
                s.info.name = QString::fromStdString(*shown);
            s.note = note;
        }
        const auto count = capi_number(row, "qty").value_or(0);
        s.count += count;
        s.value += capi_number(row, "value").value_or(0);
        total += count;
    }

    std::vector<stack> rows;
    for (auto &[_, s]: stacks) rows.push_back(std::move(s));
    std::ranges::sort(rows, {}, [](const stack &s) { return s.info.name; });

    manifest->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto &s = rows[i];
        auto *label = new QTableWidgetItem(s.note.isEmpty()
                                               ? s.info.name
                                               : QStringLiteral("%1 (%2)").arg(s.info.name, s.note));
        if (!s.note.isEmpty()) label->setForeground(QColor(0xe0, 0x60, 0x50));
        auto *category = new QTableWidgetItem(s.info.category);
        category->setForeground(QColor(0x8a, 0x8a, 0x8a));
        auto *count = new QTableWidgetItem(format::number(s.count));
        count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *value = new QTableWidgetItem(format::credits(s.value));
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        manifest->setItem(i, 0, label);
        manifest->setItem(i, 1, category);
        manifest->setItem(i, 2, count);
        manifest->setItem(i, 3, value);
    }
    cargo->setText(QStringLiteral("CARGO  %1 t").arg(format::number(total)));
}
