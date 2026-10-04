#pragma once

#include <memory>
#include <vector>

#include <QMainWindow>

#include <journal/journal_service.hpp>

class Dashboard;
class QLabel;
class QStackedWidget;
class QTimer;
class Sidebar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void addDashboard(Dashboard *dashboard);

private:
    void buildMenu();
    void startJournals();
    void showState(const std::shared_ptr<const journal::game_state> &state);
    void showProgress(const journal::ingest_progress &progress);
    void refreshHistory();

    Sidebar *m_sidebar;
    QStackedWidget *m_stack;
    QLabel *m_status;
    QLabel *m_ingest;
    QTimer *m_historyTimer;
    std::vector<Dashboard *> m_dashboards;

    std::unique_ptr<journal::journal_service> m_journals;
    std::shared_ptr<journal::commander_feed> m_feed;
    std::vector<journal::subscription> m_subscriptions;
};
