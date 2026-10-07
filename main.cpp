#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QStyleHints>
#include <QStyleFactory>

#include <cstdio>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "src/include/main_window.hpp"

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

// Qt's own warnings (network, OAuth, style sheets) into the same log.
void qt_to_spdlog(QtMsgType type, const QMessageLogContext &, const QString &message) {
    const std::string text = message.toStdString();
    switch (type) {
        case QtDebugMsg: spdlog::debug("qt: {}", text); break;
        case QtInfoMsg: spdlog::info("qt: {}", text); break;
        case QtWarningMsg: spdlog::warn("qt: {}", text); break;
        case QtCriticalMsg:
        case QtFatalMsg: spdlog::error("qt: {}", text); break;
    }
}

// Diagnostics go to the console, when there is one, and to plum.log beside the history store.
void setup_logging() {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(dir)) {

    }
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((dir + "/plum.log").toStdString(), true);
        file_sink->set_level(spdlog::level::info);

        auto logger = std::make_shared<spdlog::logger>("plum", spdlog::sinks_init_list{console_sink, file_sink});
        logger->set_level(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));
    } catch (const spdlog::spdlog_ex &) {
    }
}

} // namespace

int main(int argc, char *argv[]) {
    attach_console();
    QApplication app(argc, argv);
    QApplication::setApplicationName("Plum");
    QApplication::setOrganizationName("Plum");
    setup_logging();
    qInstallMessageHandler(qt_to_spdlog);

    QApplication::setFont(monospace_font());
    dark_theme(app);

    main_window window;
    window.resize(1280, 800);
    window.show();

    return QApplication::exec();
}
