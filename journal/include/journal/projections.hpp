#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "journal/history.hpp"

// The built-in projections, each with the reads for the tables it owns.
namespace journal {
    // Every credit movement, as a running ledger. The journal only ever states what the balance is
    // now, never where money went, so this has to be accumulated or it is lost.
    //
    // LoadGame and CarrierBankTransfer state the balance outright; they are stored with the balance
    // set, so a reconstructed balance can be checked against them.
    class ledger_projection final : public projection {
    public:
        [[nodiscard]] std::string_view name() const override { return "ledger"; }
        [[nodiscard]] int version() const override { return 1; }

        [[nodiscard]] std::set<std::string> events() const override;

        void schema(sql::database &db) override;

        void reset(sql::database &db) override;

        void apply(sql::database &db, std::int64_t seq, const game_event &event) override;

        struct entry {
            timestamp time;
            std::string kind;
            std::int64_t delta = 0;
            std::optional<std::int64_t> balance; // only where the game stated it
            std::optional<std::string> detail;
        };

        // Newest first.
        static std::vector<entry> recent(const history &h, int limit);

        // Oldest first, the shape a chart wants.
        static std::vector<entry> between(const history &h, timestamp from, timestamp to);

        // Sum of every recorded movement.
        static std::int64_t net(const history &h);
    };

    // Every system visited, with coordinates and a visit count. A Location in the system you were
    // already in (logging in again, say) is not a new visit.
    class visited_system_projection final : public projection {
    public:
        [[nodiscard]] std::string_view name() const override { return "visited_system"; }
        [[nodiscard]] int version() const override { return 1; }

        [[nodiscard]] std::set<std::string> events() const override;

        void schema(sql::database &db) override;

        void reset(sql::database &db) override;

        void apply(sql::database &db, std::int64_t seq, const game_event &event) override;

        struct visited {
            std::int64_t address = 0;
            std::string name;
            std::optional<double> x, y, z;
            timestamp first_seen;
            timestamp last_seen;
            int visits = 0;
        };

        static std::vector<visited> recent(const history &h, int limit);

        static std::vector<visited> most_visited(const history &h, int limit);

        static std::optional<visited> by_name(const history &h, const std::string &system);

        static std::int64_t count(const history &h);
    };

    // One row per mission, folded together from the events its life is scattered across. Where it
    // was taken is read back from the most recent arrival in the ledger.
    class mission_log_projection final : public projection {
    public:
        enum class status { accepted, completed, failed, abandoned };

        [[nodiscard]] std::string_view name() const override { return "mission_log"; }
        [[nodiscard]] int version() const override { return 1; }

        [[nodiscard]] std::set<std::string> events() const override;

        void schema(sql::database &db) override;

        void reset(sql::database &db) override;

        void apply(sql::database &db, std::int64_t seq, const game_event &event) override;

        struct mission {
            std::int64_t id = 0;
            std::optional<std::string> name, title, faction;
            status state = status::accepted;
            std::optional<timestamp> accepted_at, ended_at, expires_at;
            std::optional<std::int64_t> reward, fine, donation;
            std::optional<std::string> target, target_type, commodity;
            std::optional<std::int64_t> quantity;
            std::optional<std::string> origin_system, origin_station;
            std::optional<std::string> destination_system, destination_station, destination_settlement;
        };

        // Still open, soonest deadline first.
        static std::vector<mission> active(const history &h);

        // Most recently touched, newest first.
        static std::vector<mission> recent(const history &h, int limit);

        static std::int64_t count(const history &h, status s);

        // Credits actually paid by completed missions.
        static std::int64_t earned(const history &h);
    };

    // Fleet carriers and their jumps.
    class carrier_projection final : public projection {
    public:
        [[nodiscard]] std::string_view name() const override { return "carrier"; }
        [[nodiscard]] int version() const override { return 1; }

        [[nodiscard]] std::set<std::string> events() const override;

        void schema(sql::database &db) override;

        void reset(sql::database &db) override;

        void apply(sql::database &db, std::int64_t seq, const game_event &event) override;

        struct carrier {
            std::int64_t id = 0;
            std::optional<std::string> name, callsign, type, system;
            std::optional<std::int64_t> system_address;
            std::optional<std::string> body;
            std::optional<std::int64_t> balance, fuel;
            std::optional<std::string> docking;
        };

        // The commander's carrier of a type ("FleetCarrier" or "SquadronCarrier"), if known.
        static std::optional<carrier> info(const history &h, const std::string &type = "FleetCarrier");
    };


    class massacre_projection : public projection {
    public:
        [[nodiscard]] std::string_view name() const override { return "massacre"; }
        [[nodiscard]] int version() const override { return 3; }

        [[nodiscard]] std::set<std::string> events() const override;

        void schema(sql::database &db) override;

        void reset(sql::database &db) override;

        void apply(sql::database &db, std::int64_t seq, const game_event &event) override;

        struct stacked {
            std::int64_t id = 0;
            std::string issuer, target;
            std::optional<std::string> system;
            std::int64_t kills = 0, kills_needed = 0;
            std::optional<std::int64_t> reward;
            std::optional<timestamp> expires_at;
        };

        struct session {
            std::int64_t id = 0;
            std::string target;
            std::optional<std::string> system;
            timestamp started_at;
            std::optional<timestamp> ended_at;
            std::int64_t kills = 0, bounties = 0;
            std::optional<std::int64_t> payout;
        };

        // Missions still open, grouped by issuer, oldest first within each: the order kills land in.
        static std::vector<stacked> active(const history &);

        // Newest first; the open session, if any, leads.
        static std::vector<session> sessions(const history &, int limit);
    };

    // A fresh instance of each projection above, in the order history_config::standard applies them.
    std::vector<std::shared_ptr<projection> > builtin_projections();
} // namespace journal
