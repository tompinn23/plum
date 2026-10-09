#include "main_window.hpp"

#include <algorithm>
#include <filesystem>

#include <QActionGroup>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStandardPaths>

#include <spdlog/spdlog.h>

#include "dashboard.hpp"
#include "journal_dialog.hpp"
#include "sidebar.hpp"
#include "overlays.hpp"
#include "game_windows.hpp"

constexpr auto FRONTIER_CAPI_CLIENTID = "5758c1f5-e107-4b47-ac1f-a4c2b855acdd";

namespace {
    std::filesystem::path to_path(const QString &path) {
        return {path.toStdWString()};
    }
} // namespace

main_window::main_window(QWidget *parent)
    : QMainWindow(parent), sidebar(new ::sidebar), sets(new QStackedWidget), status(new QLabel),
      ingest(new QLabel) {
    setWindowTitle("Plum");
    setMinimumSize(1100, 650);
    build_menu();

    register_builtins(registry);

    windows = std::make_unique<game_windows>();
    overlays = std::make_unique<::overlays>(*windows);

    // Status bar sits under the dashboards only, so the sidebar runs full height.
    auto *status_bar = new QWidget;
    status_bar->setObjectName("status_bar");
    status_bar->setAttribute(Qt::WA_StyledBackground);
    status_bar->setFixedHeight(28);
    status->setObjectName("status_text");
    ingest->setObjectName("status_text");
    auto *status_layout = new QHBoxLayout(status_bar);
    status_layout->setContentsMargins(8, 0, 8, 0);
    status_layout->addWidget(ingest);
    status_layout->addStretch();
    status_layout->addWidget(status);

    auto *right = new QWidget;
    auto *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);
    right_layout->addWidget(sets, 1);
    right_layout->addWidget(status_bar);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sidebar);
    layout->addWidget(right, 1);
    setCentralWidget(central);

    connect(sidebar, &::sidebar::current_changed, this, &main_window::show_page);

    start_journals();
}

// Pages go first, taking their subscriptions with them; then the service, so its thread cannot
// post into a window being torn down.
main_window::~main_window() {
    spdlog::debug("main window closing {} feeds", feeds.size());
    for (auto &[id, entry]: feeds) delete entry.pages;
    feeds.clear();
    overlays.reset();
    windows.reset();
    journals.reset();
}

// Every feed gets the same pages in the same order, since the sidebar indexes into whichever set
// is shown. New dashboards are added to the registry, not here.
QStackedWidget *main_window::make_pages(const std::shared_ptr<journal::commander_feed> &feed) {
    auto *pages = new QStackedWidget;
    for (const auto &make: registry.dashboards()) pages->addWidget(make(feed, *overlays));

    // The first set decides the sidebar; the rest match it.
    if (!sidebar_built) {
        for (int i = 0; i < pages->count(); ++i)
            if (const auto *board = qobject_cast<dashboard *>(pages->widget(i))) sidebar->add_entry(board->title());
        sidebar->set_current(page);
        sidebar_built = true;
    }
    return pages;
}

void main_window::set_active(const std::string &id) {
    const auto it = feeds.find(id);
    if (it == feeds.end() || !it->second.pages) return;
    if (active != id) spdlog::info("[{}] showing {}", id, display_name(it->second).toStdString());
    active = id;
    sets->setCurrentWidget(it->second.pages);
    show_page(page);
    update_commander_menu();
}

// The sidebar picks the page within the active feed's set.
void main_window::show_page(const int index) {
    if (page != index) spdlog::debug("page {} selected", index);
    page = index;
    const auto it = feeds.find(active);
    if (it == feeds.end() || !it->second.pages) return;
    it->second.pages->setCurrentIndex(index);
    if (const auto *board = qobject_cast<dashboard *>(it->second.pages->currentWidget()))
        status->setText(QStringLiteral("%1  %2").arg(display_name(it->second), board->title()));
}

void main_window::start_journals() {
    const QString history_directory =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/plum/history";
    spdlog::info("starting journal service; history in {}", history_directory.toStdString());

    journal::history_config config;
    config.directory = to_path(history_directory);
    config.projections = registry.projections();
    try {
        journals = std::make_unique<journal::journal_service>(std::move(config));
    } catch (const std::exception &e) {
        spdlog::error("cannot start journal service: {}", e.what());
        ingest->setText(QStringLiteral("journal service failed: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    apply_journal_sources(load_journal_sources());
}

void main_window::edit_journal_sources() {
    std::vector<journal_source> current;
    for (const auto &[id, entry]: feeds) current.push_back(entry.source);
    // feeds is keyed by id; show them in the order they were saved instead.
    const auto saved = load_journal_sources();
    std::ranges::sort(current, {}, [&saved](const journal_source &s) {
        return std::ranges::find(saved, s.id, &journal_source::id) - saved.begin();
    });

    journal_dialog dialog(current, this);

    // Each row's Link button signs in that feed's client; the button says when it has. The
    // connections end with the dialog.
    for (const auto &[id, entry]: feeds) {
        if (!entry.capi) continue;
        const QString source_id = entry.source.id;
        dialog.set_linked(source_id, entry.capi->is_authorized());
        connect(entry.capi.get(), &client::authorized, &dialog,
                [&dialog, source_id] { dialog.set_linked(source_id, true); });
        connect(entry.capi.get(), &client::failed, &dialog,
                [&dialog](const QString &why) { QMessageBox::warning(&dialog, "Link Frontier account", why); });
    }
    connect(&dialog, &journal_dialog::link_requested, &dialog, [this, &dialog](const journal_source &source) {
        const auto it = feeds.find(source.id.toStdString());
        if (it == feeds.end() || !it->second.capi || it->second.source.directory != source.directory) {
            QMessageBox::information(&dialog, "Link Frontier account",
                                     "Save this directory first, then link it.");
        } else {
            spdlog::info("[{}] linking Frontier account", it->first);
            it->second.capi->authorize();
        }
    });

    if (dialog.exec() != QDialog::Accepted) {
        spdlog::debug("journal directories dialog cancelled");
        return;
    }

    const auto sources = dialog.sources();
    spdlog::info("journal directories saved: {}", sources.size());
    save_journal_sources(sources);
    apply_journal_sources(sources);
}

// Brings the open feeds in line with `sources`: closes the ones that went away or moved, and
// opens the ones that are new. Feeds whose directory is unchanged keep running untouched.
void main_window::apply_journal_sources(const std::vector<journal_source> &sources) {
    if (!journals) return;
    spdlog::debug("applying {} journal sources to {} open feeds", sources.size(), feeds.size());

    for (auto it = feeds.begin(); it != feeds.end();) {
        const auto wanted = std::ranges::find(sources, QString::fromStdString(it->first), &journal_source::id);
        if (wanted != sources.end() && wanted->directory == it->second.source.directory) {
            ++it;
            continue;
        }
        spdlog::info("[{}] {}", it->first, wanted == sources.end() ? "removed" : "moved");
        close_feed(it);
        it = feeds.erase(it);
    }

    for (const auto &source: sources)
        if (!feeds.contains(source.id.toStdString())) open_feed(source);

    // The active feed went away, or there was none: show the first that has pages.
    if (!feeds.contains(active)) {
        active.clear();
        for (const auto &[id, entry]: feeds) {
            if (!entry.pages) continue;
            set_active(id);
            break;
        }
        if (active.empty()) status->clear();
    }

    update_ingest_status();
    update_commander_menu();
}

// Pages are deleted now rather than later, so their subscriptions are gone before the feed is.
void main_window::close_feed(const std::map<std::string, feed_entry>::iterator it) const {
    spdlog::info("[{}] closing journals in {}", it->first, it->second.source.directory.toStdString());
    auto &entry = it->second;
    if (entry.pages) {
        sets->removeWidget(entry.pages);
        delete entry.pages;
        entry.pages = nullptr;
    }
    entry.subscriptions.clear();
    entry.capi.reset();
    entry.overlay_providers.clear();
    overlays->remove_feed(it->first);
    windows->untrack(it->first);
    journals->close(it->first);
}

/**
 * Initializes the commander data sources.
 * Everything requiring data should be registered here.
 * @param source
 */
void main_window::open_feed(const journal_source &source) {
    const std::string id = source.id.toStdString();
    auto &entry = feeds[id];
    entry.source = source;
    entry.commander = journal_commander(source.directory);
    spdlog::info("[{}] opening journals in {}; last commander there {}", id, source.directory.toStdString(),
                 entry.commander.toStdString());

    try {
        entry.feed = journals->open(id, to_path(source.directory));
    } catch (const std::exception &e) {
        spdlog::error("cannot watch journals in {}: {}", source.directory.toStdString(), e.what());
        return;
    }

    // Listeners run on the journal thread; hop to the UI thread before touching widgets.
    entry.subscriptions.push_back(entry.feed->on_progress([this, id](const journal::ingest_progress &progress) {
        QMetaObject::invokeMethod(this, [this, id, progress] { show_progress(id, progress); }, Qt::QueuedConnection);
    }));

    entry.capi = std::make_unique<client>(FRONTIER_CAPI_CLIENTID, entry.feed);
    // Its own Frontier account, signed in through the journal directories dialog.
    connect(entry.capi.get(), &client::failed, this, [id = source.id](const QString &why) {
        spdlog::warn("[{}] companion API: {}", id.toStdString(), why.toStdString());
    });
    connect(entry.capi.get(), &client::refresh_token_changed, this,
            [id = source.id](const QString &token) { save_refresh_token(id, token); });
    // Signed in before: pick the account up again without the browser.
    if (const QString token = load_refresh_token(source.id); !token.isEmpty()) {
        entry.capi->authorize(token);
    } else {
        spdlog::debug("[{}] no Frontier account linked", id);
    }

    windows->track(id, entry.feed);
    overlays->add_feed(entry.feed);

    for (const auto &make: registry.overlay_providers()) entry.overlay_providers.push_back(make(entry.feed, *overlays));

    // Built before the feed starts, so the pages hear the StartUp it sends on going live.
    entry.pages = make_pages(entry.feed);
    sets->addWidget(entry.pages);
    if (active.empty()) set_active(id);

    entry.feed->start();
    spdlog::debug("[{}] feed started with {} pages", id, entry.pages->count());
}

void main_window::show_progress(const std::string &id, const journal::ingest_progress &progress) {
    // A feed that was just closed may still deliver one last update.
    const auto it = feeds.find(id);
    if (it == feeds.end()) return;

    const bool newly_live = progress.caught_up() && !(it->second.progress && it->second.progress->caught_up());
    spdlog::trace("[{}] progress {} {}/{} ({} skipped)", id, journal::to_string(progress.stage), progress.files_done,
                  progress.files_total, progress.files_skipped);
    it->second.progress = progress;
    update_ingest_status();

    // The commander is known by now; label them by it.
    if (newly_live) {
        spdlog::info("[{}] live as {}", id, display_name(it->second).toStdString());
        update_commander_menu();
        if (id == active) show_page(page);
    }
}

// Whoever is playing, once the feed has read that far; until then, the journals' last word.
QString main_window::display_name(const feed_entry &entry) const {
    const auto state = entry.feed ? entry.feed->state() : nullptr;
    return state && state->name ? QString::fromStdString(*state->name) : entry.commander;
}

// One checkable entry per feed with pages; choosing one shows that feed's set.
void main_window::update_commander_menu() {
    if (!commanders) return;
    commanders->clear();
    delete commander_group;
    commander_group = new QActionGroup(this);
    commander_group->setExclusive(true);

    for (const auto &[id, entry]: feeds) {
        if (!entry.pages) continue;
        auto *action = commanders->addAction(display_name(entry));
        action->setToolTip(QDir::toNativeSeparators(entry.source.directory));
        action->setCheckable(true);
        action->setChecked(id == active);
        commander_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] { set_active(id); });
    }
    commanders->setEnabled(!commanders->isEmpty());
}

void main_window::update_ingest_status() {
    if (feeds.empty()) {
        ingest->setText(QStringLiteral("no journal directories  (File > Journal directories...)"));
        return;
    }

    QStringList parts;
    for (const auto &[id, entry]: feeds) {
        const QString name = display_name(entry);
        if (!entry.feed) {
            parts << QStringLiteral("%1: not found").arg(name);
        } else if (!entry.progress) {
            parts << QStringLiteral("%1: starting").arg(name);
        } else if (const auto &p = *entry.progress; p.caught_up()) {
            parts << QStringLiteral("%1: live  %2 journals").arg(name).arg(p.files_total);
        } else {
            parts << QStringLiteral("%1: %2  %3 / %4")
                    .arg(name, QString::fromLatin1(journal::to_string(p.stage)))
                    .arg(p.files_done)
                    .arg(p.files_total);
        }
    }
    ingest->setText(parts.join(QStringLiteral("   |   ")));
}

void main_window::build_menu() {
    auto *file = menuBar()->addMenu("File");
    file->addAction("Journal directories...", QKeySequence("Ctrl+,"), this, &main_window::edit_journal_sources);
    file->addSeparator();
    file->addAction("Quit", QKeySequence("Ctrl+Q"), this, &QWidget::close);

    commanders = menuBar()->addMenu("Commander");
    commanders->setEnabled(false);

    auto *help = menuBar()->addMenu("Help");
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "About", "Plum\n\nElite Dangerous companion built with Qt 6.");
    });
}
