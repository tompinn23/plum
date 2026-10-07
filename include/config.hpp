#pragma once

#include "toml++/toml.hpp"

#include <QObject>
#include <QVariant>


class config : public QObject {
    Q_OBJECT

public:
    explicit config(QObject *parent = nullptr) : QObject(parent) {
    }

    explicit config(const QString &path, QObject *parent = nullptr);

    ~config() override;

    bool load(const QString &path);

    bool reload();

    bool save(const QString &path = QString());

    QString get_path() const;

    QString get_error() const;

    bool contains(const QString &key) const;

    QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;

    template<typename T>
    T get(const QString &key, const T &defaultValue = T()) const {
        const QVariant v = value(key);
        return (v.isValid() && v.canConvert<T>()) ? v.value<T>() : defaultValue;
    }

    bool set(const QString &key, const QVariant &value);

    bool remove(const QString &key);

    void clear();

    QStringList child_keys(const QString &group = QString()) const;

    QStringList child_groups(const QString &group = QString()) const;

    QVariantMap to_variant_map() const;

    Q_SIGNALS:


    void load_failed(const QString &error);

private:
    bool parse_data(const QByteArray &data, const QString &source);

    toml::table root;
    QString path;
    QString error;
};
