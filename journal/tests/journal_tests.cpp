#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "journal/journal_service.hpp"
#include "journal/projections.hpp"
#include "game_process.hpp"
#include "journal_files.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace journal;
using namespace std::chrono_literals;

namespace {
    // For tests that are not about the game: whether Elite happens to be running here is irrelevant.
    const game_probe no_game = [](const fs::path &) { return false; };

    // A probe the test switches on and off.
    struct fake_game {
        std::shared_ptr<std::atomic<bool> > up = std::make_shared<std::atomic<bool> >(false);

        [[nodiscard]] game_probe probe() const {
            return [up = up](const fs::path &) { return up->load(); };
        }
    };

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

    fs::path write(const fs::path &dir, std::string_view name, std::string_view content) {
        fs::create_directories(dir);
        const fs::path file = dir / name;
        std::ofstream(file, std::ios::binary) << content;
        return file;
    }

    void append(const fs::path &file, std::string_view content) {
        std::ofstream(file, std::ios::binary | std::ios::app) << content;
    }

    template<class Pred>
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
        std::shared_ptr<std::atomic<bool> > live = std::make_shared<std::atomic<bool> >(false);
        std::shared_ptr<ingest_progress> last = std::make_shared<ingest_progress>();
        subscription progress;

        started_feed(journal_service &service, const std::string &id, const fs::path &dir) {
            feed = service.open(id, dir);
            progress = feed->on_progress([live = live, last = last](const ingest_progress &p) {
                *last = p;
                if (p.caught_up()) *live = true;
            });
            feed->start();
            REQUIRE(wait_until([&] { return live->load(); }));
        }
    };
} // namespace

// ── files ───────────────────────────────────────────────────────────────────

TEST_CASE (

"journal files order by the time and part in their names, not as text"
)
 {
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

TEST_CASE (

"read_lines reports line offsets and holds back a partial line"
)
 {
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

TEST_CASE (

"statements reject unknown and unbound parameters rather than writing NULL"
)
 {
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

TEST_CASE (

"a feed replays history oldest first, catches up on the newest, then tails it"
)
 {
    temp_dir dir("feed");
    write(dir.path, "Journal.230101010101.01.log", session("Kirk", 100));
    write(dir.path, "Journal.2024-01-02T030405.01.log", session("Picard", 1'000));
    write(dir.path, "Journal.2024-01-03T030405.9.log", session("Sisko", 2'000));
    const auto newest = write(dir.path, "Journal.2024-01-03T030405.10.log", session("Janeway", 5'000));

    journal_service service(std::nullopt, no_game);
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

TEST_CASE (

"a journal started while live is counted and reported"
)
 {
    temp_dir dir("live_count");
    write(dir.path, "Journal.2024-01-01T120000.01.log", session("Main", 1'000, "MAIN"));
    write(dir.path, "Journal.2024-01-02T120000.01.log", session("Main", 2'000, "MAIN"));

    journal_service service(std::nullopt, no_game);
    started_feed f(service, "cmdr", dir.path);
    CHECK(f.last->files_total == 2);

    // The game is launched: it starts a new journal.
    write(dir.path, "Journal.2024-01-03T120000.01.log", session("Main", 3'000, "MAIN"));
    REQUIRE(wait_until([&] { return f.feed->state()->credits == 3'000; }));
    REQUIRE(wait_until([&] { return f.last->files_total == 3; }));
    CHECK(f.last->caught_up());
    CHECK(f.last->files_done == 3);
}

TEST_CASE (

"an incomplete trailing line is never emitted"
)
 {
    temp_dir dir("partial");
    write(dir.path, "Journal.2024-01-02T030405.01.log",
          session("Picard", 1'000) + R"({"timestamp":"2024-01-02T03:06:00Z","eve)");

    journal_service service(std::nullopt, no_game);
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

TEST_CASE (

"feeds in different directories share the one ingest thread and both go live"
)
 {
    temp_dir a("multiplex_a");
    temp_dir b("multiplex_b");
    const auto a_file = write(a.path, "Journal.2024-01-02T030405.01.log", session("Kirk", 100, "FA"));
    const auto b_file = write(b.path, "Journal.2024-01-02T030405.01.log", session("Picard", 200, "FB"));
    for (int i = 0; i < 20; ++i) {
        write(a.path, std::format("Journal.2023-01-{:02}T000000.01.log", i + 1), session("Kirk", i, "FA"));
        write(b.path, std::format("Journal.2023-01-{:02}T000000.01.log", i + 1), session("Picard", i, "FB"));
    }

    journal_service service(std::nullopt, no_game);
    started_feed fa(service, "a", a.path);
    started_feed fb(service, "b", b.path);
    CHECK(fa.feed->state()->credits == 100);
    CHECK(fb.feed->state()->credits == 200);

    append(a_file, R"({"timestamp":"2024-01-02T04:00:00Z","event":"RedeemVoucher","Amount":1})" "\n");
    append(b_file, R"({"timestamp":"2024-01-02T04:00:00Z","event":"RedeemVoucher","Amount":2})" "\n");
    CHECK(wait_until([&] { return fa.feed->state()->credits == 101 && fb.feed->state()->credits == 202; }));
}

// ── history ─────────────────────────────────────────────────────────────────

TEST_CASE (

"history persists, and a second launch resumes without re-reading"
)
 {
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
        journal_service service(history_config::standard(store), no_game);
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
        journal_service service(history_config::standard(store), no_game);
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
        explicit jump_labels(int version) : version_(version) {
        }

        [[nodiscard]] std::string_view name() const override { return "jump_labels"; }
        [[nodiscard]] int version() const override { return version_; }
        [[nodiscard]] std::set<std::string> events() const override { return {"FSDJump"}; }

        void schema(sql::database &db) override {
            db.exec("CREATE TABLE IF NOT EXISTS jump_labels (seq INTEGER PRIMARY KEY, label TEXT NOT NULL)");
        }

        void reset(sql::database &db) override { db.exec("DROP TABLE IF EXISTS jump_labels"); }

        void apply(sql::database &db, std::int64_t seq, const game_event &e) override {
            db.prepare("INSERT INTO jump_labels(seq, label) VALUES (:seq, :label)")
                    .bind("seq", seq)
                    .bind("label", std::format("v{}:{}", version_, e.str("StarSystem")))
                    .run();
        }

    private:
        int version_;
    };

    std::vector<std::string> labels(const history &h) {
        return h.query([](sql::database &db) {
            std::vector<std::string> out;
            auto s = db.prepare("SELECT label FROM jump_labels ORDER BY seq");
            while (s.step()) out.push_back(s.text(0));
            return out;
        });
    }
} // namespace

TEST_CASE (

"a projection whose version changed is rebuilt from the ledger, not the journals"
)
 {
    temp_dir root("rebuild");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    write(journals, "Journal.2024-01-01T120000.01.log",
          R"({"timestamp":"2024-01-01T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-01T12:00:01Z","event":"LoadGame","Commander":"Janeway","Credits":1000,"GameMode":"Solo"}
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
        journal_service service(config(1), no_game);
        started_feed f(service, "cmdr", journals);
        CHECK(labels(f.feed->history()) == std::vector<std::string>{"v1:Sol", "v1:Achenar"});
    }

    // The rebuild must not need the journals; the catalog remembers whose directory this was.
    fs::remove_all(journals);
    fs::create_directories(journals);
    {
        journal_service service(config(2), no_game);
        started_feed f(service, "cmdr", journals);
        CHECK(labels(f.feed->history()) == std::vector<std::string>{"v2:Sol", "v2:Achenar"});
    }
}

TEST_CASE (

"every commander in a directory gets their own history, and the feed follows who is playing"
)
 {
    temp_dir root("binding");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    const std::string sell = R"({"timestamp":"2024-01-01T12:20:00Z","event":"MarketSell","Type":"gold","Count":1,"TotalSale":100}
)";
    write(journals, "Journal.2024-01-01T120000.01.log", session("Alt", 5, "ALT") + sell);
    write(journals, "Journal.2024-01-02T120000.01.log", session("Main", 10, "MAIN") + sell);
    write(journals, "Journal.2024-01-03T120000.01.log", session("Main", 20, "MAIN"));

    {
        journal_service service(history_config::standard(store), no_game);
        started_feed f(service, "cmdr", journals);
        CHECK(ledger_projection::net(f.feed->history()) == 100);  // Main is playing: Main's sale
        CHECK(f.feed->history().file_count() == 1);              // the newest is still open
        CHECK(ledger_projection::net(service.history("ALT")) == 100);
        CHECK(ledger_projection::net(service.history("MAIN")) == 100);

        auto commanders = service.commanders();
        std::ranges::sort(commanders, {}, &commander_info::key);
        REQUIRE(commanders.size() == 2);
        CHECK(commanders[0].key == "ALT");
        CHECK(commanders[1].name == "Main");
    }

    // The alt plays again, most recently of all: their store gains it, Main's is untouched.
    write(journals, "Journal.2024-01-04T120000.01.log", session("Alt", 7, "ALT") + sell);
    {
        journal_service service(history_config::standard(store), no_game);
        started_feed f(service, "cmdr", journals);
        CHECK(ledger_projection::net(f.feed->history()) == 200);
        CHECK(f.feed->state()->name == "Alt");
        CHECK(f.feed->state()->credits == 107);
    }
    CHECK(ledger_projection::net(history(store / "MAIN.db")) == 100);
}

TEST_CASE (

"a commander switching while live is recorded into their own store"
)
 {
    temp_dir root("switch");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 10, "MAIN"));

    journal_service service(history_config::standard(store), no_game);
    started_feed f(service, "cmdr", journals);
    CHECK(f.feed->history().enabled());

    // The game creates the journal at the main menu; the commander appears once they log in.
    std::atomic<bool> header{false};
    auto headers = f.feed->subscribe({"Fileheader"}, [&](const game_event&, phase) { header = true; });
    const auto next = write(journals, "Journal.2024-01-02T120000.01.log",
                            R"({"timestamp":"2024-01-02T12:00:00Z","event":"Fileheader","gameversion":"4.0"})" "\n");
    REQUIRE(wait_until([&] { return header.load(); }));
    append(next, R"({"timestamp":"2024-01-02T12:00:01Z","event":"Commander","FID":"ALT","Name":"Alt"}
{"timestamp":"2024-01-02T12:00:02Z","event":"LoadGame","Commander":"Alt","FID":"ALT","Credits":5,"GameMode":"Solo"}
{"timestamp":"2024-01-02T12:20:00Z","event":"MarketSell","Type":"gold","Count":1,"TotalSale":100}
)");

    REQUIRE(wait_until([&] { return f.feed->state()->name == "Alt"; }));
    REQUIRE(wait_until([&] { return ledger_projection::net(service.history("ALT")) == 100; }));
    CHECK(ledger_projection::net(f.feed->history()) == 100);
    // The Fileheader written before anyone logged in went with the rest of the journal.
    CHECK(service.history("ALT").event_count() == 4);
    CHECK(ledger_projection::net(service.history("MAIN")) == 0);
}

TEST_CASE (

"two directories played by the same commander share one store"
)
 {
    temp_dir root("shared");
    const fs::path store = root.path / "history";
    const std::string sell = R"({"timestamp":"2024-01-01T12:20:00Z","event":"MarketSell","Type":"gold","Count":1,"TotalSale":100}
)";
    write(root.path / "a", "Journal.2024-01-01T120000.01.log", session("Main", 10, "MAIN") + sell);
    write(root.path / "b", "Journal.2024-01-02T120000.01.log", session("Main", 20, "MAIN") + sell);
    // The same journal backed up into both is stored once.
    write(root.path / "a", "Journal.2023-06-01T120000.01.log", session("Main", 1, "MAIN") + sell);
    write(root.path / "b", "Journal.2023-06-01T120000.01.log", session("Main", 1, "MAIN") + sell);

    journal_service service(history_config::standard(store), no_game);
    started_feed a(service, "a", root.path / "a");
    started_feed b(service, "b", root.path / "b");
    CHECK(ledger_projection::net(a.feed->history()) == 300);
    CHECK(ledger_projection::net(b.feed->history()) == 300);
}

namespace {
    std::string sale(std::string_view time, int amount) {
        return std::format(R"({{"timestamp":"{}","event":"MarketSell","Type":"gold","Count":1,"TotalSale":{}}})"
                           "\n",
                           time, amount);
    }

    // The sales in a history, newest first. LoadGame rows, which only state the balance, are left out.
    std::vector<std::int64_t> sales(const history &h) {
        std::vector<std::int64_t> out;
        for (const auto &e: ledger_projection::recent(h, 100))
            if (e.delta != 0) out.push_back(e.delta);
        return out;
    }

    std::string started_session(std::string_view time) {
        return std::format(R"({{"timestamp":"{0}","event":"Fileheader","gameversion":"4.0"}}
{{"timestamp":"{0}","event":"Commander","FID":"MAIN","Name":"Main"}}
{{"timestamp":"{0}","event":"LoadGame","Commander":"Main","FID":"MAIN","Credits":0,"GameMode":"Solo"}}
)",
                           time);
    }
} // namespace

TEST_CASE (

"journals stored out of order are put back in game-time order"
)
 {
    temp_dir root("ordering_history");
    const fs::path store = root.path / "history";
    write(root.path / "new", "Journal.2024-03-01T120000.01.log",
          started_session("2024-03-01T12:00:00Z") + sale("2024-03-01T12:10:00Z", 300));
    write(root.path / "new", "Journal.2024-03-02T120000.01.log", started_session("2024-03-02T12:00:00Z"));

    journal_service service(history_config::standard(store), no_game);
    started_feed newer(service, "new", root.path / "new");

    // An older directory turns up afterwards: its journals are stored after newer ones.
    write(root.path / "old", "Journal.2024-01-01T120000.01.log",
          started_session("2024-01-01T12:00:00Z") + sale("2024-01-01T12:10:00Z", 100));
    write(root.path / "old", "Journal.2024-02-01T120000.01.log",
          started_session("2024-02-01T12:00:00Z") + sale("2024-02-01T12:10:00Z", 200));
    started_feed older(service, "old", root.path / "old");

    CHECK(sales(service.history("MAIN")) == std::vector<std::int64_t>{300, 200, 100});  // newest first
    CHECK(ledger_projection::net(service.history("MAIN")) == 600);
}

TEST_CASE (

"a journal is ordered by its own header, not the clock that named it"
)
 {
    temp_dir root("ordering_header");
    const fs::path store = root.path / "history";
    // Named on a machine whose clock ran a day ahead, but written before the other.
    write(root.path / "a", "Journal.2024-01-03T000000.01.log",
          started_session("2024-01-01T12:00:00Z") + sale("2024-01-01T12:10:00Z", 1));
    write(root.path / "b", "Journal.2024-01-02T000000.01.log",
          started_session("2024-01-02T12:00:00Z") + sale("2024-01-02T12:10:00Z", 2));
    write(root.path / "b", "Journal.2024-01-09T000000.01.log", started_session("2024-01-09T12:00:00Z"));
    write(root.path / "a", "Journal.2024-01-09T000000.02.log", started_session("2024-01-09T12:00:00Z"));

    journal_service service(history_config::standard(store), no_game);
    started_feed a(service, "a", root.path / "a");
    started_feed b(service, "b", root.path / "b");

    CHECK(sales(service.history("MAIN")) == std::vector<std::int64_t>{2, 1});
}

TEST_CASE (

"a journal from before FIDs joins the history of the FID that used its name"
)
 {
    temp_dir root("legacy");
    const fs::path journals = root.path / "journals";
    const fs::path store = root.path / "history";
    write(journals, "Journal.161201120000.01.log",
          R"({"timestamp":"2016-12-01T12:00:00Z","event":"Fileheader","gameversion":"2.2"}
{"timestamp":"2016-12-01T12:00:01Z","event":"LoadGame","Commander":"Main","Credits":1,"GameMode":"Solo"}
)" + sale("2016-12-01T12:10:00Z", 50));
    write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 10, "MAIN"));

    journal_service service(history_config::standard(store), no_game);
    started_feed f(service, "cmdr", journals);
    CHECK(ledger_projection::net(service.history("MAIN")) == 50);
    CHECK_FALSE(fs::exists(store / "cmdr_Main.db"));
}

TEST_CASE (

"the ledger counts voucher income once, organic sales in full, and carrier transfers"
)
 {
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

    journal_service service(history_config::standard(root.path / "history"), no_game);
    started_feed f(service, "cmdr", journals);
    const auto h = f.feed->history();

    CHECK(ledger_projection::net(h) == 8'000 + 7'000 - 500);
    const auto entries = ledger_projection::recent(h, 10);
    const auto transfer = std::ranges::find(entries, std::string("CarrierBankTransfer"), &ledger_projection::entry::kind);
    REQUIRE(transfer != entries.end());
    CHECK(transfer->balance == 15'500);
}

TEST_CASE (

"live credits follow the events mun's parser missed"
)
 {
    temp_dir dir("credits");
    write(dir.path, "Journal.2024-01-02T030405.01.log",
          session("Main", 1'000) +
              R"({"timestamp":"2024-01-02T03:05:00Z","event":"MultiSellExplorationData","TotalEarnings":500}
{"timestamp":"2024-01-02T03:06:00Z","event":"PayFines","Amount":100}
{"timestamp":"2024-01-02T03:07:00Z","event":"SellOrganicData","BioData":[{"Value":10,"Bonus":5}]}
{"timestamp":"2024-01-02T03:08:00Z","event":"RepairAll","Cost":15}
)");

    journal_service service(std::nullopt, no_game);
    started_feed f(service, "cmdr", dir.path);
    CHECK(f.feed->state()->credits == 1'000 + 500 - 100 + 15 - 15);
}

TEST_CASE (

"logging in again in the same system is not a new visit"
)
 {
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

    journal_service service(history_config::standard(root.path / "history"), no_game);
    started_feed f(service, "cmdr", journals);
    const auto sol = visited_system_projection::by_name(f.feed->history(), "Sol");
    REQUIRE(sol);
    CHECK(sol->visits == 2);  // the first login, then the jump back; not the two later logins
}

TEST_CASE (

"a mission records where it was taken, from the ledger"
)
 {
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

    journal_service service(history_config::standard(root.path / "history"), no_game);
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

TEST_CASE (

"a newest journal that never loaded a commander does not blank the state"
)
 {
    temp_dir dir("menu_only");
    write(dir.path, "Journal.2024-01-01T120000.01.log", session("Old", 1));
    write(dir.path, "Journal.2024-01-02T120000.01.log", session("Main", 42));
    const auto newest = write(dir.path, "Journal.2024-01-03T120000.01.log",
                              R"({"timestamp":"2024-01-03T12:00:00Z","event":"Fileheader","gameversion":"4.0"}
{"timestamp":"2024-01-03T12:02:00Z","event":"Shutdown"}
)");

    journal_service service(std::nullopt, no_game);
    started_feed f(service, "cmdr", dir.path);
    CHECK(f.feed->state()->name == "Main");
    CHECK(f.feed->state()->credits == 42);

    // ...and the live tail is still on the newest file.
    append(newest, R"({"timestamp":"2024-01-03T12:05:00Z","event":"Commander","FID":"F1","Name":"Next"})" "\n");
    CHECK(wait_until([&] { return f.feed->state()->name == "Next"; }));
}

TEST_CASE (

"engineer progress merges each shape the journal writes"
)
 {
    temp_dir dir("engineers");
    write(dir.path, "Journal.2024-01-01T120000.01.log",
          session("Main", 1) +
              R"({"timestamp":"2024-01-01T12:00:10Z","event":"EngineerProgress","Engineers":[{"Engineer":"Juri Ishmaak","EngineerID":300250,"Progress":"Unlocked","RankProgress":0,"Rank":1},{"Engineer":"The Sarge","EngineerID":300040,"Progress":"Known"}]}
{"timestamp":"2024-01-01T12:01:00Z","event":"EngineerProgress","Engineer":"Juri Ishmaak","EngineerID":300250,"Rank":2}
{"timestamp":"2024-01-01T12:02:00Z","event":"EngineerProgress","Engineer":"The Sarge","EngineerID":300040,"Progress":"Invited"}
)");

    journal_service service(std::nullopt, no_game);
    started_feed f(service, "cmdr", dir.path);
    const auto& engineers = f.feed->state()->engineers;
    REQUIRE(engineers.contains("Juri Ishmaak"));
    CHECK(engineers.at("Juri Ishmaak").rank == 2);
    CHECK(engineers.at("Juri Ishmaak").state == "Unlocked");
    CHECK(engineers.at("The Sarge").state == "Invited");
    CHECK_FALSE(engineers.at("The Sarge").rank.has_value());
}

// ── game process ────────────────────────────────────────────────────────────

TEST_CASE (

"only the game client counts as the game"
)
 {
    CHECK(process::is_game_image("EliteDangerous64.exe"));
    CHECK(process::is_game_image("elitedangerous64.EXE"));
    CHECK_FALSE(process::is_game_image("EDLaunch.exe"));
    CHECK_FALSE(process::is_game_image("EliteDangerous64.exe.bak"));
}

TEST_CASE (

"a directory and a file made in it have the same owner, and this process is not the game"
)
 {
    temp_dir dir("owner");
    const auto file = write(dir.path, "x.txt", "x");
    const auto owner = process::owner_of(dir.path);
    REQUIRE(owner);
    CHECK(process::owner_of(file) == owner);
    CHECK_FALSE(process::owner_of(dir.path / "missing"));

#ifdef _WIN32
    const auto self = static_cast<std::uint32_t>(_getpid());
#else
    const auto self = static_cast<std::uint32_t>(getpid());
#endif
    CHECK_FALSE(process::is_game(self));
    for (const auto& g : process::running_games()) CHECK(g.pid != self);
}

namespace {
    // Collects what one subscriber is sent, on whatever thread sends it.
    struct received {
        std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
        std::shared_ptr<std::vector<game_event> > events = std::make_shared<std::vector<game_event> >();

        [[nodiscard]] event_listener listener() const {
            return [mutex = mutex, events = events](const game_event &e, phase) {
                std::lock_guard lock(*mutex);
                events->push_back(e);
            };
        }

        [[nodiscard]] std::vector<game_event> named(std::string_view name) const {
            std::lock_guard lock(*mutex);
            std::vector<game_event> out;
            for (const auto &e: *events)
                if (e.name() == name) out.push_back(e);
            return out;
        }
    };
} // namespace

TEST_CASE (

"a feed that goes live while the game runs sends StartUp from its state"
)
 {
    temp_dir root("startup");
    const fs::path journals = root.path / "journals";
    write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 1'000, "MAIN") +
              R"({"timestamp":"2024-01-01T12:00:02Z","event":"Location","StarSystem":"Sol","SystemAddress":10477373803,"StarPos":[0.0,0.0,0.0],"Docked":true,"StationName":"Abraham Lincoln"}
)");

    fake_game game;
    game.up->store(true);
    journal_service service(history_config::standard(root.path / "history"), game.probe());

    received early;
    auto feed = service.open("cmdr", journals);
    auto sub = feed->subscribe({"StartUp"}, early.listener());
    std::atomic<bool> live{false};
    auto progress = feed->on_progress([&](const ingest_progress& p) { if (p.caught_up()) live = true; });
    feed->start();
    REQUIRE(wait_until([&] { return live.load(); }));
    CHECK(feed->game_running());

    REQUIRE(wait_until([&] { return !early.named("StartUp").empty(); }));
    const auto startup = early.named("StartUp").front();
    CHECK(startup.str("Commander") == "Main");
    CHECK(startup.str("FID") == "MAIN");
    CHECK(startup.num("Credits") == 1'000);
    CHECK(startup.str("StarSystem") == "Sol");
    CHECK(startup.flag("Docked"));
    CHECK(startup.str("StationName") == "Abraham Lincoln");
    CHECK(startup.time().has_value());

    // A subscriber arriving now gets one of its own; the first is not sent another.
    received late;
    auto late_sub = feed->subscribe({}, late.listener());
    REQUIRE(wait_until([&] { return late.named("StartUp").size() == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(early.named("StartUp").size() == 1);

    // It is the session as it stands, never history.
    CHECK(feed->history().query([](sql::database& db) {
        auto s = db.prepare("SELECT COUNT(*) FROM event WHERE name = 'StartUp'");
        return s.step() ? s.integer(0) : -1;
    }) == 0);
}

TEST_CASE (

"no StartUp while the game is not running, nor when it is launched while watched"
)
 {
    temp_dir root("no_startup");
    const fs::path journals = root.path / "journals";
    write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 1'000, "MAIN"));

    fake_game game;
    journal_service service(std::nullopt, game.probe());
    received seen;
    auto feed = service.open("cmdr", journals);
    auto sub = feed->subscribe({"StartUp"}, seen.listener());
    std::atomic<bool> live{false};
    auto progress = feed->on_progress([&](const ingest_progress& p) { if (p.caught_up()) live = true; });
    feed->start();
    REQUIRE(wait_until([&] { return live.load(); }));
    CHECK_FALSE(feed->game_running());

    received late;
    auto late_sub = feed->subscribe({"StartUp"}, late.listener());

    // The game starts: noticed within the probe interval, but the journal will restate the session.
    game.up->store(true);
    REQUIRE(wait_until([&] { return feed->game_running(); }, 15s));
    std::this_thread::sleep_for(200ms);
    CHECK(seen.named("StartUp").empty());
    CHECK(late.named("StartUp").empty());

    game.up->store(false);
    CHECK(wait_until([&] { return !feed->game_running(); }, 15s));
}

TEST_CASE (

"a game that stops without writing Shutdown gets one made for it"
)
 {
    temp_dir root("crash");
    const fs::path journals = root.path / "journals";
    const auto journal = write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 1'000, "MAIN"));

    fake_game game;
    game.up->store(true);
    journal_service service(history_config::standard(root.path / "history"), game.probe());
    started_feed f(service, "cmdr", journals);
    received seen;
    auto sub = f.feed->subscribe({"Shutdown"}, seen.listener());

    game.up->store(false);  // crashed: the journal just stops
    REQUIRE(wait_until([&] { return !seen.named("Shutdown").empty(); }, 15s));
    CHECK(seen.named("Shutdown").front().time().has_value());
    CHECK(f.feed->history().query([](sql::database& db) {
        auto s = db.prepare("SELECT COUNT(*) FROM event WHERE name = 'Shutdown'");
        return s.step() ? s.integer(0) : -1;
    }) == 0);

    // Only once: stopping again with nothing new in between sends nothing.
    game.up->store(true);
    REQUIRE(wait_until([&] { return f.feed->game_running(); }, 15s));
    game.up->store(false);
    REQUIRE(wait_until([&] { return !f.feed->game_running(); }, 15s));
    std::this_thread::sleep_for(200ms);
    CHECK(seen.named("Shutdown").size() == 1);
}

TEST_CASE (

"a game that writes Shutdown as it exits gets no second one"
)
 {
    temp_dir root("clean_exit");
    const fs::path journals = root.path / "journals";
    const auto journal = write(journals, "Journal.2024-01-01T120000.01.log", session("Main", 1'000, "MAIN"));

    fake_game game;
    game.up->store(true);
    journal_service service(std::nullopt, game.probe());
    started_feed f(service, "cmdr", journals);
    received seen;
    auto sub = f.feed->subscribe({"Shutdown"}, seen.listener());

    // Written and gone at once, as the game does: the exit can be noticed before the write is.
    append(journal, R"({"timestamp":"2024-01-01T13:00:00Z","event":"Shutdown"})" "\n");
    game.up->store(false);
    REQUIRE(wait_until([&] { return !f.feed->game_running(); }, 15s));
    REQUIRE(wait_until([&] { return !seen.named("Shutdown").empty(); }));
    std::this_thread::sleep_for(300ms);
    REQUIRE(seen.named("Shutdown").size() == 1);
    CHECK(seen.named("Shutdown").front().str("timestamp") == "2024-01-01T13:00:00Z");  // the game's own
}
