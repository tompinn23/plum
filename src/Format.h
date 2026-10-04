#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <QLocale>
#include <QString>

// Display formatting shared by the dashboards.
namespace fmt_ui {

inline QString text(const std::optional<std::string> &value) {
    return value && !value->empty() ? QString::fromStdString(*value) : QStringLiteral("-");
}

inline QString number(std::int64_t value) {
    return QLocale(QLocale::English).toString(static_cast<qlonglong>(value));
}

inline QString number(const std::optional<std::int64_t> &value) {
    return value ? number(*value) : QStringLiteral("-");
}

inline QString credits(std::int64_t value) {
    return number(value) + QStringLiteral(" cr");
}

inline QString credits(const std::optional<std::int64_t> &value) {
    return value ? credits(*value) : QStringLiteral("-");
}

inline QString decimal(const std::optional<double> &value, int places, const QString &unit = {}) {
    if (!value) return QStringLiteral("-");
    QString s = QString::number(*value, 'f', places);
    return unit.isEmpty() ? s : s + QLatin1Char(' ') + unit;
}

}  // namespace fmt_ui
