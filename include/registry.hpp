#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>

#include "journal/history.hpp"

class dashboard;
class overlays;

namespace journal {
    class commander_feed;
}

// Everything the app builds per feed, and every projection the history store runs. The main
// window asks this what to make instead of naming each feature itself, so adding one is a single
// registration in register_builtins() (or, later, a plugin's).
//
// Order is kept: dashboards appear in the sidebar in the order they were added, and projections
// are applied in it.
class registry {
public:
    // A new page for a feed. Every feed gets one of each, so the sidebar can index into any set.
    using dashboard_factory =
            std::function<dashboard *(const std::shared_ptr<journal::commander_feed> &, ::overlays &)>;

    // Fills a feed's overlays. Owned by the feed's entry and gone before the feed's overlay windows.
    using overlay_factory =
            std::function<std::unique_ptr<QObject>(const std::shared_ptr<journal::commander_feed> &, ::overlays &)>;

    void add_dashboard(dashboard_factory make) { dashboards_.push_back(std::move(make)); }

    template<std::derived_from<dashboard> T>
    void add_dashboard() {
        add_dashboard([](const std::shared_ptr<journal::commander_feed> &feed, ::overlays &notes) -> dashboard * {
            return new T(feed, notes);
        });
    }

    void add_overlay(overlay_factory make) { overlay_providers_.push_back(std::move(make)); }

    template<std::derived_from<QObject> T>
    void add_overlay() {
        add_overlay([](const std::shared_ptr<journal::commander_feed> &feed, ::overlays &notes) -> std::unique_ptr<QObject> {
            return std::make_unique<T>(feed, notes);
        });
    }

    // Must be added before the journal service starts: the store's projections are fixed then.
    void add_projection(std::shared_ptr<journal::projection> p) { projections_.push_back(std::move(p)); }

    [[nodiscard]] const std::vector<dashboard_factory> &dashboards() const { return dashboards_; }

    [[nodiscard]] const std::vector<overlay_factory> &overlay_providers() const { return overlay_providers_; }

    [[nodiscard]] const std::vector<std::shared_ptr<journal::projection> > &projections() const {
        return projections_;
    }

private:
    std::vector<dashboard_factory> dashboards_;
    std::vector<overlay_factory> overlay_providers_;
    std::vector<std::shared_ptr<journal::projection> > projections_;
};

// Adds every dashboard, overlay and projection that ships with plum.
void register_builtins(registry &r);
