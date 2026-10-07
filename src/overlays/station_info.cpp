#include "overlays/station_info.hpp"

#include <algorithm>

#include <QLabel>
#include <QLocale>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

#include "feed_subscriber.hpp"
#include "overlays.hpp"

namespace {
    // The services worth knowing about on arrival, by their journal names, in the order shown.
    constexpr std::pair<std::string_view, const char *> notable_services[] = {
        {"refuel", "Refuel"},
        {"repair", "Repair"},
        {"rearm", "Rearm"},
        {"commodities", "Market"},
        {"blackmarket", "Black market"},
        {"shipyard", "Shipyard"},
        {"outfitting", "Outfitting"},
        {"exploration", "Cartographics"},
        {"facilitator", "Interstellar Factors"},
        {"materialtrader", "Material trader"},
        {"techBroker", "Tech broker"},
    };

    // The _Localised form is what the game shows; the plain one may be a symbol like "$economy_Industrial;".
    QString shown(const journal::json &row, const std::string &key) {
        auto value = journal::field::opt_str(row, key + "_Localised");
        if (!value || value->empty()) value = journal::field::opt_str(row, key);
        return value && !value->empty() && !value->starts_with('$') ? QString::fromStdString(*value) : QString();
    }

    QString row(const QString &key, const QString &value) {
        return value.isEmpty() ? QString()
                               : QStringLiteral("<tr><td style='color:#8a8a8a; padding-right:10px'>%1</td><td>%2</td></tr>")
                                         .arg(key, value.toHtmlEscaped());
    }

    QString describe(const journal::game_event &e) {
        const auto &p = e.payload();

        QString html = QStringLiteral("<b style='color:#f0b040; font-size:14px'>%1</b>")
                               .arg(QString::fromStdString(e.str("StationName", "?")).toHtmlEscaped());
        QStringList where;
        if (const auto type = shown(p, "StationType"); !type.isEmpty()) where << type;
        if (const auto system = shown(p, "StarSystem"); !system.isEmpty()) where << system;
        if (!where.isEmpty()) html += QStringLiteral("<br><span style='color:#8a8a8a'>%1</span>").arg(where.join(" · ").toHtmlEscaped());

        const auto &faction = e.object("StationFaction");
        QString owner = shown(faction, "Name");
        if (const auto state = shown(faction, "FactionState"); !owner.isEmpty() && !state.isEmpty() && state != "None")
            owner += QStringLiteral(" (%1)").arg(state);

        QStringList politics;
        if (const auto government = shown(p, "StationGovernment"); !government.isEmpty()) politics << government;
        if (const auto allegiance = shown(p, "StationAllegiance"); !allegiance.isEmpty()) politics << allegiance;

        QStringList economies;
        for (const auto &economy: e.rows("StationEconomies")) {
            const auto name = shown(economy, "Name");
            if (name.isEmpty()) continue;
            const double share = journal::field::dec(economy, "Proportion");
            economies << (share > 0 && share < 1 ? QStringLiteral("%1 %2%").arg(name).arg(qRound(share * 100)) : name);
        }
        if (economies.isEmpty())
            if (const auto economy = shown(p, "StationEconomy"); !economy.isEmpty()) economies << economy;

        QString distance;
        if (const auto ls = e.opt_double("DistFromStarLS"))
            distance = QLocale().toString(qRound64(*ls)) + QStringLiteral(" ls");

        const auto &pads = e.object("LandingPads");
        QString landing;
        if (!pads.empty())
            landing = QStringLiteral("S %1 · M %2 · L %3")
                              .arg(journal::field::num(pads, "Small"))
                              .arg(journal::field::num(pads, "Medium"))
                              .arg(journal::field::num(pads, "Large"));

        const auto services = journal::field::strings(p, "StationServices");
        QStringList offered;
        for (const auto &[key, label]: notable_services)
            if (std::ranges::find(services, key) != services.end()) offered << label;

        html += QStringLiteral("<table style='margin-top:6px'>%1%2%3%4%5%6</table>")
                        .arg(row("Faction", owner), row("Government", politics.join(" · ")),
                             row("Economy", economies.join(", ")), row("From star", distance), row("Pads", landing),
                             row("Services", offered.join(", ")));
        return html;
    }
} // namespace

station_overlay::station_overlay(std::shared_ptr<journal::commander_feed> feed, overlays& overlay, QObject *parent) : QObject(
    parent), feed_subscriber(this, std::move(feed)), overlay(overlay) {
    subscribe({"Docked"}, [this](const journal::game_state &, const journal::game_event &event) { show(event); });
    subscribe({"Undocked"}, [this](const journal::game_state &, const journal::game_event &) { hide(); });
}

void station_overlay::show(const journal::game_event &docked) {
    if (!window) {
        window = overlay.spawn(feed()->id(), {.corner = Qt::TopLeftCorner, .margin = {32, 32}, .size = {320, 0}});
        if (!window) return;
        window->setObjectName("station_info");
        details = new QLabel;
        details->setObjectName("station_info");
        details->setWordWrap(true);
        details->setTextFormat(Qt::RichText);
        window->body()->addWidget(details);
    }
    details->setText(describe(docked));
    window->refit();
}

void station_overlay::hide() {
    overlay.close(window);
    window = nullptr;
    details = nullptr;
}
