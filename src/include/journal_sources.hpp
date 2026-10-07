#pragma once

#include <vector>

#include <QString>

// A journal directory the user has asked us to watch. The id identifies the feed while the app
// runs and in the config file. It has no name of its own: it is labelled by the commander its journals
// name. History is kept per commander, not per source.
struct journal_source {
    QString id;
    QString directory;
};

// Reads the configured directories. On first run, before anything has been saved, this is the
// standard Elite Dangerous journal location.
std::vector<journal_source> load_journal_sources();

void save_journal_sources(const std::vector<journal_source> &sources);

// The Frontier refresh token kept with a source's entry, or empty. Saving an empty one forgets it.
QString load_refresh_token(const QString &id);

void save_refresh_token(const QString &id, const QString &token);

QString new_journal_source_id();

// The commander named in a directory's newest journal that names anyone, or "<unknown>".
QString journal_commander(const QString &directory);

QString default_journal_directory();
