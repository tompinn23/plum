#include "massacre_dashboard.hpp"

#include <QDateTime>
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>

#include <map>
#include <set>

#include <journal/projections.hpp>
#include <spdlog/spdlog.h>

namespace {
    QString local_time(const std::optional<journal::timestamp> &t, const QString &shape = QStringLiteral("ddd HH:mm")) {
        if (!t) return QStringLiteral("-");
        const auto when = QDateTime::fromString(QString::fromStdString(journal::format_timestamp(*t)), Qt::ISODate);
        return when.isValid() ? when.toLocalTime().toString(shape) : QStringLiteral("-");
    }

    QTableWidgetItem *right_aligned(const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    }
} // namespace

massacre_dashboard::massacre_dashboard(const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                                       QWidget *parent)
    : ::dashboard("Massacres", feed, notes, parent) {
    auto *stack_panel = add_panel("STACK", 0);
    target = stack_panel->add_field("Target", "-", true);
    system = stack_panel->add_field("System");
    missions = stack_panel->add_field("Missions");
    kills_left = stack_panel->add_field("Kills left", "-", true);
    value = stack_panel->add_field("Value", "-", true);
    progress = stack_panel->add_bar("Progress");
    stack = stack_panel->add_table({"Issuer", "Kills", "Reward", "Expires"});

    auto *session_panel = add_panel("SESSIONS", 1);
    sessions = session_panel->add_table({"Target", "Started", "Kills", "Bounties", "Payout"});

    subscribe({
                  ready, "StartUp", "MissionAccepted", "MissionRedirected", "MissionCompleted", "MissionFailed",
                  "MissionAbandoned", "Bounty"
              },
              [this](const journal::game_state &, const journal::game_event &) { paint_history(); });
}

void massacre_dashboard::paint_history() const {
    const auto h = feed()->history();
    if (!h.enabled()) return;

    std::vector<journal::massacre_projection::stacked> open;
    std::vector<journal::massacre_projection::session> past;
    try {
        open = journal::massacre_projection::active(h);
        past = journal::massacre_projection::sessions(h, 50);
    } catch (const std::exception &e) {
        // Tables appear once the store has been opened on the ingest thread.
        spdlog::debug("history not ready: {}", e.what());
        return;
    }

    // Each kill counts once per issuer, so a stack is finished when its busiest issuer is. Stacks
    // against different factions need separate kills, so those add up.
    std::map<std::string, std::map<std::string, std::pair<std::int64_t, std::int64_t>>> by_target; // done, needed
    std::set<std::string> systems;
    std::int64_t reward = 0;
    for (const auto &m: open) {
        auto &[done, needed] = by_target[m.target][m.issuer];
        done += m.kills;
        needed += m.kills_needed;
        reward += m.reward.value_or(0);
        if (m.system) systems.insert(*m.system);
    }
    std::int64_t left = 0, total = 0;
    QStringList targets;
    for (const auto &[faction, issuers]: by_target) {
        std::int64_t most_left = 0, most_needed = 0;
        for (const auto &[_, counts]: issuers) {
            most_left = std::max(most_left, counts.second - counts.first);
            most_needed = std::max(most_needed, counts.second);
        }
        left += most_left;
        total += most_needed;
        targets << QString::fromStdString(faction);
    }
    QStringList system_names;
    for (const auto &s: systems) system_names << QString::fromStdString(s);

    target->setText(targets.isEmpty() ? QStringLiteral("-") : targets.join(", "));
    system->setText(system_names.isEmpty() ? QStringLiteral("-") : system_names.join(", "));
    missions->setText(format::number(static_cast<std::int64_t>(open.size())));
    kills_left->setText(open.empty() ? QStringLiteral("-") : format::number(left));
    value->setText(open.empty() ? QStringLiteral("-") : format::credits(reward));
    progress->setValue(total > 0 ? static_cast<int>((total - left) * 100 / total) : 0);

    stack->setRowCount(static_cast<int>(open.size()));
    for (int i = 0; i < static_cast<int>(open.size()); ++i) {
        const auto &m = open[i];
        auto *issuer = new QTableWidgetItem(QString::fromStdString(m.issuer));
        auto *kills = right_aligned(QStringLiteral("%1 / %2").arg(m.kills).arg(m.kills_needed));
        if (m.kills >= m.kills_needed) kills->setForeground(QColor(0x60, 0xc0, 0x70));
        stack->setItem(i, 0, issuer);
        stack->setItem(i, 1, kills);
        stack->setItem(i, 2, right_aligned(format::credits(m.reward)));
        stack->setItem(i, 3, right_aligned(local_time(m.expires_at)));
    }

    sessions->setRowCount(static_cast<int>(past.size()));
    for (int i = 0; i < static_cast<int>(past.size()); ++i) {
        const auto &s = past[i];
        auto *faction = new QTableWidgetItem(s.system
                                                 ? QStringLiteral("%1 (%2)").arg(QString::fromStdString(s.target),
                                                                                QString::fromStdString(*s.system))
                                                 : QString::fromStdString(s.target));
        auto *payout = right_aligned(s.ended_at ? format::credits(s.payout) : QStringLiteral("open"));
        if (!s.ended_at) payout->setForeground(QColor(0x60, 0xc0, 0x70));
        sessions->setItem(i, 0, faction);
        sessions->setItem(i, 1, right_aligned(local_time(s.started_at, QStringLiteral("d MMM HH:mm"))));
        sessions->setItem(i, 2, right_aligned(format::number(s.kills)));
        sessions->setItem(i, 3, right_aligned(format::credits(s.bounties)));
        sessions->setItem(i, 4, payout);
    }
}
