// PlotMath.h — number formatting (same output as qml/js/Format.js), axis ticks and QML data
// conversion shared by the plot items.
#pragma once

#include <QJSValue>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace plotmath {

inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

inline bool finite(double v) { return std::isfinite(v); }

struct SiParts { double scaled; QString prefix; };

inline SiParts siParts(double v)
{
    static const char* const P[] = {"f", "p", "n", "µ", "m", "", "k", "M", "G"};
    if (v == 0 || !finite(v)) return {v, QString()};
    int e = int(std::floor(std::log10(std::abs(v)) / 3)) * 3;
    e = std::clamp(e, -15, 9);
    return {v / std::pow(10.0, e), QString::fromUtf8(P[(e + 15) / 3])};
}

inline QString si(double v, const QString& unit, int digits = 5)
{
    if (std::isnan(v)) return QStringLiteral("—");
    if (!finite(v)) return v > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    if (unit.isEmpty() && v == std::floor(v) && std::abs(v) < 1e15) return QString::number(qint64(v));
    const SiParts p = siParts(v);
    const double a = std::abs(p.scaled);
    const int intDigits = a >= 100 ? 3 : a >= 10 ? 2 : 1;
    const QString s = QString::number(p.scaled, 'f', std::max(0, digits - intDigits)) + ' ' + p.prefix + unit;
    return s.trimmed();
}

// Axis tick text: decimals follow the tick step, prefix follows the larger of value and step.
inline QString tick(double value, double step, const QString* unit)
{
    const double ref = std::max(std::abs(value), std::abs(step));
    const SiParts p = siParts(ref);
    const double scale = p.scaled != 0 ? ref / p.scaled : 1;
    const double ss = std::abs(step / scale);
    const int decimals = ss >= 1 ? 0 : std::min(9, int(std::ceil(-std::log10(ss) - 1e-9)));
    QString s = QString::number(value / scale, 'f', decimals);
    if (unit) s += ' ' + p.prefix + *unit;
    return s.trimmed();
}

struct Nice { double min, max, step; };

inline Nice niceTicks(double lo, double hi, int count)
{
    if (!finite(lo) || !finite(hi)) return {0, 1, 0.25};
    if (lo == hi) {
        const double pad = std::abs(lo) > 0 ? std::abs(lo) * 1e-3 : 1e-9;
        lo -= pad;
        hi += pad;
    }
    const double rough = (hi - lo) / std::max(1, count);
    const double mag = std::pow(10.0, std::floor(std::log10(rough)));
    const double norm = rough / mag;
    const double step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
    return {std::floor(lo / step) * step, std::ceil(hi / step) * step, step};
}

// Tick values inside [lo, hi] (exact zero instead of -1e-17).
inline std::vector<double> ticksIn(double lo, double hi, int count, double* stepOut)
{
    const Nice n = niceTicks(lo, hi, count);
    *stepOut = n.step;
    std::vector<double> out;
    const double first = std::ceil(lo / n.step - 1e-9);
    for (int k = 0; k < 1000; ++k) {
        double v = (first + k) * n.step;
        if (v > hi + n.step * 1e-9) break;
        if (std::abs(v) < n.step * 1e-6) v = 0;
        out.push_back(v);
    }
    return out;
}

inline QVariantList unwrapList(const QVariant& v)
{
    if (v.metaType() == QMetaType::fromType<QJSValue>()) return v.value<QJSValue>().toVariant().toList();
    return v.toList();
}

inline QVariantMap unwrapMap(const QVariant& v)
{
    if (v.metaType() == QMetaType::fromType<QJSValue>()) return v.value<QJSValue>().toVariant().toMap();
    return v.toMap();
}

inline std::vector<double> toDoubles(const QVariant& v)
{
    const QVariantList l = unwrapList(v);
    std::vector<double> out;
    out.reserve(size_t(l.size()));
    for (const QVariant& q : l) {
        bool ok = false;
        const double d = q.isNull() ? NaN : q.toDouble(&ok);   // bools count as 1 / 0
        out.push_back(ok ? d : NaN);
    }
    return out;
}

} // namespace plotmath
