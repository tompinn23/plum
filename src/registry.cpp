#include "registry.hpp"

#include "journal/projections.hpp"

#include "dashboards/carrier_dashboard.hpp"
#include "dashboards/commander_dashboard.hpp"
#include "dashboards/event_log_dashboard.hpp"
#include "dashboards/massacre_dashboard.hpp"
#include "overlays/station_info.hpp"

void register_builtins(registry &r) {
    // Sidebar order.
    r.add_dashboard<commander_dashboard>();
    r.add_dashboard<carrier_dashboard>();
    r.add_dashboard<massacre_dashboard>();
    r.add_dashboard<event_log_dashboard>();

    r.add_overlay<station_overlay>();

    for (auto &p: journal::builtin_projections()) r.add_projection(std::move(p));
}
