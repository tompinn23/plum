#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "journal/journal_service.hpp"
#include "journal/projections.hpp"
#include "journal_files.hpp"

namespace fs = std::filesystem;
using namespace journal;
using namespace std::chrono_literals;

namespace {

// A scratch directory removed on scope exit.
struct temp_dir {
    fs::path path;
    explicit temp_dir(std::string_view name) {
        path = fs::temp_directory_path() / "plum_journal_tests" / name;
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path write(const fs::path& dir, std::string_view name, std::string_view content) {
    fs::create_directories(dir);
    const fs::path file = dir / name;
    std::ofstream(file, std::ios::binary) << content;
    return file;
}

void append(const fs::path& file, std::string_view content) {
    std::ofstream(file, std::ios::binary | std::ios::app) << content;
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

std::string session(std::string_view commander, std::int64_t credits, std::string_view fid = "F1") {
    return std::format(
        R"({{"timestamp":"2024-01-02T03:04:05Z","event":"Fileheader","gameversion":"4.0.0.1"}})"
        "\n"
        R"({{"timestamp":"2024-01-02T03:04:06Z","event":"Commander","FID":"{2}","Name":"{0}"}})"
        "\n"
        R"({{"timestamp":"2024-01-02T03:04:07Z","event":"LoadGame","Commander":"{0}","FID":"{2}","Credits":{1},"GameMode":"Solo"}})"
        "\n",
        commander, credits, fid);
}

// Opens a feed, starts it, and waits for it to go live. Records the last progress report.
struct started_feed {
    std::shared_ptr<commander_feed> feed;
    std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<ingest_progress> last = std::make_shared<ingest_progress>();
    subscription progress;

    started_feed(journal_service& service, const std::string& id, const fs::path& dir) {
        feed = service.open(id, dir);
        progress = feed->on_progress([live = live, last = last](const ingest_progress& p) {
            *last = p;
            if (p.caught_up()) *live = true;
        });
        feed->start();
        REQUIRE(wait_until([&] { return live->load(); }));
    }
};

}  // namespace

// ── files ───────────────────────────────────────────────────────────────────

TEST_CASE("journal files order by the time and part in their names, not as text") {
    temp_dir dir("ordering");
    write(dir.path, "Journal.2024-01-03T030405.10.log", "");
    write(dir.path, "Journal.230101010101.01.log", "");
    write(dir.path, "Journal.2024-01-03T030405.9.log", "");
    write(dir.path, "Journal.2024-01-02T030405.01.log", "");
    write(dir.path, "Status.json", "");

    std::vector<std::string> names;
    for (const auto& f : files::list(dir.path)) names.push_back(files::utf8(f.path.filename()));
    CHECK(names == std::vector<std::string>{
                       "Journal.230101010101.01.log",
                       "Journal.2024-01-02T030405.01.log",
                       "Journal.2024-01-03T030405.9.log",
                       "Journal.2024-01-03T030405.10.log",
                   });
    CHECK(files::utf8(files::newest(dir.path)->path.filename()) == "Journal.2024-01-03T030405.10.log");
}

TEST_CASE("read_lines reports line offsets and holds back a partial line") {
    temp_dir dir("lines");
    const auto file = write(dir.path, "j.log", "ab\r\ncd\nef\rgh\rpartial");

    std::vector<std::pair<std::string, std::uint64_t>> lines;
    const auto sink = [&](std::string_view line, std::uint64_t offset) { lines.emplace_back(line, offset); };
    const auto next = files::read_lines(file, 0, sink);

    CHECK(lines == std::vector<std::pair<std::string, std::uint64_t>>{{"ab", 0}, {"cd", 4}, {"ef", 7}, {"gh", 10}});
    CHECK(next == 13);  // the first byte of "partial", so the next read picks it up whole

    append(file, " line\n");
    lines.clear();
    CHECK(files::read_lines(file, next, sink) == 26);
    CHECK(lines == std::vector<std::pair<std::string, std::uint64_t>>{{"partial line", 13}});
}

// ── sql ─────────────────────────────────────────────────────────────────────

TEST_CASE("statements reject unknown and unbound parameters rather than writing NULL") {
    temp_dir dir("sql");
    sql::database db(dir.path / "t.db");
    db.exec("CREATE TABLE t (a INTEGER, b TEXT)");

    auto s = db.prepare("INSERT INTO t(a, b) VALUES (:a, :b)");
    CHECK_THROWS_AS(s.bind("nope", 1), sql::error);
    s.bind("a", 1);
    CHECK_THROWS_AS(s.run(), sql::error);
    s.reset();
    s.bind("a", 1).bind("b", "x").run();

    auto q = db.prepare("SELECT a, b FROM t");
    REQUIRE(q.step());
    CHECK(q.integer(0) == 1);
    CHECK(q.text(1) == "x");
}

// ── feed ────────────────────────────────────────────────────────────────────

TEST_CASE("a feed replays history oldest first, catches up on the newest, then tails it") {
    temp_dir dir("feed");
    write(dir.path, "Journal.230101010101.01.log", session("Kirk", 100));
    write(dir.path, "Journal.2024-01-02T030405.01.log", session("Picard", 1'000));
    write(dir.path, "Journal.2024-01-03T030405.9.log", session("Sisko", 2'000));
    const auto newest = write(dir.path, "Journal.2024-01-03T030405.10.log", session("Janeway", 5'000));

    journal_service service;
    auto feed = service.open("test", dir.path);

    std::mutex mutex;
    std::vector<std::string> history, catchup;
    auto loads = feed->subscribe({"LoadGame"}, [&](const game_event& e, phase p) {
        std::lock_guard lock(mutex);
        (p == phase::history ? history : catchup).push_back(e.str("Commander"));
    }, {phase::history, phase::catchup});

    std::atomic<bool> live{false};
    auto progress = feed->on_progress([&](const ingest_progress& p) { if (p.caught_up()) live = true; });
    feed->start();
    REQUIRE(wait_until([&] { return live.load(); }));

    {
        std::lock_guard lock(mutex);
        CHECK(history == std::vector<std::string>{"Kirk", "Picard", "Sisko"});
        CHECK(catchup == std::vector<std::string>{"Janeway"});
    }
    CHECK(feed->state()->credits == 5'000);  // from the .10 file, not the lexically last .9
    CHECK(feed->state()->name == "Janeway");

    std::atomic<bool> redeemed{false};
    auto vouchers = feed->subscribe({"RedeemVoucher"}, [&](const game_event&, phase) { redeemed = true; });
    append(newest, R"({"timestamp":"2024-01-03T04:00:00Z","event":"RedeemVoucher","Amount":250})" "\n");

    REQUIRE(wait_until([&] { return redeemed.load(); }));
    CHECK(feed->state()->credits == 5'250);
}

TEST_CASE("an incomplete trailing line is never emitted") {
    temp_dir dir("partial");
    write(dir.path, "Journal.2024-01-02T030405.01.log",
          session("Picard", 1'000) + R"({"timestamp":"2024-01-02T03:06:00Z","eve)");

    journal_service service;
    auto feed = service.open("test", dir.path);
    std::mutex mutex;
    std::vector<std::string> seen;
    auto all = feed->subscribe({}, [&](const game_event& e, phase) {
        std::lock_guard lock(mutex);
        seen.push_back(e.name());
    }, {phase::history, phase::catchup, phase::live});
    std::atomic<bool> live{false};
    auto progress = feed->on_progress([&](const ingest_progress& p) { if (p.caught_up()) live = true; });
    feed->start();
    REQUIRE(wait_until([&] { return live.load(); }));

    std::lock_guard lock(mutex);
    CHECK(seen == std::vector<std::string>{"Fileheader", "Commander", "LoadGame"});
}

TEST_CASE("feeds in different directories share the one ingest thread and both go live") {
    temp_dir a("multiplex_a");
    temp_dir b("multiplex_b");
    const auto a_file = write(a.path, "Journal.2024-01-02T030405.01.log", session("Kirk", 100, "FA"));
    const auto b_file = write(b.path, "Journal.2024-01-02T030405.01.log", session("Picard", 200, "FB"));
    for (int i = 0; i < 20; ++i) {
        write(a.path, std::format("Journal.2023-01-{:02}T000000.01.log", i + 1), session("Kirk", i, "FA"));
        write(b.path, std::format("Journal.2023-01-{:02}T000000.01.log", i + 1), session("Picard", i, "FB"));
    }

    journal_service service;
    started_feed fa(service, "a", a.path);
    started_feed fb(service, "b", b.path);
    CHECK(fa.feed->state()->credits == 100);
    CHECK(fb.feed->state()->credits == 200);

    append(a_file, R"({"timestamp":"2024-01-02T04:00:00Z","event":"RedeemVoucher","Amount":1})" "\n");
    append(b_file, R"({"timestamp":"2024-01-02T04:00:00Z","event":"RedeemVoucher","Amount":2})" "\n");
    CHECK(wait_until([&] { return fa.feed->state()->credits == 101 && fb.feed->state()->credits == 202; }));
}

// ── history ─────────────────────────────────────────────────────────────────

TEST_CASE("history persists, and a second launch resumes without re-reading") {
    temp_dir root("persist");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";

    write(journals, "Journal.2024-01-01T120000.01.log",
          R"({"timestamp":"2024-01-01T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-01T12:00:01Z","event":"LoadGame","Commander":"Janeway","Credits":1000000,"GameMode":"Solo"}
{"timestamp":"2024-01-01T12:10:00Z","event":"FSDJump","StarSystem":"Sol","SystemAddress":10477373803,"StarPos":[0.0,0.0,0.0]}
{"timestamp":"2024-01-01T12:20:00Z","event":"MarketSell","Type":"gold","Count":100,"TotalSale":500000}
)");
    write(journals, "Journal.2024-01-02T120000.01.log",
          R"({"timestamp":"2024-01-02T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-02T12:00:01Z","event":"LoadGame","Commander":"Janeway","Credits":1500000,"GameMode":"Solo"}
{"timestamp":"2024-01-02T12:10:00Z","event":"FSDJump","StarSystem":"Sol","SystemAddress":10477373803,"StarPos":[0.0,0.0,0.0]}
{"timestamp":"2024-01-02T12:30:00Z","event":"MarketBuy","Type":"tritium","Count":50,"TotalCost":200000}
)");

    std::int64_t events = 0;
    {
        journal_service service(history_config::standard(store));
        started_feed f(service, "cmdr", journals);
        const auto h = f.feed->history();

        events = h.event_count();
        CHECK(events == 8);
        CHECK(h.file_count() == 1);  // only the older file is complete

        const auto systems = visited_system_projection::recent(h, 10);
        REQUIRE(systems.size() == 1);
        CHECK(systems.front().name == "Sol");
        CHECK(systems.front().visits == 2);
        CHECK(ledger_projection::net(h) == 300'000);
    }
    {
        journal_service service(history_config::standard(store));
        started_feed f(service, "cmdr", journals);
        CHECK(f.last->files_skipped == 1);

        const auto h = f.feed->history();
        CHECK(h.event_count() == events);
        CHECK(visited_system_projection::recent(h, 10).front().visits == 2);
        CHECK(ledger_projection::net(h) == 300'000);
    }
}

namespace {

// Labels each jump differently depending on its version, to prove a rebuild happened.
class jump_labels final : public projection {
public:
    explicit jump_labels(int version) : version_(version) {}
    [[nodiscard]] std::string_view name() const override { return "jump_labels"; }
    [[nodiscard]] int version() const override { return version_; }
    [[nodiscard]] std::set<std::string> events() const override { return {"FSDJump"}; }
    void schema(sql::database& db) override {
        db.exec("CREATE TABLE IF NOT EXISTS jump_labels (seq INTEGER PRIMARY KEY, label TEXT NOT NULL)");
    }
    void reset(sql::database& db) override { db.exec("DROP TABLE IF EXISTS jump_labels"); }
    void apply(sql::database& db, std::int64_t seq, const game_event& e) override {
        db.prepare("INSERT INTO jump_labels(seq, label) VALUES (:seq, :label)")
            .bind("seq", seq)
            .bind("label", std::format("v{}:{}", version_, e.str("StarSystem")))
            .run();
    }

private:
    int version_;
};

std::vector<std::string> labels(const history& h) {
    return h.query([](sql::database& db) {
        std::vector<std::string> out;
        auto s = db.prepare("SELECT label FROM jump_labels ORDER BY seq");
        while (s.step()) out.push_back(s.text(0));
        return out;
    });
}

}  // namespace

TEST_CASE("a projection whose version changed is rebuilt from the ledger, not the journals") {
    temp_dir root("rebuild");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    write(journals, "Journal.2024-01-01T120000.01.log",
          R"({"timestamp":"2024-01-01T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-01T12:10:00Z","event":"FSDJump","StarSystem":"Sol","SystemAddress":1,"StarPos":[0.0,0.0,0.0]}
{"timestamp":"2024-01-01T12:20:00Z","event":"FSDJump","StarSystem":"Achenar","SystemAddress":2,"StarPos":[67.5,-119.4,24.8]}
)");
    write(journals, "Journal.2024-01-02T120000.01.log",
          R"({"timestamp":"2024-01-02T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
)");

    auto config = [&](int version) {
        history_config c;
        c.directory = store;
        c.projections = {std::make_shared<jump_labels>(version)};
        return c;
    };

    {
        journal_service service(config(1));
        started_feed f(service, "cmdr", journals);
        CHECK(labels(f.feed->history()) == std::vector<std::string>{"v1:Sol", "v1:Achenar"});
    }

    fs::remove_all(journals);  // the rebuild must not need them
    fs::create_directories(journals);
    {
        journal_service service(config(2));
        started_feed f(service, "cmdr", journals);
        CHECK(labels(f.feed->history()) == std::vector<std::string>{"v2:Sol", "v2:Achenar"});
    }
}

TEST_CASE("the store binds to the most recent commander and keeps others out, even later") {
    temp_dir root("binding");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    const std::string sell = R"({"timestamp":"2024-01-01T12:20:00Z","event":"MarketSell","Type":"gold","Count":1,"TotalSale":100}
)";
    write(journals, "Journal.2024-01-01T120000.01.log", session("Alt", 5, "ALT") + sell);
    write(journals, "Journal.2024-01-02T120000.01.log", session("Main", 10, "MAIN") + sell);
    write(journals, "Journal.2024-01-03T120000.01.log", session("Main", 20, "MAIN"));

    {
        journal_service service(history_config::standard(store));
        started_feed f(service, "cmdr", journals);
        CHECK(ledger_projection::net(f.feed->history()) == 100);  // Main's sale only
        CHECK(f.feed->history().file_count() == 1);
    }

    // The alt plays again, most recently of all. The store stays Main's; live state follows the alt.
    write(journals, "Journal.2024-01-04T120000.01.log", session("Alt", 7, "ALT") + sell);
    {
        journal_service service(history_config::standard(store));
        started_feed f(service, "cmdr", journals);
        CHECK(ledger_projection::net(f.feed->history()) == 100);
        CHECK(f.feed->state()->name == "Alt");
        CHECK(f.feed->state()->credits == 107);
    }
}

TEST_CASE("the ledger counts voucher income once, organic sales in full, and carrier transfers") {
    temp_dir root("ledger");
    const fs::path journals = root.path / "journals";
    write(journals, "Journal.2024-01-01T120000.01.log",
          session("Main", 1'000, "MAIN") +
              R"({"timestamp":"2024-01-01T12:01:00Z","event":"Bounty","TotalReward":5000,"VictimFaction":"Pirates"}
{"timestamp":"2024-01-01T12:02:00Z","event":"FactionKillBond","Reward":3000,"AwardingFaction":"Feds"}
{"timestamp":"2024-01-01T12:03:00Z","event":"RedeemVoucher","Type":"bounty","Amount":8000}
{"timestamp":"2024-01-01T12:04:00Z","event":"SellOrganicData","MarketID":1,"BioData":[{"Genus":"$Codex_Ent_Bacterial_Genus_Name;","Value":1000,"Bonus":4000},{"Genus":"$Codex_Ent_Stratum_Genus_Name;","Value":2000,"Bonus":0}]}
{"timestamp":"2024-01-01T12:05:00Z","event":"CarrierBankTransfer","CarrierID":1,"Deposit":500,"PlayerBalance":15500,"CarrierBalance":500}
)");
    write(journals, "Journal.2024-01-02T120000.01.log", session("Main", 15'500, "MAIN"));

    journal_service service(history_config::standard(root.path / "history"));
    started_feed f(service, "cmdr", journals);
    const auto h = f.feed->history();

    CHECK(ledger_projection::net(h) == 8'000 + 7'000 - 500);
    const auto entries = ledger_projection::recent(h, 10);
    const auto transfer = std::ranges::find(entries, std::string("CarrierBankTransfer"), &ledger_projection::entry::kind);
    REQUIRE(transfer != entries.end());
    CHECK(transfer->balance == 15'500);
}

TEST_CASE("live credits follow the events mun's parser missed") {
    temp_dir dir("credits");
    write(dir.path, "Journal.2024-01-02T030405.01.log",
          session("Main", 1'000) +
              R"({"timestamp":"2024-01-02T03:05:00Z","event":"MultiSellExplorationData","TotalEarnings":500}
{"timestamp":"2024-01-02T03:06:00Z","event":"PayFines","Amount":100}
{"timestamp":"2024-01-02T03:07:00Z","event":"SellOrganicData","BioData":[{"Value":10,"Bonus":5}]}
{"timestamp":"2024-01-02T03:08:00Z","event":"RepairAll","Cost":15}
)");

    journal_service service;
    started_feed f(service, "cmdr", dir.path);
    CHECK(f.feed->state()->credits == 1'000 + 500 - 100 + 15 - 15);
}

TEST_CASE("logging in again in the same system is not a new visit") {
    temp_dir root("visits");
    const fs::path journals = root.path / "journals";
    auto location = [](int day) {
        return std::format(
            R"({{"timestamp":"2024-01-0{}T12:00:02Z","event":"Location","StarSystem":"Sol","SystemAddress":1,"StarPos":[0.0,0.0,0.0]}})"
            "\n",
            day);
    };
    write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 1, "MAIN") + location(1));
    write(journals, "Journal.2024-01-02T120000.01.log", session("Main", 1, "MAIN") + location(2));
    write(journals, "Journal.2024-01-03T120000.01.log",
          session("Main", 1, "MAIN") + location(3) +
              R"({"timestamp":"2024-01-03T13:00:00Z","event":"FSDJump","StarSystem":"Achenar","SystemAddress":2,"StarPos":[1.0,2.0,3.0]}
{"timestamp":"2024-01-03T14:00:00Z","event":"FSDJump","StarSystem":"Sol","SystemAddress":1,"StarPos":[0.0,0.0,0.0]}
)");

    journal_service service(history_config::standard(root.path / "history"));
    started_feed f(service, "cmdr", journals);
    const auto sol = visited_system_projection::by_name(f.feed->history(), "Sol");
    REQUIRE(sol);
    CHECK(sol->visits == 2);  // the first login, then the jump back; not the two later logins
}

TEST_CASE("a mission records where it was taken, from the ledger") {
    temp_dir root("missions");
    const fs::path journals = root.path / "journals";
    write(journals, "Journal.2024-01-01T120000.01.log",
          session("Main", 1, "MAIN") +
              R"({"timestamp":"2024-01-01T12:01:00Z","event":"Docked","StarSystem":"Sol","StationName":"Abraham Lincoln","MarketID":1}
{"timestamp":"2024-01-01T12:02:00Z","event":"MissionAccepted","MissionID":42,"Name":"Mission_Delivery","LocalisedName":"Deliver gold","Faction":"Feds","Reward":10000,"Expiry":"2024-01-08T12:00:00Z","DestinationSystem":"Achenar"}
{"timestamp":"2024-01-01T12:30:00Z","event":"FSDJump","StarSystem":"Achenar","SystemAddress":2,"StarPos":[1.0,2.0,3.0]}
{"timestamp":"2024-01-01T13:00:00Z","event":"MissionCompleted","MissionID":42,"Name":"Mission_Delivery","Faction":"Feds","Reward":12000}
)");
    write(journals, "Journal.2024-01-02T120000.01.log", session("Main", 1, "MAIN"));

    journal_service service(history_config::standard(root.path / "history"));
    started_feed f(service, "cmdr", journals);
    const auto h = f.feed->history();
    const auto missions = mission_log_projection::recent(h, 10);
    REQUIRE(missions.size() == 1);
    const auto& m = missions.front();
    CHECK(m.state == mission_log_projection::status::completed);
    CHECK(m.origin_system == "Sol");
    CHECK(m.origin_station == "Abraham Lincoln");
    CHECK(m.destination_system == "Achenar");
    CHECK(m.reward == 12'000);  // what was paid, not what was promised
    CHECK(mission_log_projection::earned(h) == 12'000);
}

TEST_CASE("a newest journal that never loaded a commander does not blank the state") {
    temp_dir dir("menu_only");
    write(dir.path, "Journal.2024-01-01T120000.01.log", session("Old", 1));
    write(dir.path, "Journal.2024-01-02T120000.01.log", session("Main", 42));
    const auto newest = write(dir.path, "Journal.2024-01-03T120000.01.log",
                              R"({"timestamp":"2024-01-03T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-03T12:02:00Z","event":"Shutdown"}
)");

    journal_service service;
    started_feed f(service, "cmdr", dir.path);
    CHECK(f.feed->state()->name == "Main");
    CHECK(f.feed->state()->credits == 42);

    // ...and the live tail is still on the newest file.
    append(newest, R"({"timestamp":"2024-01-03T12:05:00Z","event":"Commander","FID":"F1","Name":"Next"})" "\n");
    CHECK(wait_until([&] { return f.feed->state()->name == "Next"; }));
}

TEST_CASE("engineer progress merges each shape the journal writes") {
    temp_dir dir("engineers");
    write(dir.path, "Journal.2024-01-01T120000.01.log",
          session("Main", 1) +
              R"({"timestamp":"2024-01-01T12:00:10Z","event":"EngineerProgress","Engineers":[{"Engineer":"Juri Ishmaak","EngineerID":300250,"Progress":"Unlocked","RankProgress":0,"Rank":1},{"Engineer":"The Sarge","EngineerID":300040,"Progress":"Known"}]}
{"timestamp":"2024-01-01T12:01:00Z","event":"EngineerProgress","Engineer":"Juri Ishmaak","EngineerID":300250,"Rank":2}
{"timestamp":"2024-01-01T12:02:00Z","event":"EngineerProgress","Engineer":"The Sarge","EngineerID":300040,"Progress":"Invited"}
)");

    journal_service service;
    started_feed f(service, "cmdr", dir.path);
    const auto& engineers = f.feed->state()->engineers;
    REQUIRE(engineers.contains("Juri Ishmaak"));
    CHECK(engineers.at("Juri Ishmaak").rank == 2);
    CHECK(engineers.at("Juri Ishmaak").state == "Unlocked");
    CHECK(engineers.at("The Sarge").state == "Invited");
    CHECK_FALSE(engineers.at("The Sarge").rank.has_value());
}
