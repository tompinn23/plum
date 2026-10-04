#include "CommanderDashboard.h"

#include <array>

#include <QLabel>

#include <journal/projections.hpp>
#include <spdlog/spdlog.h>

#include "Format.h"

namespace {

// Rank names by journal rank key; index is the rank number. Ranks past the end are Elite I-V.
struct RankScale {
    const char *key;
    const char *label;
    std::array<const char *, 15> names;
};

constexpr std::array<RankScale, 8> rankScales{{
    {"Combat", "Combat",
     {"Harmless", "Mostly Harmless", "Novice", "Competent", "Expert", "Master", "Dangerous", "Deadly", "Elite"}},
    {"Trade", "Trade",
     {"Penniless", "Mostly Penniless", "Peddler", "Dealer", "Merchant", "Broker", "Entrepreneur", "Tycoon", "Elite"}},
    {"Explore", "Exploration",
     {"Aimless", "Mostly Aimless", "Scout", "Surveyor", "Trailblazer", "Pathfinder", "Ranger", "Pioneer", "Elite"}},
    {"Exobiologist", "Exobiology",
     {"Directionless", "Mostly Directionless", "Compiler", "Collector", "Cataloguer", "Taxonomist", "Ecologist",
      "Geneticist", "Elite"}},
    {"Soldier", "Mercenary",
     {"Defenceless", "Mostly Defenceless", "Rookie", "Soldier", "Gunslinger", "Warrior", "Gladiator", "Deadeye",
      "Elite"}},
    {"CQC", "CQC",
     {"Helpless", "Mostly Helpless", "Amateur", "Semi Professional", "Professional", "Champion", "Hero", "Legend",
      "Elite"}},
    {"Federation", "Federation",
     {"None", "Recruit", "Cadet", "Midshipman", "Petty Officer", "Chief Petty Officer", "Warrant Officer", "Ensign",
      "Lieutenant", "Lt. Commander", "Post Commander", "Post Captain", "Rear Admiral", "Vice Admiral", "Admiral"}},
    {"Empire", "Empire",
     {"None", "Outsider", "Serf", "Master", "Squire", "Knight", "Lord", "Baron", "Viscount", "Count", "Earl",
      "Marquis", "Duke", "Prince", "King"}},
}};

QString rankName(const RankScale &scale, int rank) {
    if (rank >= 0 && rank < static_cast<int>(scale.names.size()) && scale.names[rank]) return scale.names[rank];
    if (rank > 8 && QString(scale.key) != "Federation" && QString(scale.key) != "Empire")
        return QStringLiteral("Elite %1").arg(rank - 8);
    return QString::number(rank);
}

}  // namespace

CommanderDashboard::CommanderDashboard(QWidget *parent) : Dashboard("Commander", parent) {
    m_cmdr = addPanel("CMDR", 0);
    m_balance = m_cmdr->addField("Balance", "-", true);
    m_system = m_cmdr->addField("System");
    m_station = m_cmdr->addField("Station");
    m_ship = m_cmdr->addField("Ship", "-", true);
    m_cmdr->addSection("Status");
    m_mode = m_cmdr->addField("Game mode");
    m_activity = m_cmdr->addField("Activity");

    auto *ranks = addPanel("RANKS", 0);
    for (const auto &scale : rankScales) m_ranks.insert(scale.key, ranks->addField(scale.label));

    auto *location = addPanel("LOCATION", 1);
    m_locSystem = location->addField("System", "-", true);
    m_body = location->addField("Body");
    m_locStation = location->addField("Station");
    m_population = location->addField("Population");

    auto *reputation = addPanel("REPUTATION", 1);
    for (const char *power : {"Federation", "Empire", "Alliance", "Independent"})
        m_reputation.insert(power, reputation->addField(power));

    auto *history = addPanel("HISTORY", 1);
    m_journals = history->addField("Journals");
    m_events = history->addField("Events");
    m_visited = history->addField("Systems visited");
    m_missions = history->addField("Missions done");
    m_earned = history->addField("Mission pay", "-", true);
    m_net = history->addField("Net credits", "-", true);
}

void CommanderDashboard::showState(const journal::game_state &s) {
    m_cmdr->setTitle(s.name ? QStringLiteral("CMDR %1").arg(QString::fromStdString(*s.name)) : QStringLiteral("CMDR"));
    m_balance->setText(fmt_ui::credits(s.credits));
    m_system->setText(fmt_ui::text(s.system_name));
    m_station->setText(fmt_ui::text(s.station_name));
    m_ship->setText(fmt_ui::text(s.ship_localised ? s.ship_localised : s.ship_type));
    m_mode->setText(s.mode.empty() ? QStringLiteral("-") : QString::fromStdString(s.mode));
    m_activity->setText(s.on_foot ? "On foot" : s.is_docked ? "Docked" : s.taxi ? "In taxi" : "Flying");

    for (const auto &scale : rankScales) {
        const auto it = s.rank.find(scale.key);
        m_ranks.value(scale.key)->setText(
            it == s.rank.end() ? QStringLiteral("-")
                               : QStringLiteral("%1  %2%").arg(rankName(scale, it->second.rank)).arg(it->second.progress));
    }

    m_locSystem->setText(fmt_ui::text(s.system_name));
    m_body->setText(fmt_ui::text(s.body));
    m_locStation->setText(fmt_ui::text(s.station_name));
    m_population->setText(fmt_ui::number(s.system_population));

    for (auto it = m_reputation.begin(); it != m_reputation.end(); ++it) {
        const auto rep = s.reputation.find(it.key().toStdString());
        it.value()->setText(rep == s.reputation.end() ? QStringLiteral("-")
                                                      : QString::asprintf("%+.1f", rep->second));
    }
}

void CommanderDashboard::showHistory(const journal::history &h) {
    if (!h.enabled()) return;
    try {
        using missions = journal::mission_log_projection;
        m_journals->setText(fmt_ui::number(h.file_count()));
        m_events->setText(fmt_ui::number(h.event_count()));
        m_visited->setText(fmt_ui::number(journal::visited_system_projection::count(h)));
        m_missions->setText(fmt_ui::number(missions::count(h, missions::status::completed)));
        m_earned->setText(fmt_ui::credits(missions::earned(h)));
        m_net->setText(fmt_ui::credits(journal::ledger_projection::net(h)));
    } catch (const std::exception &e) {
        // Tables appear once the store has been opened on the ingest thread.
        spdlog::debug("history not ready: {}", e.what());
    }
}
