#include "event_log_dashboard.hpp"

#include <QDateTime>
#include <QHeaderView>
#include <QTableWidget>

namespace {
    constexpr int max_lines = 500;

    const QColor info(0xd0, 0xd0, 0xd0);
    const QColor good(0x44, 0xcc, 0x88);
    const QColor warn(0xf0, 0xb0, 0x40);
    const QColor danger(0xe0, 0x60, 0x50);
    const QColor capi(0x70, 0xa8, 0xe0); // Companion API responses, not the journal

    struct line {
        QString text;
        QColor colour;
    };

    // The _Localised form is what the game shows; the plain one may be a symbol like "$Gold_Name;".
    QString shown(const journal::game_event &e, const std::string &key) {
        auto value = e.opt_str(key + "_Localised");
        if (!value || value->empty()) value = e.opt_str(key);
        if (!value || value->empty()) return QStringLiteral("?");
        return QString::fromStdString(*value);
    }

    QString text(const journal::game_event &e, const std::string_view key) {
        return QString::fromStdString(e.str(key, "?"));
    }

    QString gain(const std::int64_t credits) {
        return QStringLiteral("+") + format::credits(credits);
    }

    // One line for an event worth reading, or nothing.
    std::optional<line> describe(const journal::game_event &e) {
        const std::string &name = e.name();

        // Travel
        if (name == "FSDJump") {
            QString about = QStringLiteral("Jumped to %1").arg(text(e, "StarSystem"));
            if (const auto distance = e.opt_double("JumpDist")) about += "  " + format::decimal(distance, 2, "ly");
            if (const auto security = e.opt_str("SystemSecurity_Localised"))
                about += "  " + QString::fromStdString(*security);
            return line{about, info};
        }
        if (name == "Docked")
            return line{QStringLiteral("Docked at %1, %2").arg(text(e, "StationName"), text(e, "StarSystem")), info};
        if (name == "Undocked") return line{QStringLiteral("Undocked from %1").arg(text(e, "StationName")), info};
        if (name == "Touchdown" && e.flag("PlayerControlled"))
            return line{QStringLiteral("Landed on %1").arg(text(e, "Body")), info};
        if (name == "ApproachSettlement") return line{QStringLiteral("Approaching %1").arg(shown(e, "Name")), info};

        // Trade
        if (name == "MarketSell") {
            const auto count = e.num("Count");
            const auto profit = e.num("TotalSale") - e.num("AvgPricePaid") * count;
            return line{
                QStringLiteral("Sold %1 t %2  %3  (profit %4)")
                .arg(format::number(count), shown(e, "Type"), gain(e.num("TotalSale")), format::credits(profit)),
                profit >= 0 ? good : warn
            };
        }
        if (name == "MarketBuy")
            return line{
                QStringLiteral("Bought %1 t %2  -%3")
                .arg(format::number(e.num("Count")), shown(e, "Type"), format::credits(e.num("TotalCost"))),
                info
            };
        if (name == "MiningRefined") return line{QStringLiteral("Refined 1 t %1").arg(shown(e, "Type")), good};

        // Missions
        if (name == "MissionAccepted")
            return line{
                QStringLiteral("Mission accepted: %1").arg(shown(e, "LocalisedName")), info
            };
        if (name == "MissionCompleted") {
            QString about = QStringLiteral("Mission complete: %1").arg(shown(e, "LocalisedName"));
            if (const auto reward = e.opt_long("Reward"); reward && *reward > 0) about += "  " + gain(*reward);
            return line{about, good};
        }
        if (name == "MissionFailed")
            return line{
                QStringLiteral("Mission failed: %1").arg(shown(e, "LocalisedName")), danger
            };
        if (name == "MissionAbandoned")
            return line{
                QStringLiteral("Mission abandoned: %1").arg(shown(e, "LocalisedName")), warn
            };

        // Combat
        if (name == "Bounty")
            return line{QStringLiteral("Bounty on %1  %2").arg(shown(e, "Target"), gain(e.num("TotalReward"))), good};
        if (name == "FactionKillBond")
            return line{
                QStringLiteral("Combat bond from %1  %2").arg(text(e, "AwardingFaction"), gain(e.num("Reward"))), good
            };
        if (name == "Interdicted") {
            const bool npc = !e.flag("IsPlayer");
            return line{
                QStringLiteral("Interdicted by %1%2").arg(shown(e, "Interdictor"), npc ? "" : " (player)"), danger
            };
        }
        if (name == "EscapeInterdiction") return line{QStringLiteral("Escaped %1").arg(shown(e, "Interdictor")), good};
        if (name == "UnderAttack" && e.str("Target") == "You") return line{QStringLiteral("Under attack"), danger};
        if (name == "ShieldState")
            return e.flag("ShieldsUp")
                       ? line{QStringLiteral("Shields back up"), good}
                       : line{QStringLiteral("Shields down"), danger};
        if (name == "HullDamage" && e.flag("PlayerPilot"))
            return line{QStringLiteral("Hull at %1%").arg(qRound(e.dec("Health") * 100)), danger};
        if (name == "Died") {
            const auto killer = e.opt_str("KillerName_Localised")
                                    ? e.opt_str("KillerName_Localised")
                                    : e.opt_str("KillerName");
            return line{
                killer
                    ? QStringLiteral("Destroyed by %1").arg(QString::fromStdString(*killer))
                    : QStringLiteral("Destroyed"),
                danger
            };
        }
        if (name == "Scanned") return line{QStringLiteral("Being scanned: %1").arg(text(e, "ScanType")), warn};

        // Income
        if (name == "RedeemVoucher")
            return line{
                QStringLiteral("Redeemed %1 voucher  %2").arg(text(e, "Type"), gain(e.num("Amount"))), good
            };
        if (name == "SellExplorationData" || name == "MultiSellExplorationData")
            return line{QStringLiteral("Sold exploration data  %1").arg(gain(e.num("TotalEarnings"))), good};
        if (name == "SellOrganicData") {
            std::int64_t total = 0;
            for (const auto &row: e.rows("BioData"))
                total += journal::field::num(row, "Value") + journal::field::num(row, "Bonus");
            return line{QStringLiteral("Sold organic data  %1").arg(gain(total)), good};
        }

        // Exploration
        if (name == "FSSDiscoveryScan")
            return line{QStringLiteral("Discovery scan: %1 bodies").arg(format::number(e.num("BodyCount"))), info};
        if (name == "FSSAllBodiesFound")
            return line{
                QStringLiteral("All bodies found in %1").arg(text(e, "SystemName")), good
            };
        if (name == "SAAScanComplete") return line{QStringLiteral("Mapped %1").arg(text(e, "BodyName")), good};
        if (name == "CodexEntry" && e.flag("IsNewEntry"))
            return line{QStringLiteral("New codex entry: %1").arg(shown(e, "Name")), good};
        if (name == "ScanOrganic" && e.str("ScanType") == "Analyse")
            return line{QStringLiteral("Analysed %1").arg(shown(e, "Species")), good};

        // Standing and ships
        if (name == "Promotion") {
            QStringList ranks;
            for (const auto &[key, _]: e.payload().items())
                if (key != "event" && key != "timestamp") ranks << QString::fromStdString(key);
            return line{QStringLiteral("Promoted: %1").arg(ranks.join(", ")), good};
        }
        if (name == "ShipyardSwap") return line{QStringLiteral("Switched to %1").arg(shown(e, "ShipType")), info};
        if (name == "ShipyardBuy")
            return line{
                QStringLiteral("Bought a %1  -%2").arg(shown(e, "ShipType"), format::credits(e.num("ShipPrice"))), info
            };
        if (name == "EngineerCraft")
            return line{
                QStringLiteral("%1: %2 grade %3").arg(text(e, "Engineer"), shown(e, "BlueprintName"),
                                                      QString::number(e.num("Level"))),
                good
            };

        // Fleet carrier
        if (name == "CarrierJumpRequest") {
            const auto when = QDateTime::fromString(QString::fromStdString(e.str("DepartureTime")), Qt::ISODate);
            return line{
                QStringLiteral("Carrier jump to %1 at %2")
                .arg(text(e, "SystemName"), when.isValid() ? when.toLocalTime().toString("HH:mm") : "?"),
                warn
            };
        }
        if (name == "CarrierJumpCancelled") return line{QStringLiteral("Carrier jump cancelled"), warn};
        if (name == "CarrierJump")
            return line{
                QStringLiteral("Carrier arrived at %1").arg(text(e, "StarSystem")), info
            };

        // Companion API
        if (name == "CAPIMarket")
            return line{
                QStringLiteral("CAPI market: %1, %2 commodities")
                .arg(text(e, "name"), format::number(static_cast<std::int64_t>(e.rows("commodities").size()))),
                capi
            };
        if (name == "CAPIShipyard") {
            const auto ships = journal::field::object(e.object("ships"), "shipyard_list").size();
            const auto modules = e.object("modules").size();
            return line{
                QStringLiteral("CAPI shipyard: %1, %2 ships, %3 modules")
                .arg(text(e, "name"), QString::number(ships), QString::number(modules)),
                capi
            };
        }
        if (name == "CAPIFleetcarrier") {
            const auto callsign = journal::field::str(e.object("name"), "callsign", "?");
            return line{
                QStringLiteral("CAPI carrier: %1 in %2, %3 cargo stacks")
                .arg(QString::fromStdString(callsign), text(e, "currentStarSystem"),
                     QString::number(e.rows("cargo").size())),
                capi
            };
        }

        return std::nullopt;
    }
} // namespace

event_log_dashboard::event_log_dashboard(const std::shared_ptr<journal::commander_feed> &feed, notifications &notes,
                                         QWidget *parent)
    : ::dashboard("Event log", feed, notes, parent), log(new QTableWidget(0, 2)) {
    log->setHorizontalHeaderLabels({"Time", "Event"});
    log->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    log->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    log->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    log->verticalHeader()->hide();
    log->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    log->verticalHeader()->setDefaultSectionSize(22);
    log->setShowGrid(false);
    log->setFocusPolicy(Qt::NoFocus);
    log->setEditTriggers(QAbstractItemView::NoEditTriggers);
    log->setSelectionMode(QAbstractItemView::NoSelection);
    add_panel("EVENTS", 0)->add_widget(log);

    // Everything, live only; describe() picks what is worth a line.
    subscribe({}, [this](const journal::game_state &, const journal::game_event &e) {
        if (const auto entry = describe(e)) add_line(e, entry->text, entry->colour);
    });
}

void event_log_dashboard::add_line(const journal::game_event &e, const QString &text, const QColor &colour) const {
    const auto when = e.time()
                          ? QDateTime::fromSecsSinceEpoch(e.time()->time_since_epoch().count()).toLocalTime()
                          : QDateTime::currentDateTime();

    log->insertRow(0);
    auto *time = new QTableWidgetItem(when.toString("HH:mm:ss"));
    time->setForeground(QColor(0x6a, 0x6a, 0x6a));
    auto *event = new QTableWidgetItem(text);
    event->setForeground(colour);
    event->setToolTip(QString::fromStdString(e.name()));
    log->setItem(0, 0, time);
    log->setItem(0, 1, event);

    if (log->rowCount() > max_lines) log->removeRow(log->rowCount() - 1);
}
