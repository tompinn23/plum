#include "journal/projections.hpp"

namespace journal {
    namespace {
        constexpr std::string_view columns =
                "SELECT address, name, x, y, z, first_seen, last_seen, visits FROM visited_system";

        std::vector<visited_system_projection::visited> read(sql::statement &s) {
            std::vector<visited_system_projection::visited> out;
            while (s.step()) {
                out.push_back({
                    .address = s.integer(0),
                    .name = s.text(1),
                    .x = s.opt_real(2),
                    .y = s.opt_real(3),
                    .z = s.opt_real(4),
                    .first_seen = s.opt_time(5).value_or(timestamp{}),
                    .last_seen = s.opt_time(6).value_or(timestamp{}),
                    .visits = static_cast<int>(s.integer(7)),
                });
            }
            return out;
        }

        // The system of the arrival before this one, read back from the ledger.
        std::optional<std::int64_t> previous_arrival(sql::database &db, std::int64_t seq) {
            auto s = db.prepare(R"(
        SELECT json_extract(payload, '$.SystemAddress') FROM event
         WHERE seq < :seq AND name IN ('FSDJump', 'CarrierJump', 'Location')
         ORDER BY seq DESC LIMIT 1)");
            s.bind("seq", seq);
            return s.step() ? s.opt_integer(0) : std::nullopt;
        }
    } // namespace

    std::set<std::string> visited_system_projection::events() const {
        return {"FSDJump", "CarrierJump", "Location"};
    }

    void visited_system_projection::schema(sql::database &db) {
        db.exec(R"(
        CREATE TABLE IF NOT EXISTS visited_system (
          address    INTEGER PRIMARY KEY,
          name       TEXT    NOT NULL,
          x          REAL,
          y          REAL,
          z          REAL,
          first_seen TEXT    NOT NULL,
          last_seen  TEXT    NOT NULL,
          visits     INTEGER NOT NULL DEFAULT 1
        );
        CREATE INDEX IF NOT EXISTS visited_system_name ON visited_system(name);
        CREATE INDEX IF NOT EXISTS visited_system_last ON visited_system(last_seen);
    )");
    }

    // The visit counter is why a rebuild drops the table rather than merging into it: replaying over
    // existing rows would double every count.
    void visited_system_projection::reset(sql::database &db) {
        db.exec("DROP TABLE IF EXISTS visited_system");
    }

    void visited_system_projection::apply(sql::database &db, std::int64_t seq, const game_event &event) {
        const auto address = event.opt_long("SystemAddress");
        const std::string system = event.str("StarSystem");
        if (!address || system.empty() || !event.time()) return;

        // A jump always arrives somewhere new. Location is written at every login and after a carrier
        // jump while docked on it, so it is only a visit if it puts you somewhere you were not.
        const bool arrived = event.name() != "Location" || previous_arrival(db, seq) != address;

        const auto pos = event.doubles("StarPos");
        const bool has_pos = pos.size() == 3;
        auto s = db.prepare(R"(
        INSERT INTO visited_system(address, name, x, y, z, first_seen, last_seen, visits)
        VALUES (:address, :name, :x, :y, :z, :seen, :seen, 1)
        ON CONFLICT(address) DO UPDATE SET
          name       = excluded.name,
          x          = COALESCE(excluded.x, visited_system.x),
          y          = COALESCE(excluded.y, visited_system.y),
          z          = COALESCE(excluded.z, visited_system.z),
          first_seen = MIN(visited_system.first_seen, excluded.first_seen),
          last_seen  = MAX(visited_system.last_seen, excluded.last_seen),
          visits     = visited_system.visits + :increment)");
        s.bind("address", *address)
                .bind("name", system)
                .bind("x", has_pos ? std::optional(pos[0]) : std::nullopt)
                .bind("y", has_pos ? std::optional(pos[1]) : std::nullopt)
                .bind("z", has_pos ? std::optional(pos[2]) : std::nullopt)
                .bind("seen", *event.time())
                .bind("increment", arrived ? 1 : 0)
                .run();
    }

    std::vector<visited_system_projection::visited> visited_system_projection::recent(const history &h, int limit) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(std::string(columns) + " ORDER BY last_seen DESC LIMIT :limit");
            s.bind("limit", limit);
            return read(s);
        });
    }

    std::vector<visited_system_projection::visited>
    visited_system_projection::most_visited(const history &h, int limit) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(std::string(columns) + " ORDER BY visits DESC, last_seen DESC LIMIT :limit");
            s.bind("limit", limit);
            return read(s);
        });
    }

    std::optional<visited_system_projection::visited> visited_system_projection::by_name(const history &h,
        const std::string &system) {
        return h.query([&](sql::database &db) -> std::optional<visited> {
            auto s = db.prepare(std::string(columns) + " WHERE name = :name COLLATE NOCASE LIMIT 1");
            s.bind("name", system);
            auto rows = read(s);
            if (rows.empty()) return std::nullopt;
            return std::move(rows.front());
        });
    }

    std::int64_t visited_system_projection::count(const history &h) {
        return h.query([](sql::database &db) {
            auto s = db.prepare("SELECT COUNT(*) FROM visited_system");
            return s.step() ? s.integer(0) : 0;
        });
    }
} // namespace journal
