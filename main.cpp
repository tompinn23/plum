#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QStyleFactory>
#include <QSysInfo>

#include <chrono>
#include <cstdio>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "main_window.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

QFont monospace_font() {
    const QStringList preferred{"IBM Plex Mono", "Cascadia Mono", "Consolas"};
    const QStringList available = QFontDatabase::families();
    for (const QString &family : preferred) {
        if (available.contains(family)) {
            QFont font(family);
            font.setPointSize(9);
            font.setStyleHint(QFont::Monospace);
            return font;
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

void dark_theme(QApplication &app) {
    // Also darkens the native Windows title bar.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, QColor(0x0d, 0x0d, 0x0d));
    p.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Base, QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::AlternateBase, QColor(0x16, 0x16, 0x16));
    p.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Button, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::ButtonText, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Highlight, QColor(0xf0, 0xb0, 0x40));
    p.setColor(QPalette::HighlightedText, QColor(0x0d, 0x0d, 0x0d));
    p.setColor(QPalette::ToolTipBase, QColor(0x16, 0x16, 0x16));
    p.setColor(QPalette::ToolTipText, QColor(0xd0, 0xd0, 0xd0));
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5a, 0x5a, 0x5a));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5a, 0x5a, 0x5a));
    app.setPalette(p);

    QFile qss(":/resources/theme.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

// A GUI app starts without a console. Output it was already given (an IDE's pipe, a redirect) is
// kept; otherwise it writes to the terminal that started it, or in a debug build, a console of its
// own. Before setup_logging: spdlog's console sink takes the handle when it is made.
void attach_console() {
#ifdef _WIN32
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
#ifdef NDEBUG
        return;
#else
        if (!AllocConsole()) return;
#endif
    }
    const HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, out);
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#endif
}

// Qt's own warnings (network, OAuth, style sheets) into the same log, with the category when Qt
// gives one.
void qt_to_spdlog(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    const std::string text = message.toStdString();
    const std::string_view category = context.category ? context.category : "default";
    const std::string_view prefix = category == "default" ? "qt" : category;
    switch (type) {
        case QtDebugMsg: spdlog::debug("{}: {}", prefix, text); break;
        case QtInfoMsg: spdlog::info("{}: {}", prefix, text); break;
        case QtWarningMsg: spdlog::warn("{}: {}", prefix, text); break;
        case QtCriticalMsg:
        case QtFatalMsg: spdlog::critical("{}: {}", prefix, text); break;
    }
}

// Diagnostics go to the console, when there is one, and to plum.log beside the history store.
// Debug and up by default; SPDLOG_LEVEL=trace (or info, warn...) changes that.
void setup_logging() {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const bool have_dir = QDir().mkpath(dir);
    const std::string file = (dir + "/plum.log").toStdString();
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    std::string file_error;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file, true));
    } catch (const spdlog::spdlog_ex &e) {
        file_error = e.what();
    }

    auto logger = std::make_shared<spdlog::logger>("plum", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);
    // Thread ids tell the UI, ingest and window-tracking threads apart.
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    spdlog::cfg::load_env_levels();
    spdlog::flush_every(std::chrono::seconds(2));

    if (!have_dir) spdlog::warn("cannot create {}", dir.toStdString());
    if (!file_error.empty()) spdlog::warn("logging to the console only: {}", file_error);
    else spdlog::info("logging to {} at level {}", file, spdlog::level::to_string_view(spdlog::get_level()));
}

void log_environment() {
    spdlog::info("Plum starting: Qt {} (built against {}), {} {}", qVersion(), QT_VERSION_STR,
                 QSysInfo::prettyProductName().toStdString(), QSysInfo::currentCpuArchitecture().toStdString());
    spdlog::info("config in {}", QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation).toStdString());
    if (const QString dir = qEnvironmentVariable("PLUM_JOURNAL_DIR"); !dir.isEmpty())
        spdlog::info("PLUM_JOURNAL_DIR={}", dir.toStdString());
    for (const auto *screen: QGuiApplication::screens())
        spdlog::debug("screen {}: {}x{} at {},{} scale {}", screen->name().toStdString(), screen->geometry().width(),
                      screen->geometry().height(), screen->geometry().x(), screen->geometry().y(),
                      screen->devicePixelRatio());
}

} // namespace

int main(int argc, char *argv[]) {
    attach_console();
    QApplication app(argc, argv);
    QApplication::setApplicationName("Plum");
    QApplication::setOrganizationName("Plum");
    setup_logging();
    qInstallMessageHandler(qt_to_spdlog);
    log_environment();

    QApplication::setFont(monospace_font());
    dark_theme(app);

    main_window window;
    window.resize(1280, 800);
    window.show();

    const int code = QApplication::exec();
    spdlog::info("Plum exiting with code {}", code);
    return code;
}
