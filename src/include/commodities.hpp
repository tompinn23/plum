#pragma once

#include <QString>

// A commodity's name as the game shows it, and its market category.
struct commodity {
    QString name;
    QString category;
};

// By journal or Companion API symbol in any case ("Gold", "$gold_name;", "gold"). Unknown symbols
// come back as themselves, uncategorised.
commodity find_commodity(const QString &symbol);
