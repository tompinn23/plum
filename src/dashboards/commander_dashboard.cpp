#include "commander_dashboard.hpp"

#include <array>

#include <QLabel>
#include <QTableWidget>

#include <journal/projections.hpp>
#include <spdlog/spdlog.h>

#include "commodities.hpp"
#include "journal/journal_service.hpp"


namespace {
    // Rank names by journal rank key; index is the rank number. Ranks past the end are Elite I-V.
    struct rank_scale {
        const char *key;
        const char *label;
        std::array<const char *, 15> names;
    };

    constexpr std::array<rank_scale, 8> rank_scales{
        {
            {
                .key = "Combat", .label = "Combat",
                .names = {
                    "Harmless", "Mostly Harmless", "Novice", "Competent", "Expert", "Master", "Dangerous", "Deadly",
                    "Elite"
                }
            },
            {
                .key = "Trade", .label = "Trade",
                .names = {
                    "Penniless", "Mostly Penniless", "Peddler", "Dealer", "Merchant", "Broker", "Entrepreneur",
                    "Tycoon", "Elite"
                }
            },
            {
                .key = "Explore", .label = "Exploration",
                .names = {
                    "Aimless", "Mostly Aimless", "Scout", "Surveyor", "Trailblazer", "Pathfinder", "Ranger", "Pioneer",
                    "Elite"
                }
            },
            {
                .key = "Exobiologist", .label = "Exobiology",
                .names = {
                    "Directionless", "Mostly Directionless", "Compiler", "Collector", "Cataloguer", "Taxonomist",
                    "Ecologist",
                    "Geneticist", "Elite"
                }
            },
            {
                .key = "Soldier", .label = "Mercenary",
                .names = {
                    "Defenceless", "Mostly Defenceless", "Rookie", "Soldier", "Gunslinger", "Warrior", "Gladiator",
                    "Deadeye",
                    "Elite"
                }
            },
            {
                .key = "CQC", .label = "CQC",
                .names = {
                    "Helpless", "Mostly Helpless", "Amateur", "Semi Professional", "Professional", "Champion", "Hero",
                    "Legend",
                    "Elite"
                }
            },
            {
                .key = "Federation", .label = "Federation",
                .names = {
                    "None", "Recruit", "Cadet", "Midshipman", "Petty Officer", "Chief Petty Officer", "Warrant Officer",
                    "Ensign",
                    "Lieutenant", "Lt. Commander", "Post Commander", "Post Captain", "Rear Admiral", "Vice Admiral",
                    "Admiral"
                }
            },
            {
                .key = "Empire", .label = "Empire",
                .names = {
                    "None", "Outsider", "Serf", "Master", "Squire", "Knight", "Lord", "Baron", "Viscount", "Count",
                    "Earl",
                    "Marquis", "Duke", "Prince", "King"
                }
            },
        }
    };

    QString rank_name(const rank_scale &scale, int rank) {
        if (rank >= 0 && rank < static_cast<int>(scale.names.size()) && scale.names[rank]) return scale.names[rank];
        if (rank > 8 && QString(scale.key) != "Federation" && QString(scale.key) != "Empire")
            return QStringLiteral("Elite %1").arg(rank - 8);
        return QString::number(rank);
    }
} // namespace

commander_dashboard::commander_dashboard(const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                                         QWidget *parent)
    : ::dashboard("Commander", feed, notes, parent) {
    cmdr = add_panel("CMDR", 0);
    balance = cmdr->add_field("Balance", "-", true);
    system = cmdr->add_field("System");
    station = cmdr->add_field("Station");
    ship = cmdr->add_field("Ship", "-", true);
    cmdr->add_section("Status");
    mode = cmdr->add_field("Game mode");
    activity = cmdr->add_field("Activity");
    cargo = cmdr->add_section("Cargo");
    manifest = cmdr->add_table({"Commodity", "Category", "Qty"});

    auto *ranks_panel = add_panel("RANKS", 0);
    for (const auto &scale: rank_scales) ranks.insert(scale.key, ranks_panel->add_field(scale.label));

    auto *location_panel = add_panel("LOCATION", 1);
    loc_system = location_panel->add_field("System", "-", true);
    body = location_panel->add_field("Body");
    loc_station = location_panel->add_field("Station");
    population = location_panel->add_field("Population");

    auto *rep_panel = add_panel("REPUTATION", 1);
    for (const char *power: {"Federation", "Empire", "Alliance", "Independent"})
        reputation.insert(power, rep_panel->add_field(power));

    auto *history_panel = add_panel("HISTORY", 1);
    journals = history_panel->add_field("Journals");
    events = history_panel->add_field("Events");
    visited = history_panel->add_field("Systems visited");
    missions = history_panel->add_field("Missions done");
    earned = history_panel->add_field("Mission pay", "-", true);
    net = history_panel->add_field("Net credits", "-", true);

    // Every subscription takes StartUp too, which a page created mid-session is sent first, and
    // Ready, for the first paint once history is read.

    subscribe({ready, "StartUp", "LoadGame", "Commander"},
              [this](const journal::game_state &s, const journal::game_event &) {
                  cmdr->set_title(s.name
                                      ? QStringLiteral("CMDR %1").arg(QString::fromStdString(*s.name))
                                      : QStringLiteral("CMDR"));
                  mode->setText(s.mode.empty() ? QStringLiteral("-") : QString::fromStdString(s.mode));
              });

    subscribe({
                  ready, "StartUp", "LoadGame", "MarketBuy", "MarketSell", "BuyDrones", "SellDrones", "RedeemVoucher",
                  "PayBounties", "PayFines", "PayLegacyFines", "SellExplorationData", "MultiSellExplorationData",
                  "SellOrganicData", "CommunityGoalReward", "PowerplaySalary", "BuyTradeData", "BuyExplorationData",
                  "MissionCompleted", "ShipyardBuy", "ShipyardSell", "ShipyardTransfer", "SellShipOnRebuy", "ModuleBuy",
                  "ModuleSell", "ModuleSellRemote", "FetchRemoteModule", "Repair", "RepairAll", "RefuelAll",
                  "RefuelPartial", "RestockVehicle", "BuyAmmo", "CarrierBankTransfer", "BuyMicroResources",
                  "SellMicroResources", "BuySuit", "BuyWeapon", "Resurrect"
              },
              [this](const journal::game_state &s, const journal::game_event &) {
                  balance->setText(format::credits(s.credits));
              });

    subscribe({ready, "StartUp", "LoadGame", "Loadout", "ShipyardSwap", "ShipyardNew", "SetUserShipName"},
              [this](const journal::game_state &s, const journal::game_event &) {
                  ship->setText(format::text(s.ship_localised ? s.ship_localised : s.ship_type));
              });

    subscribe({
                  ready, "StartUp", "Location", "FSDJump", "CarrierJump", "Docked", "Undocked", "ApproachBody",
                  "LeaveBody",
                  "SupercruiseEntry", "SupercruiseExit"
              },
              [this](const journal::game_state &s, const journal::game_event &) {
                  system->setText(format::text(s.system_name));
                  station->setText(format::text(s.station_name));
                  loc_system->setText(format::text(s.system_name));
                  body->setText(format::text(s.body));
                  loc_station->setText(format::text(s.station_name));
                  population->setText(format::number(s.system_population));
              });

    subscribe({
                  ready, "StartUp", "LoadGame", "Location", "Shutdown", "Docked", "Undocked", "Touchdown", "Liftoff",
                  "Embark",
                  "Disembark", "BookTaxi", "CancelTaxi", "BookDropship", "CancelDropship", "DropshipDeploy"
              },
              [this](const journal::game_state &s, const journal::game_event &e) {
                  // The state keeps the last session as it was; only the event, or the game not
                  // running once history is read, says it is over.
                  const bool offline = e.name() == "Shutdown" || (e.name() == ready && !this->feed()->game_running());
                  activity->setText(offline
                                        ? "Offline"
                                        : s.on_foot
                                              ? "On foot"
                                              : s.is_docked
                                                    ? "Docked"
                                                    : s.taxi
                                                          ? "In taxi"
                                                          : "Flying");
              });

    // Everything the parser counts the hold by, and Loadout for its size.
    subscribe({
                  ready, "StartUp", "Loadout", "Cargo", "CargoTransfer", "CollectCargo", "MiningRefined", "MarketBuy",
                  "BuyDrones", "EjectCargo", "MarketSell", "SellDrones", "SearchAndRescue", "MissionCompleted",
                  "EngineerContribution", "TechnologyBroker"
              },
              [this](const journal::game_state &s, const journal::game_event &) {
                  struct row {
                      commodity info;
                      int count;
                  };
                  std::vector<row> rows;
                  int total = 0;
                  for (const auto &[symbol, count]: s.cargo) {
                      if (count <= 0) continue;
                      rows.push_back({find_commodity(QString::fromStdString(symbol)), count});
                      total += count;
                  }
                  std::ranges::sort(rows, {}, [](const row &r) { return r.info.name; });

                  manifest->setRowCount(static_cast<int>(rows.size()));
                  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                      auto *category = new QTableWidgetItem(rows[i].info.category);
                      category->setForeground(QColor(0x8a, 0x8a, 0x8a));
                      auto *count = new QTableWidgetItem(format::number(rows[i].count));
                      count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                      manifest->setItem(i, 0, new QTableWidgetItem(rows[i].info.name));
                      manifest->setItem(i, 1, category);
                      manifest->setItem(i, 2, count);
                  }
                  cargo->setText(s.cargo_capacity
                                     ? QStringLiteral("CARGO  %1 / %2 t")
                                     .arg(format::number(total), format::number(*s.cargo_capacity))
                                     : QStringLiteral("CARGO  %1 t").arg(format::number(total)));
              });

    subscribe({ready, "StartUp", "Rank", "Progress", "Promotion"},
              [this](const journal::game_state &s, const journal::game_event &) {
                  for (const auto &scale: rank_scales) {
                      const auto it = s.rank.find(scale.key);
                      ranks.value(scale.key)->setText(
                          it == s.rank.end()
                              ? QStringLiteral("-")
                              : QStringLiteral("%1  %2%").arg(rank_name(scale, it->second.rank)).arg(
                                  it->second.progress));
                  }
              });

    subscribe({ready, "StartUp", "Reputation"}, [this](const journal::game_state &s, const journal::game_event &) {
        for (auto it = reputation.begin(); it != reputation.end(); ++it) {
            const auto rep = s.reputation.find(it.key().toStdString());
            it.value()->setText(rep == s.reputation.end()
                                    ? QStringLiteral("-")
                                    : QString::asprintf("%+.1f", rep->second));
        }
    });

    // The feed's history follows whoever is playing, so it is asked for afresh each time.
    subscribe({
                  ready, "StartUp", "LoadGame", "Commander", "Location", "FSDJump", "CarrierJump", "MarketBuy",
                  "MarketSell",
                  "RedeemVoucher", "SellExplorationData", "MultiSellExplorationData", "SellOrganicData",
                  "MissionCompleted", "MissionFailed", "MissionAbandoned", "CarrierBankTransfer"
              },
              [this](const journal::game_state &, const journal::game_event &) {
                  const auto h = this->feed()->history();
                  if (!h.enabled()) return;
                  try {
                      using missions_log = journal::mission_log_projection;
                      journals->setText(format::number(h.file_count()));
                      events->setText(format::number(h.event_count()));
                      visited->setText(format::number(journal::visited_system_projection::count(h)));
                      missions->setText(format::number(missions_log::count(h, missions_log::status::completed)));
                      earned->setText(format::credits(missions_log::earned(h)));
                      net->setText(format::credits(journal::ledger_projection::net(h)));
                  } catch (const std::exception &e) {
                      // Tables appear once the store has been opened on the ingest thread.
                      spdlog::debug("history not ready: {}", e.what());
                  }
              });

    // Where you have just arrived: who runs it, what it does, how safe it is, how many live there.
    subscribe({"FSDJump"}, [this](const journal::game_state &, const journal::game_event &e) {
        // The _Localised forms are what the game shows; the plain ones are symbols like "$economy_Industrial;".
        const auto pick = [&e](std::string_view key) {
            auto value = e.opt_str(std::string(key) + "_Localised");
            if (!value || value->empty()) value = e.opt_str(key);
            return value && !value->empty() && !value->starts_with('$') ? QString::fromStdString(*value) : QString();
        };

        QStringList about;
        for (const auto &part: {pick("SystemAllegiance"), pick("SystemEconomy"), pick("SystemSecurity")})
            if (!part.isEmpty()) about << part;

        QStringList numbers;
        if (const auto population = e.opt_long("Population"); population && *population > 0)
            numbers << QStringLiteral("Pop %1").arg(format::number(*population));
        if (const auto distance = e.opt_double("JumpDist")) numbers << format::decimal(distance, 2, "ly");

        QStringList lines;
        if (!about.isEmpty()) lines << about.join(QStringLiteral(" · "));
        if (!numbers.isEmpty()) lines << numbers.join(QStringLiteral(" · "));
        notify({.title = QString::fromStdString(e.str("StarSystem")), .text = lines.join('\n')});
    });
}
