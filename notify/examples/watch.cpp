// Usage: watch <path> [<path>...]
// Prints every event until Ctrl+C, like notify's "monitor_raw" example.
#include <notify/notify.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <path>...\n";
        return 1;
    }

    notify::watcher watcher;
    for (int i = 1; i < argc; ++i) watcher.watch(argv[i]);

    for (;;) {
        notify::result r = watcher.receive();
        if (!r) {
            std::cerr << "error: " << r.error().code.message() << '\n';
            continue;
        }
        const notify::event& e = r.event();
        std::cout << notify::to_string(e.kind);
        if (e.kind == notify::event_kind::Modify && e.modify != notify::modify_kind::Any)
            std::cout << (e.modify == notify::modify_kind::Data ? "(data)" : "(metadata)");
        if (e.need_rescan) std::cout << " [rescan needed]";
        for (const auto& p : e.paths) std::cout << ' ' << p;
        std::cout << std::endl;
    }
}
