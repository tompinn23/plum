#include "journal/projections.hpp"

namespace journal {
    namespace {
        std::optional<timestamp> expiry(const game_event &e) {
            const auto raw = e.opt_str("Expiry");
            return raw ? parse_timestamp(*raw) : std::nullopt;
        }

        // Closes every open session with no active missions left, as of when its last one ended.
        void close_finished(sql::database &db) {
            db.exec(R"(
                UPDATE massacre_session
                   SET ended_at = (SELECT MAX(ended_at) FROM massacre_mission WHERE session = massacre_session.id),
                       payout = (SELECT SUM(reward) FROM massacre_mission
                                  WHERE session = massacre_session.id AND status = 'completed')
                 WHERE ended_at IS NULL
                   AND NOT EXISTS (SELECT 1 FROM massacre_mission
                                    WHERE session = massacre_session.id AND status = 'active'))");
        }

        // The journal does not reliably write MissionFailed when a mission runs out, so anything past
        // its expiry counts as failed. Without a stated expiry, a mission is over once it has outlived
        // the longest a massacre mission lasts: a week for a wing mission, a day otherwise.
        void expire(sql::database &db, const timestamp now) {
            db.prepare(R"(
                UPDATE massacre_mission
                   SET status = 'failed',
                       ended_at = CASE WHEN expires_at < :now THEN expires_at ELSE :now END
                 WHERE status = 'active'
                   AND (expires_at < :now
                        OR accepted_at < CASE WHEN wing THEN :wing_cutoff ELSE :solo_cutoff END))")
                    .bind("now", now)
                    .bind("wing_cutoff", now - std::chrono::days(7))
                    .bind("solo_cutoff", now - std::chrono::hours(24))
                    .run();
            close_finished(db);
        }
    } // namespace

    std::set<std::string> massacre_projection::events() const {
        return {
            "MissionAccepted", "MissionRedirected", "MissionCompleted", "MissionFailed", "MissionAbandoned", "Bounty",
            "Location", "FSDJump", "CarrierJump"
        };
    }

    void massacre_projection::schema(sql::database &db) {
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS massacre_session (
                id             INTEGER PRIMARY KEY,
                target_faction TEXT    NOT NULL,
                system         TEXT,
                started_at     TEXT    NOT NULL,
                ended_at       TEXT,
                kills          INTEGER NOT NULL DEFAULT 0,
                bounties       INTEGER NOT NULL DEFAULT 0,
                payout         INTEGER
            );
            CREATE TABLE IF NOT EXISTS massacre_mission (
                id             INTEGER PRIMARY KEY,
                seq            INTEGER NOT NULL,
                session        INTEGER NOT NULL,
                issuer         TEXT    NOT NULL,
                target_faction TEXT    NOT NULL,
                system         TEXT,
                kills          INTEGER NOT NULL DEFAULT 0,
                kills_needed   INTEGER NOT NULL,
                reward         INTEGER,
                wing           INTEGER NOT NULL,
                status         TEXT    NOT NULL,
                accepted_at    TEXT    NOT NULL,
                expires_at     TEXT,
                ended_at       TEXT
            );
            CREATE INDEX IF NOT EXISTS massacre_open ON massacre_mission(status, target_faction, issuer, seq);
            -- The system the commander is in, so a bounty only counts in the system the stack targets.
            CREATE TABLE IF NOT EXISTS massacre_position (
                id     INTEGER PRIMARY KEY CHECK (id = 0),
                system TEXT
            );
                )");
    }

    void massacre_projection::reset(sql::database &db) {
        db.exec("DROP TABLE IF EXISTS massacre_mission; DROP TABLE IF EXISTS massacre_session; "
            "DROP TABLE IF EXISTS massacre_position;");
    }

    void massacre_projection::apply(sql::database &db, std::int64_t seq, const game_event &event) {
        const auto &name = event.name();
        if (event.time()) expire(db, *event.time());

        if (name == "Location" || name == "FSDJump" || name == "CarrierJump") {
            db.prepare(R"(
                INSERT INTO massacre_position(id, system) VALUES (0, :system)
                ON CONFLICT(id) DO UPDATE SET system = excluded.system)")
                    .bind("system", event.opt_str("StarSystem"))
                    .run();
            return;
        }

        if (name == "Bounty") {
            const auto victim = event.opt_str("VictimFaction");
            if (!victim) return;
            // A mission with no known system counts kills anywhere.
            db.prepare(R"(
                WITH next AS (SELECT id, ROW_NUMBER() OVER (PARTITION BY issuer ORDER BY seq) AS n
                FROM massacre_mission
                WHERE status = 'active' AND target_faction = :victim AND kills < kills_needed
                  AND (system IS NULL OR system = (SELECT system FROM massacre_position)))
                UPDATE massacre_mission SET kills = kills + 1 WHERE id IN (SELECT id FROM next WHERE n = 1))")
                    .bind("victim", *victim)
                    .run();

            db.prepare(R"(
                UPDATE massacre_session SET kills = kills + 1, bounties = bounties + :reward
                WHERE ended_at IS NULL AND target_faction = :victim
                  AND (system IS NULL OR system = (SELECT system FROM massacre_position)))")
                    .bind("victim", *victim)
                    .bind("reward", event.num("TotalReward"))
                    .run();
            return;
        }

        const auto id = event.opt_long("MissionID");
        if (!id) return;

        if (name == "MissionAccepted") {
            const std::string mission = event.str("Name");
            if (!mission.starts_with("Mission_Massacre")) return;
            const auto target = event.opt_str("TargetFaction");
            const auto needed = event.opt_long("KillCount");
            if (!target || !needed || !event.time()) return;

            db.prepare(R"(
            INSERT INTO massacre_session(target_faction, system, started_at)
            SELECT :target, :system, :ts
             WHERE NOT EXISTS (SELECT 1 FROM massacre_session WHERE ended_at IS NULL AND target_faction = :target))")
                    .bind("target", *target)
                    .bind("system", event.opt_str("DestinationSystem"))
                    .bind("ts", *event.time())
                    .run();
            db.prepare(R"(
            INSERT OR IGNORE INTO massacre_mission(id, seq, session, issuer, target_faction, system,
                                                   kills_needed, reward, wing, status, accepted_at, expires_at)
            SELECT :id, :seq, id, :issuer, :target, :system, :needed, :reward, :wing, 'active', :ts, :expires
              FROM massacre_session WHERE ended_at IS NULL AND target_faction = :target)")
                    .bind("id", *id)
                    .bind("seq", seq)
                    .bind("issuer", event.str("Faction"))
                    .bind("target", *target)
                    .bind("system", event.opt_str("DestinationSystem"))
                    .bind("needed", *needed)
                    .bind("reward", event.opt_long("Reward"))
                    .bind("wing", event.flag("Wing") || mission.starts_with("Mission_MassacreWing"))
                    .bind("ts", *event.time())
                    .bind("expires", expiry(event))
                    .run();
        } else if (name == "MissionRedirected") {
            // The game saying the kills are done; trust it over the inferred count.
            db.prepare("UPDATE massacre_mission SET kills = kills_needed WHERE id = :id").bind("id", *id).run();
        } else {
            const char *status = name == "MissionCompleted"
                                     ? "completed"
                                     : name == "MissionFailed"
                                           ? "failed"
                                           : "abandoned";
            db.prepare(R"(
            UPDATE massacre_mission SET status = :status, ended_at = :ts, reward = COALESCE(:reward, reward)
             WHERE id = :id)")
                    .bind("id", *id)
                    .bind("status", status)
                    .bind("ts", event.time())
                    .bind("reward", event.opt_long("Reward"))
                    .run();
            close_finished(db);
        }
    }

    std::vector<massacre_projection::stacked> massacre_projection::active(const history &h) {
        return h.query([](sql::database &db) {
            auto s = db.prepare(R"(
                SELECT id, issuer, target_faction, system, kills, kills_needed, reward, expires_at
                  FROM massacre_mission WHERE status = 'active' ORDER BY issuer, seq)");
            std::vector<stacked> out;
            while (s.step()) {
                out.push_back({
                    .id = s.integer(0),
                    .issuer = s.text(1),
                    .target = s.text(2),
                    .system = s.opt_text(3),
                    .kills = s.integer(4),
                    .kills_needed = s.integer(5),
                    .reward = s.opt_integer(6),
                    .expires_at = s.opt_time(7),
                });
            }
            return out;
        });
    }

    std::vector<massacre_projection::session> massacre_projection::sessions(const history &h, int limit) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(R"(
                SELECT id, target_faction, system, started_at, ended_at, kills, bounties, payout
                  FROM massacre_session ORDER BY ended_at IS NOT NULL, started_at DESC LIMIT :limit)");
            s.bind("limit", limit);
            std::vector<session> out;
            while (s.step()) {
                out.push_back({
                    .id = s.integer(0),
                    .target = s.text(1),
                    .system = s.opt_text(2),
                    .started_at = s.opt_time(3).value_or(timestamp{}),
                    .ended_at = s.opt_time(4),
                    .kills = s.integer(5),
                    .bounties = s.integer(6),
                    .payout = s.opt_integer(7),
                });
            }
            return out;
        });
    }
}
