// Assertion-based smoke test; build without NDEBUG.
#undef NDEBUG
#include <notify/notify.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static std::vector<notify::Event> drain(notify::Watcher& watcher) {
    std::vector<notify::Event> out;
    while (auto r = watcher.receive_for(200ms)) {
        assert(*r);
        out.push_back(r->event());
    }
    return out;
}

static bool has(const std::vector<notify::Event>& evs, notify::EventKind k, const fs::path& p) {
    for (auto& e : evs)
        for (auto& q : e.paths)
            if (e.kind == k && q.filename() == p.filename()) return true;
    return false;
}

int main() {
    fs::path dir = fs::temp_directory_path() / "notify_smoke";
    fs::remove_all(dir);
    fs::create_directories(dir / "outside");
    fs::create_directories(dir / "w");
    fs::path w = dir / "w";

    {
        notify::Watcher watcher;
        watcher.watch(w / "");  // trailing slash normalised

        std::ofstream(w / "a.txt") << "hi";
        std::ofstream(w / "a.txt", std::ios::app) << "more";
        fs::rename(w / "a.txt", w / "b.txt");
        fs::rename(w / "b.txt", dir / "outside" / "b.txt");
        fs::rename(dir / "outside" / "b.txt", w / "c.txt");
        fs::remove(w / "c.txt");

        auto evs = drain(watcher);
        for (auto& e : evs) {
            std::cout << notify::to_string(e.kind);
            for (auto& p : e.paths) std::cout << ' ' << p.filename();
            std::cout << '\n';
        }
        assert(has(evs, notify::EventKind::Create, "a.txt"));
        assert(has(evs, notify::EventKind::Modify, "a.txt"));
        assert(has(evs, notify::EventKind::Rename, "b.txt"));
#ifdef _WIN32
        // ReadDirectoryChangesW reports moves across directories as remove/add.
        assert(has(evs, notify::EventKind::Remove, "b.txt"));
        assert(has(evs, notify::EventKind::Create, "c.txt"));
#else
        assert(has(evs, notify::EventKind::RenameFrom, "b.txt"));
        assert(has(evs, notify::EventKind::RenameTo, "c.txt"));
#endif
        assert(has(evs, notify::EventKind::Remove, "c.txt"));

        // unwatch -> no more events; double unwatch -> error
        watcher.unwatch("./" + fs::relative(w).string());
        std::ofstream(w / "ignored.txt") << "x";
        assert(drain(watcher).empty());
        std::error_code ec;
        watcher.unwatch(w, ec);
        assert(ec);

        // single-file watch
        std::ofstream(dir / "outside" / "f.txt") << "1";
        watcher.watch(dir / "outside" / "f.txt");
        std::ofstream(dir / "outside" / "f.txt", std::ios::app) << "2";
        auto fevs = drain(watcher);
        assert(has(fevs, notify::EventKind::Modify, "f.txt"));

        // nonexistent path -> error code
        watcher.watch(dir / "nope", ec);
        assert(ec);
        std::cout << "missing path error: " << ec.message() << '\n';
    }

    // Moving a Watcher keeps delivering into the same queue; blocking receive() works.
    {
        notify::Watcher original;
        original.watch(w);
        notify::Watcher moved = std::move(original);
        std::ofstream(w / "moved.txt") << "x";
        notify::Result r = moved.receive();
        assert(r && r.event().paths.at(0).filename() == "moved.txt");
        (void)moved.receive_for(0ms);  // zero timeout = non-blocking poll
    }

    fs::remove_all(dir);
    std::cout << "ALL OK\n";
}
