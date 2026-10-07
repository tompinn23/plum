#include "config.hpp"

#include <QFile>
#include <QSaveFile>
#include <QTimeZone>

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>


namespace {
    std::string toStd(const QString &s) { return s.toStdString(); } // UTF-8
    QString fromStd(const std::string_view s) { return QString::fromUtf8(s.data(), static_cast<int>(s.size())); }

    QStringList splitKey(const QString &key) { return key.split(QLatin1Char('.'), Qt::SkipEmptyParts); }

    QDate toQDate(const toml::date &d) { return QDate(d.year, d.month, d.day); }

    QTime toQTime(const toml::time &t) {
        return {t.hour, t.minute, t.second, static_cast<int>(t.nanosecond / 1000000)};
    }

    QDateTime toQDateTime(const toml::date_time &dt) {
        const QDate date = toQDate(dt.date);
        const QTime time = toQTime(dt.time);
        if (!dt.offset)
            return {date, time}; // TOML "local date-time"
        const int secs = dt.offset->minutes * 60;

        return {date, time, QTimeZone::fromSecondsAheadOfUtc(secs)};
    }

    QVariant nodeToVariant(const toml::node &node) {
        switch (node.type()) {
            case toml::node_type::string: return fromStd(node.as_string()->get());
            case toml::node_type::integer: return node.as_integer()->get();
            case toml::node_type::floating_point: return node.as_floating_point()->get();
            case toml::node_type::boolean: return node.as_boolean()->get();
            case toml::node_type::date: return toQDate(node.as_date()->get());
            case toml::node_type::time: return toQTime(node.as_time()->get());
            case toml::node_type::date_time: return toQDateTime(node.as_date_time()->get());
            case toml::node_type::array: {
                QVariantList list;
                for (const toml::node &el: *node.as_array())
                    list.append(nodeToVariant(el));
                return list;
            }
            case toml::node_type::table: {
                QVariantMap map;
                for (const auto &[k, v]: *node.as_table())
                    map.insert(fromStd(k.str()), nodeToVariant(v));
                return map;
            }
            default:
                return {};
        }
    }

    // ---------- Qt -> TOML ----------

    toml::date fromQDate(const QDate &d) { return {d.year(), d.month(), d.day()}; }

    toml::time fromQTime(const QTime &t) {
        return {t.hour(), t.minute(), t.second(), static_cast<uint32_t>(t.msec()) * 1000000u};
    }

    toml::date_time fromQDateTime(const QDateTime &dt) {
        const toml::date date = fromQDate(dt.date());
        const toml::time time = fromQTime(dt.time());
        if (dt.timeSpec() == Qt::LocalTime)
            return {date, time};
        const int minutes = dt.offsetFromUtc() / 60;
        return {date, time, toml::time_offset(minutes / 60, minutes % 60)};
    }

    bool pushInto(toml::array &arr, const QVariant &v);

    bool insertInto(toml::table &tbl, const std::string &key, const QVariant &v);

    // Converts a QVariant into a TOML value and hands it to `sink`.
    template<typename Sink>
    bool emitNode(const QVariant &v, Sink &&sink) {
        switch (v.userType()) {
            case QMetaType::Bool:
                sink(v.toBool());
                return true;
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::Short:
            case QMetaType::UShort:
            case QMetaType::Long:
            case QMetaType::ULong:
            case QMetaType::LongLong:
            case QMetaType::ULongLong:
                sink(int64_t(v.toLongLong()));
                return true;
            case QMetaType::Double:
            case QMetaType::Float:
                sink(v.toDouble());
                return true;
            case QMetaType::QString:
                sink(toStd(v.toString()));
                return true;
            case QMetaType::QDate:
                sink(fromQDate(v.toDate()));
                return true;
            case QMetaType::QTime:
                sink(fromQTime(v.toTime()));
                return true;
            case QMetaType::QDateTime:
                sink(fromQDateTime(v.toDateTime()));
                return true;
            case QMetaType::QStringList:
            case QMetaType::QVariantList: {
                toml::array arr;
                for (const QVariantList list = v.toList(); const QVariant &item: list)
                    pushInto(arr, item);
                sink(std::move(arr));
                return true;
            }
            case QMetaType::QVariantMap: {
                toml::table tbl;
                const QVariantMap map = v.toMap();
                for (auto it = map.cbegin(); it != map.cend(); ++it)
                    insertInto(tbl, toStd(it.key()), it.value());
                sink(std::move(tbl));
                return true;
            }
            case QMetaType::QVariantHash: {
                toml::table tbl;
                const QVariantHash hash = v.toHash();
                for (auto it = hash.cbegin(); it != hash.cend(); ++it)
                    insertInto(tbl, toStd(it.key()), it.value());
                sink(std::move(tbl));
                return true;
            }
            default:
                // QColor, QUrl, QByteArray, enums, ... anything with a string form.
                if (!v.isNull() && v.canConvert<QString>()) {
                    sink(toStd(v.toString()));
                    return true;
                }
                return false;
        }
    }

    bool pushInto(toml::array &arr, const QVariant &v) {
        return emitNode(v, [&arr](auto &&n) { arr.push_back(std::forward<decltype(n)>(n)); });
    }

    bool insertInto(toml::table &tbl, const std::string &key, const QVariant &v) {
        return emitNode(v, [&](auto &&n) { tbl.insert_or_assign(key, std::forward<decltype(n)>(n)); });
    }

    QString formatError(const toml::parse_error &e, const QString &source) {
        return QStringLiteral("%1:%2:%3: %4")
                .arg(source.isEmpty() ? QStringLiteral("<string>") : source)
                .arg(e.source().begin.line)
                .arg(e.source().begin.column)
                .arg(fromStd(e.description()));
    }
} // namespace


config::config(const QString &path, QObject *parent)
    : config(parent) {
    load(path);
}

config::~config() = default;

bool config::parse_data(const QByteArray &data, const QString &source) {
    const std::string_view doc(data.constData(), static_cast<size_t>(data.size()));
    const std::string name = toStd(source);

    try {
        root = toml::parse(doc, name);
    } catch (const toml::parse_error &e) {
        error = formatError(e, source);
        Q_EMIT load_failed(error);
        return false;
    }

    error.clear();
    return true;
}

bool config::load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = tr("Cannot open %1: %2").arg(path, file.errorString());
        Q_EMIT load_failed(error);
        return false;
    }
    if (!parse_data(file.readAll(), path))
        return false; // previous contents are kept on error

    this->path = path;
    return true;
}

bool config::save(const QString &path) {
    const QString filename = path.isEmpty() ? this->path : path;
    if (filename.isEmpty()) {
        error = tr("No file path set");
        return false;
    }

    std::ostringstream os;
    os << root << '\n';
    const QByteArray bytes = QByteArray::fromStdString(os.str());

    if (QSaveFile file(filename); !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.
                                  commit()) {
        error = tr("Cannot write %1: %2").arg(filename, file.errorString());
        return false;
    }

    this->path = filename;
    return true;
}

QString config::get_path() const { return path; }
QString config::get_error() const { return error; }

bool config::contains(const QString &key) const {
    return static_cast<bool>(root.at_path(toStd(key)));
}

QVariant config::value(const QString &key, const QVariant &defaultValue) const {
    const auto view = root.at_path(toStd(key));
    return view ? nodeToVariant(*view.node()) : defaultValue;
}

bool config::set(const QString &key, const QVariant &value) {
    const QStringList parts = splitKey(key);
    if (parts.isEmpty())
        return false;
    if (!value.isValid())
        return remove(key);

    toml::table *tbl = &root;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const std::string part = toStd(parts.at(i));
        toml::table *child = tbl->get_as<toml::table>(part);
        if (!child) {
            // missing (or not a table): create it
            tbl->insert_or_assign(part, toml::table{});
            child = tbl->get_as<toml::table>(part);
        }
        tbl = child;
    }

    if (!insertInto(*tbl, toStd(parts.last()), value)) {
        error = tr("Unsupported value type '%1' for key '%2'")
                .arg(QString::fromLatin1(value.typeName()), key);
        return false;
    }
    return true;
}

bool config::remove(const QString &key) {
    const QStringList parts = splitKey(key);
    if (parts.isEmpty())
        return false;

    toml::table *tbl = &root;
    for (int i = 0; i < parts.size() - 1 && tbl; ++i)
        tbl = tbl->get_as<toml::table>(toStd(parts.at(i)));
    if (!tbl || tbl->erase(toStd(parts.last())) == 0)
        return false;

    return true;
}

void config::clear() { root.clear(); }

QStringList config::child_keys(const QString &group) const {
    const toml::table *tbl = group.isEmpty() ? &root : root.at_path(toStd(group)).as_table();
    QStringList keys;
    if (tbl)
        for (const auto &[k, v]: *tbl)
            if (!v.is_table())
                keys << fromStd(k.str());
    return keys;
}

QStringList config::child_groups(const QString &group) const {
    const toml::table *tbl = group.isEmpty() ? &root : root.at_path(toStd(group)).as_table();
    QStringList groups;
    if (tbl)
        for (const auto &[k, v]: *tbl)
            if (v.is_table())
                groups << fromStd(k.str());
    return groups;
}

QVariantMap config::to_variant_map() const { return nodeToVariant(root).toMap(); }
