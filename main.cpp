#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QStyleHints>
#include <QStyleFactory>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "MainWindow.h"

namespace {

QFont pickMonospaceFont() {
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

void applyDarkTheme(QApplication &app) {
    // Also darkens the native Windows title bar.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    app.setStyle(QStyleFactory::create("Fusion"));

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

// A GUI app has no console, so diagnostics go to plum.log beside the history store.
void setupLogging() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    try {
        auto logger = spdlog::basic_logger_mt("plum", (dir + "/plum.log").toStdString(), true);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
    } catch (const spdlog::spdlog_ex &) {
        // Keep the default console logger.
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Plum");
    QApplication::setOrganizationName("Plum");
    setupLogging();

    QApplication::setFont(pickMonospaceFont());
    applyDarkTheme(app);

    MainWindow window;
    window.resize(1280, 800);
    window.show();

    return QApplication::exec();
}
