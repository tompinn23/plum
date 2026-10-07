#include "journal_sources.hpp"

#include <filesystem>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include <journal/journal_service.hpp>
#include <spdlog/spdlog.h>

#include "config.hpp"

namespace {
    // [[journals]] tables, in the order the user listed them, each with its account's refresh token.
    constexpr auto journals_key = "journals";

    // %LOCALAPPDATA%/plum on Windows, ~/.config/plum on Linux.
    QString config_path() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/plum/config.toml";
    }

    // False if the file exists but cannot be read: saving over it then would lose it.
    bool open_config(config &file) {
        const QString path = config_path();
        if (!QFileInfo::exists(path) || file.load(path)) return true;
        spdlog::error("cannot read {}: {}", path.toStdString(), file.get_error().toStdString());
        return false;
    }

    void save_config(config &file) {
        const QString path = config_path();
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (file.save(path)) spdlog::debug("saved {}", path.toStdString());
    else spdlog::error("cannot save {}: {}", path.toStdString(), file.get_error().toStdString());
    }
} // namespace

// PLUM_JOURNAL_DIR overrides the default location, e.g. for a Proton prefix.
QString default_journal_directory() {
    if (const QString overridden = qEnvironmentVariable("PLUM_JOURNAL_DIR"); !overridden.isEmpty()) return overridden;
    return QDir::homePath() + "/Saved Games/Frontier Developments/Elite Dangerous";
}

QString journal_commander(const QString &directory) {
    const auto name = journal::latest_commander(std::filesystem::path(directory.toStdWString()));
    return name ? QString::fromStdString(*name) : QStringLiteral("<unknown>");
}

QString new_journal_source_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::vector<journal_source> load_journal_sources() {
    config file;
    if (!open_config(file)) return {};

    // An empty list the user saved is respected; only a missing one is seeded, and saved so the
    // seeded entry keeps its id.
    if (!file.contains(journals_key)) {
        std::vector<journal_source> seeded{{new_journal_source_id(), default_journal_directory()}};
        spdlog::info("no journal directories configured; using {}", seeded.front().directory.toStdString());
        save_journal_sources(seeded);
        return seeded;
    }

    std::vector<journal_source> sources;
    for (const auto &entry: file.value(journals_key).toList()) {
        const auto table = entry.toMap();
        journal_source source{table.value("id").toString(), table.value("directory").toString()};
        if (!source.id.isEmpty() && !source.directory.isEmpty()) {
            spdlog::debug("journal source {}: {}", source.id.toStdString(), source.directory.toStdString());
            sources.push_back(std::move(source));
        } else {
            spdlog::warn("ignoring a journal source without an id or directory in {}", config_path().toStdString());
        }
    }
    return sources;
}

// Tokens stay with their journals' ids; a removed directory's token goes with it.
void save_journal_sources(const std::vector<journal_source> &sources) {
    config file;
    if (!open_config(file)) return;

    QVariantMap tokens;
    for (const auto &entry: file.value(journals_key).toList()) {
        const auto table = entry.toMap();
        tokens.insert(table.value("id").toString(), table.value("refresh_token"));
    }

    QVariantList entries;
    for (const auto &source: sources) {
        QVariantMap table{{"id", source.id}, {"directory", source.directory}};
        if (const auto token = tokens.value(source.id); token.isValid()) table.insert("refresh_token", token);
        entries.append(table);
    }
    file.set(journals_key, entries);
    save_config(file);
}

QString load_refresh_token(const QString &id) {
    config file;
    if (!open_config(file)) return {};
    for (const auto &entry: file.value(journals_key).toList()) {
        if (const auto table = entry.toMap(); table.value("id").toString() == id)
            return table.value("refresh_token").toString();
    }
    return {};
}

void save_refresh_token(const QString &id, const QString &token) {
    config file;
    if (!open_config(file)) return;

    auto entries = file.value(journals_key).toList();
    for (auto &entry: entries) {
        auto table = entry.toMap();
        if (table.value("id").toString() != id) continue;
        spdlog::debug("[{}] {} refresh token", id.toStdString(), token.isEmpty() ? "forgetting" : "saving");
        if (token.isEmpty())
            table.remove("refresh_token");
        else
            table.insert("refresh_token", token);
        entry = table;
        file.set(journals_key, entries);
        save_config(file);
        return;
    }
    spdlog::warn("[{}] no journal source to save its refresh token in", id.toStdString());
}
