#pragma once

#include <QObject>

#include "feed_subscriber.hpp"
#include "overlays.hpp"

class QLabel;

// The station the commander is docked at, from its Docked event: who runs it, its economy, pads
// and services. Shown top left over the game until they undock.
class station_overlay: public QObject, public feed_subscriber {
    Q_OBJECT
public:
    explicit station_overlay(std::shared_ptr<journal::commander_feed> feed, overlays& overlay, QObject *parent = nullptr);

private:
    void show(const journal::game_event &docked);

    void hide();

    overlays& overlay;
    overlay_window *window = nullptr; // while docked
    QLabel *details = nullptr;
};
