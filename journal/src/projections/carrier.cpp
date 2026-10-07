#include "journal/projections.hpp"

#include <spdlog/spdlog.h>

namespace journal {
    std::set<std::string> carrier_projection::events() const {
        return {"CarrierBuy", "CarrierStats", "CarrierJumpRequest", "CarrierJumpCancelled", "CarrierLocation"};
    }

    void carrier_projection::schema(sql::database &db) {
        db.exec(R"(
        CREATE TABLE IF NOT EXISTS carrier (
          carrier_id     INTEGER PRIMARY KEY,
          name           TEXT,
          callsign       TEXT,
          type           TEXT,
          system         TEXT,
          system_address INTEGER,
          body           TEXT,
          balance        INTEGER,
          fuel           INTEGER,
          docking        TEXT
        );
        CREATE TABLE IF NOT EXISTS carrier_jumps (
          seq                        INTEGER NOT NULL,
          timestamp                  TEXT    NOT NULL,
          carrier_id                 INTEGER NOT NULL,
          origin_system              TEXT,
          origin_system_address      INTEGER,
          departure_time             TEXT    NOT NULL,
          arrival_time               TEXT,
          destination_system         TEXT    NOT NULL,
          destination_system_address INTEGER NOT NULL,
          destination_body           TEXT
        );
    )");
    }

    void carrier_projection::reset(sql::database &db) {
        db.exec("DROP TABLE IF EXISTS carrier; DROP TABLE IF EXISTS carrier_jumps;");
    }

    void carrier_projection::apply(sql::database &db, std::int64_t seq, const game_event &e) {
        const std::string &name = e.name();
        const auto id = e.opt_long("CarrierID");
        if (!id) {
            spdlog::warn("skipping {} with no CarrierID", name);
            return;
        }

        if (name == "CarrierBuy") {
            const auto type = e.opt_str("CarrierType");
            const auto callsign = e.opt_str("Callsign");
            if (!type || !callsign) {
                spdlog::warn("skipping CarrierBuy missing CarrierType or Callsign");
                return;
            }
            db.prepare(R"(
            INSERT INTO carrier(carrier_id, type, system, system_address, callsign)
            VALUES (:id, :type, :system, :system_address, :callsign)
            ON CONFLICT(carrier_id) DO UPDATE SET
              system = excluded.system, system_address = excluded.system_address, callsign = excluded.callsign)")
                    .bind("id", *id)
                    .bind("type", type)
                    .bind("system", e.opt_str("Location"))
                    .bind("system_address", e.opt_long("SystemAddress"))
                    .bind("callsign", callsign)
                    .run();
        } else if (name == "CarrierStats") {
            // Stats arrive for any carrier you open the management screen of, including a squadron's;
            // only update one already known to be ours.
            db.prepare(R"(
            UPDATE carrier SET name = :name, callsign = :callsign, type = :type,
                               balance = :balance, fuel = :fuel, docking = :docking
             WHERE carrier_id = :id)")
                    .bind("id", *id)
                    .bind("name", e.opt_str("Name"))
                    .bind("callsign", e.opt_str("Callsign"))
                    .bind("type", e.str("CarrierType", "FleetCarrier"))
                    .bind("balance", field::opt_long(e.object("Finance"), "CarrierBalance"))
                    .bind("fuel", e.opt_long("FuelLevel"))
                    .bind("docking", e.opt_str("DockingAccess"))
                    .run();
        } else if (name == "CarrierJumpRequest") {
            const auto departure_text = e.opt_str("DepartureTime");
            const auto departure = departure_text ? parse_timestamp(*departure_text) : std::nullopt;
            const auto destination = e.opt_str("SystemName");
            const auto address = e.opt_long("SystemAddress");
            if (!departure || !destination || !address || !e.time()) {
                spdlog::warn("skipping CarrierJumpRequest missing required fields");
                return;
            }
            db.prepare(R"(
            INSERT INTO carrier_jumps(seq, timestamp, carrier_id, origin_system, origin_system_address,
                                      departure_time, destination_system, destination_system_address,
                                      destination_body)
            SELECT :seq, :ts, :id, system, system_address, :departure, :destination, :address, :body
              FROM carrier WHERE carrier_id = :id)")
                    .bind("seq", seq)
                    .bind("ts", *e.time())
                    .bind("id", *id)
                    .bind("departure", *departure)
                    .bind("destination", destination)
                    .bind("address", address)
                    .bind("body", e.opt_str("Body"))
                    .run();
        } else if (name == "CarrierJumpCancelled") {
            // The latest open jump for this carrier, not the latest jump overall: another carrier's
            // jump being newer must not swallow the cancellation.
            db.prepare(R"(
            DELETE FROM carrier_jumps
             WHERE seq = (SELECT MAX(seq) FROM carrier_jumps WHERE carrier_id = :id AND arrival_time IS NULL))")
                    .bind("id", *id)
                    .run();
        } else if (name == "CarrierLocation") {
            if (!e.time()) return;
            // Close the open jump this arrival completes, picking up the body it was headed for.
            std::optional<std::string> body;
            {
                auto s = db.prepare(R"(
                UPDATE carrier_jumps SET arrival_time = :arrived
                 WHERE arrival_time IS NULL AND carrier_id = :id AND destination_system_address = :address
                RETURNING destination_body)");
                s.bind("arrived", *e.time()).bind("id", *id).bind("address", e.opt_long("SystemAddress"));
                if (s.step()) body = s.opt_text(0);
                while (s.step()) {
                }
            }

            // CarrierLocation is written at login for your own carrier, so it may create the row; for
            // a squadron carrier only update one already known.
            const std::string type = e.str("CarrierType", "FleetCarrier");
            const bool own = type == "FleetCarrier";
            auto s = db.prepare(own
                                    ? R"(
            INSERT INTO carrier(carrier_id, type, system, system_address, body)
            VALUES (:id, :type, :system, :address, :body)
            ON CONFLICT(carrier_id) DO UPDATE SET
              system = excluded.system, system_address = excluded.system_address, body = excluded.body)"
                                    : R"(
            UPDATE carrier SET system = :system, system_address = :address, body = :body
             WHERE carrier_id = :id)");
            s.bind("id", *id)
                    .bind("system", e.opt_str("StarSystem"))
                    .bind("address", e.opt_long("SystemAddress"))
                    .bind("body", body);
            if (own) s.bind("type", type);
            s.run();
        }
    }

    std::optional<carrier_projection::carrier> carrier_projection::info(const history &h, const std::string &type) {
        return h.query([&](sql::database &db) -> std::optional<carrier> {
            auto s = db.prepare(R"(
            SELECT carrier_id, name, callsign, type, system, system_address, body, balance, fuel, docking
              FROM carrier WHERE type = :type)");
            s.bind("type", type);
            if (!s.step()) return std::nullopt;
            return carrier{
                .id = s.integer(0),
                .name = s.opt_text(1),
                .callsign = s.opt_text(2),
                .type = s.opt_text(3),
                .system = s.opt_text(4),
                .system_address = s.opt_integer(5),
                .body = s.opt_text(6),
                .balance = s.opt_integer(7),
                .fuel = s.opt_integer(8),
                .docking = s.opt_text(9),
            };
        });
    }
} // namespace journal
