#include "journal/projections.hpp"

#include <array>
#include <unordered_map>

namespace journal {
    namespace {
        // How an event moves credits. Bounty, FactionKillBond and CapShipBond are deliberately absent:
        // they award vouchers, and the credits arrive later with RedeemVoucher.
        using rule = std::int64_t (*)(const game_event &);

        std::int64_t cost(const game_event &e) { return -e.num("Cost"); }
        std::int64_t price_paid(const game_event &e) { return -e.num("Price"); }
        std::int64_t refund(const game_event &e) { return e.num("Refund"); }

        const std::unordered_map<std::string_view, rule> &rules() {
            static const std::unordered_map<std::string_view, rule> table = {
                {"MarketBuy", [](const game_event &e) { return -e.num("TotalCost"); }},
                {"BuyDrones", [](const game_event &e) { return -e.num("TotalCost"); }},
                {"MarketSell", [](const game_event &e) { return e.num("TotalSale"); }},
                {"SellDrones", [](const game_event &e) { return e.num("TotalSale"); }},
                {"MissionCompleted", [](const game_event &e) { return e.num("Reward") - e.num("Donated"); }},
                {"RedeemVoucher", [](const game_event &e) { return e.num("Amount"); }},
                {"PowerplaySalary", [](const game_event &e) { return e.num("Amount"); }},
                {"PayBounties", [](const game_event &e) { return -e.num("Amount"); }},
                {"PayFines", [](const game_event &e) { return -e.num("Amount"); }},
                {"PayLegacyFines", [](const game_event &e) { return -e.num("Amount"); }},
                {"CommunityGoalReward", [](const game_event &e) { return e.num("Reward"); }},
                {"SellExplorationData", [](const game_event &e) { return e.num("TotalEarnings"); }},
                {"MultiSellExplorationData", [](const game_event &e) { return e.num("TotalEarnings"); }},
                {
                    "SellOrganicData",
                    [](const game_event &e) {
                        // No top-level amount: each sample in BioData carries its own value and bonus.
                        std::int64_t total = 0;
                        for (const auto &row: e.rows("BioData"))
                            total += field::num(row, "Value") + field::num(row, "Bonus");
                        return total;
                    }
                },
                {"ShipyardBuy", [](const game_event &e) { return e.num("SellPrice") - e.num("ShipPrice"); }},
                {"ShipyardSell", [](const game_event &e) { return e.num("ShipPrice"); }},
                {"SellShipOnRebuy", [](const game_event &e) { return e.num("ShipPrice"); }},
                {"ShipyardTransfer", [](const game_event &e) { return -e.num("TransferPrice"); }},
                {"ModuleBuy", [](const game_event &e) { return e.num("SellPrice") - e.num("BuyPrice"); }},
                {"ModuleSell", [](const game_event &e) { return e.num("SellPrice"); }},
                {"ModuleSellRemote", [](const game_event &e) { return e.num("SellPrice"); }},
                {"ModuleStore", cost},
                {"ModuleRetrieve", cost},
                {"FetchRemoteModule", [](const game_event &e) { return -e.num("TransferCost"); }},
                {"BuyAmmo", cost},
                {"Repair", cost},
                {"RepairAll", cost},
                {"RefuelAll", cost},
                {"RefuelPartial", cost},
                {"RestockVehicle", cost},
                {"Resurrect", cost},
                {"CrewHire", cost},
                {"BuyTradeData", cost},
                {"BuyExplorationData", cost},
                {"BookTaxi", cost},
                {"BookDropship", cost},
                {"CancelTaxi", refund},
                {"CancelDropship", refund},
                {"BuyMicroResources", price_paid},
                {"SellMicroResources", [](const game_event &e) { return e.num("Price"); }},
                {"CarrierBuy", price_paid},
                {"CarrierBankTransfer", [](const game_event &e) { return e.num("Withdraw") - e.num("Deposit"); }},
            };
            return table;
        }

        // Payload keys worth showing beside a movement, most specific first.
        constexpr std::array detail_keys = {
            "Type_Localised", "Type", "Name_Localised", "Name", "ShipType",
            "BuyItem", "SellItem", "StarSystem", "Station"
        };

        std::optional<std::string> detail(const game_event &e) {
            for (const char *key: detail_keys) {
                if (auto value = e.opt_str(key); value && !value->empty()) return value;
            }
            return std::nullopt;
        }

        // Events that state the balance outright.
        std::optional<std::int64_t> stated_balance(const game_event &e) {
            if (e.name() == "LoadGame") return e.opt_long("Credits");
            if (e.name() == "CarrierBankTransfer") return e.opt_long("PlayerBalance");
            return std::nullopt;
        }

        constexpr std::string_view columns = "SELECT ts, kind, delta, balance, detail FROM ledger";

        std::vector<ledger_projection::entry> read(sql::statement &s) {
            std::vector<ledger_projection::entry> out;
            while (s.step()) {
                out.push_back({
                    .time = s.opt_time(0).value_or(timestamp{}),
                    .kind = s.text(1),
                    .delta = s.integer(2),
                    .balance = s.opt_integer(3),
                    .detail = s.opt_text(4),
                });
            }
            return out;
        }
    } // namespace

    std::set<std::string> ledger_projection::events() const {
        std::set<std::string> out{"LoadGame"};
        for (const auto &[name, _]: rules()) out.emplace(name);
        return out;
    }

    void ledger_projection::schema(sql::database &db) {
        db.exec(R"(
        CREATE TABLE IF NOT EXISTS ledger (
          seq     INTEGER PRIMARY KEY,
          ts      TEXT    NOT NULL,
          kind    TEXT    NOT NULL,
          delta   INTEGER NOT NULL,
          balance INTEGER,
          detail  TEXT
        );
        CREATE INDEX IF NOT EXISTS ledger_ts ON ledger(ts);
    )");
    }

    void ledger_projection::reset(sql::database &db) {
        db.exec("DROP TABLE IF EXISTS ledger");
    }

    void ledger_projection::apply(sql::database &db, std::int64_t seq, const game_event &event) {
        const auto balance = stated_balance(event);
        std::int64_t delta = 0;
        if (const auto it = rules().find(event.name()); it != rules().end()) delta = it->second(event);
        if (delta == 0 && !balance) return; // nothing moved and nothing stated; not worth a row

        db.prepare("INSERT INTO ledger(seq, ts, kind, delta, balance, detail) "
                    "VALUES (:seq, :ts, :kind, :delta, :balance, :detail)")
                .bind("seq", seq)
                .bind("ts", event.time())
                .bind("kind", event.name())
                .bind("delta", delta)
                .bind("balance", balance)
                .bind("detail", detail(event))
                .run();
    }

    std::vector<ledger_projection::entry> ledger_projection::recent(const history &h, int limit) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(std::string(columns) + " ORDER BY seq DESC LIMIT :limit");
            s.bind("limit", limit);
            return read(s);
        });
    }

    std::vector<ledger_projection::entry> ledger_projection::between(const history &h, timestamp from, timestamp to) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(std::string(columns) + " WHERE ts BETWEEN :from AND :to ORDER BY seq");
            s.bind("from", from).bind("to", to);
            return read(s);
        });
    }

    std::int64_t ledger_projection::net(const history &h) {
        return h.query([](sql::database &db) {
            auto s = db.prepare("SELECT COALESCE(SUM(delta), 0) FROM ledger");
            return s.step() ? s.integer(0) : 0;
        });
    }
} // namespace journal
