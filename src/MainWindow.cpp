#include "MainWindow.h"

#include <filesystem>

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>

#include <spdlog/spdlog.h>

#include "Dashboard.h"
#include "Sidebar.h"
#include "dashboards/CommanderDashboard.h"
#include "dashboards/ShipDashboard.h"

namespace {

// PLUM_JOURNAL_DIR overrides the default location, e.g. for a Proton prefix.
QString journalDirectory() {
    const QString overridden = qEnvironmentVariable("PLUM_JOURNAL_DIR");
    if (!overridden.isEmpty()) return overridden;
    return QDir::homePath() + "/Saved Games/Frontier Developments/Elite Dangerous";
}

std::filesystem::path toPath(const QString &path) {
    return std::filesystem::path(path.toStdWString());
}

}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_sidebar(new Sidebar), m_stack(new QStackedWidget), m_status(new QLabel),
      m_ingest(new QLabel), m_historyTimer(new QTimer(this)) {
    setWindowTitle("Plum");
    setMinimumSize(1100, 650);
    buildMenu();

    // Status bar sits under the dashboards only, so the sidebar runs full height.
    auto *statusBar = new QWidget;
    statusBar->setObjectName("statusBar");
    statusBar->setAttribute(Qt::WA_StyledBackground);
    statusBar->setFixedHeight(28);
    m_status->setObjectName("statusText");
    m_ingest->setObjectName("statusText");
    auto *statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(8, 0, 8, 0);
    statusLayout->addWidget(m_ingest);
    statusLayout->addStretch();
    statusLayout->addWidget(m_status);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_stack, 1);
    rightLayout->addWidget(statusBar);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_sidebar);
    layout->addWidget(right, 1);
    setCentralWidget(central);

    connect(m_sidebar, &Sidebar::currentIndexChanged, m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_stack, &QStackedWidget::currentChanged, this, [this](int index) {
        if (auto *dashboard = qobject_cast<Dashboard *>(m_stack->widget(index)))
            m_status->setText(dashboard->title());
    });

    addDashboard(new CommanderDashboard);
    addDashboard(new ShipDashboard);

    m_sidebar->setCurrentIndex(0);
    m_stack->setCurrentIndex(0);
    m_status->setText(m_dashboards.front()->title());

    // History changes with every recorded live event; re-reading a few counts now and then is
    // cheaper than doing it per event.
    m_historyTimer->setInterval(10'000);
    connect(m_historyTimer, &QTimer::timeout, this, &MainWindow::refreshHistory);

    startJournals();
}

// The service is stopped first, so its thread cannot post into a window being torn down.
MainWindow::~MainWindow() {
    m_subscriptions.clear();
    m_journals.reset();
}

void MainWindow::addDashboard(Dashboard *dashboard) {
    m_dashboards.push_back(dashboard);
    m_stack->addWidget(dashboard);
    m_sidebar->addEntry(dashboard->title());
}

void MainWindow::startJournals() {
    const QString directory = journalDirectory();
    const QString historyDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/history";

    try {
        m_journals = std::make_unique<journal::journal_service>(
            journal::history_config::standard(toPath(historyDirectory)));
        m_feed = m_journals->open("default", toPath(directory));
    } catch (const std::exception &e) {
        spdlog::error("cannot watch journals in {}: {}", directory.toStdString(), e.what());
        m_ingest->setText(QStringLiteral("journals not found: %1").arg(QDir::toNativeSeparators(directory)));
        return;
    }

    // Listeners run on the journal thread; hop to the UI thread before touching widgets.
    m_subscriptions.push_back(m_feed->on_state([this](std::shared_ptr<const journal::game_state> state) {
        QMetaObject::invokeMethod(this, [this, state] { showState(state); }, Qt::QueuedConnection);
    }));
    m_subscriptions.push_back(m_feed->on_progress([this](const journal::ingest_progress &progress) {
        QMetaObject::invokeMethod(this, [this, progress] { showProgress(progress); }, Qt::QueuedConnection);
    }));
    m_feed->start();
}

void MainWindow::showState(const std::shared_ptr<const journal::game_state> &state) {
    for (auto *dashboard : m_dashboards) dashboard->showState(*state);
}

void MainWindow::showProgress(const journal::ingest_progress &p) {
    if (p.caught_up()) {
        m_ingest->setText(QStringLiteral("live  %1 journals").arg(p.files_total));
        refreshHistory();
        m_historyTimer->start();
        return;
    }
    m_ingest->setText(QStringLiteral("%1  %2 / %3")
                          .arg(QString::fromLatin1(journal::to_string(p.stage)))
                          .arg(p.files_done)
                          .arg(p.files_total));
}

void MainWindow::refreshHistory() {
    if (!m_feed) return;
    const auto history = m_feed->history();
    for (auto *dashboard : m_dashboards) dashboard->showHistory(history);
}

void MainWindow::buildMenu() {
    auto *file = menuBar()->addMenu("File");
    file->addAction("Quit", QKeySequence("Ctrl+Q"), this, &QWidget::close);

    auto *help = menuBar()->addMenu("Help");
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "About", "Plum\n\nElite Dangerous companion built with Qt 6.");
    });
}
