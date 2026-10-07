#include "commodities.hpp"

#include <QFile>
#include <QHash>

namespace {
    // EDCD's FDevIDs list: id,symbol,category,name.
    QHash<QString, commodity> load() {
        QHash<QString, commodity> table;
        QFile file(QStringLiteral(":/resources/commodity.csv"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return table;
        file.readLine(); // header
        while (!file.atEnd()) {
            const auto columns = QString::fromUtf8(file.readLine()).trimmed().split(',');
            if (columns.size() == 4) table.insert(columns[1].toLower(), {.name = columns[3], .category = columns[2]});
        }
        return table;
    }

    QString bare(QString symbol) {
        symbol = symbol.toLower();
        if (symbol.startsWith('$') && symbol.endsWith("_name;")) symbol = symbol.mid(1, symbol.size() - 7);
        return symbol;
    }
} // namespace

commodity find_commodity(const QString &symbol) {
    static const QHash<QString, commodity> table = load();
    const QString key = bare(symbol);
    return table.value(key, {.name = key, .category = {}});
}
