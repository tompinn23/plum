#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMainWindow>

#include <journal/journal_service.hpp>

#include "client.hpp"
#include "journal_sources.hpp"
#include "notifications.hpp"

class QActionGroup;
class QLabel;
class QMenu;
class QStackedWidget;
class sidebar;
class toast_overlay;

class main_window : public QMainWindow {
    Q_OBJECT

public:
    explicit main_window(QWidget *parent = nullptr);

    ~main_window() override;

private:
    // One watched journal directory and what the UI knows about it.
    struct feed_entry {
        journal_source source;
        QString commander; // from the journals at open, or "<unknown>"
        std::shared_ptr<journal::commander_feed> feed; // null if the directory could not be watched
        std::vector<journal::subscription> subscriptions;
        std::optional<journal::ingest_progress> progress;
        // This feed's dashboards, in sidebar order. They subscribe to the feed themselves and stay
        // current while hidden, so switching feeds is only a matter of which set is shown.
        QStackedWidget *pages = nullptr;
        // The Companion API for this feed's Frontier account. Null if the build has no client id.
        std::unique_ptr<client> capi;
    };

    void build_menu();

    void edit_journal_sources();

    void start_journals();

    void apply_journal_sources(const std::vector<journal_source> &sources);

    void open_feed(const journal_source &source);

    void close_feed(std::map<std::string, feed_entry>::iterator it) const;

    [[nodiscard]] QStackedWidget *make_pages(const std::shared_ptr<journal::commander_feed> &feed);

    void set_active(const std::string &id);

    void show_page(int index);

    void show_progress(const std::string &id, const journal::ingest_progress &progress);

    void update_ingest_status();

    void update_commander_menu();

    [[nodiscard]] QString display_name(const feed_entry &entry) const;

    sidebar *m_sidebar;
    QStackedWidget *m_sets; // one page set per feed; the visible one is the active feed's
    QLabel *m_status;
    QLabel *m_ingest;
    QMenu *m_commanders = nullptr;
    QActionGroup *m_commander_group = nullptr;
    bool m_sidebar_built = false;

    // Before the pages, which post to it, and after them on the way out.
    notifications m_notes;
    std::unique_ptr<toast_overlay> m_toasts; // a window of its own, so not a child of this one

    std::unique_ptr<journal::journal_service> m_journals;
    std::map<std::string, feed_entry> m_feeds; // keyed by journal_source::id
    std::string m_active; // the feed whose pages are shown
    int m_page = 0; // the sidebar's page, kept across switches
};
