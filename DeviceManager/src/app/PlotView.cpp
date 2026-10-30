#include "PlotView.h"

#include <QCursor>
#include <QGuiApplication>
#include <QStyleHints>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QJSValue>
#include <QPainter>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kWheelStep = 0.85;     // zoom factor per wheel notch
constexpr double kDragScale = 150.0;    // pixels of axis drag per e-fold of zoom
constexpr double kMinBox = 6.0;         // smaller box sides do not zoom that axis

bool finite(double v) { return std::isfinite(v); }

// ---- formatting, same output as qml/js/Format.js ----------------------------

struct SiParts { double scaled; QString prefix; };

SiParts siParts(double v)
{
    static const char* const P[] = {"f", "p", "n", "µ", "m", "", "k", "M", "G"};
    if (v == 0 || !finite(v)) return {v, QString()};
    int e = int(std::floor(std::log10(std::abs(v)) / 3)) * 3;
    e = std::clamp(e, -15, 9);
    return {v / std::pow(10.0, e), QString::fromUtf8(P[(e + 15) / 3])};
}

QString si(double v, const QString& unit, int digits = 5)
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
QString tick(double value, double step, const QString* unit)
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

Nice niceTicks(double lo, double hi, int count)
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
std::vector<double> ticksIn(double lo, double hi, int count, double* stepOut)
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

QVariantList unwrapList(const QVariant& v)
{
    if (v.metaType() == QMetaType::fromType<QJSValue>()) return v.value<QJSValue>().toVariant().toList();
    return v.toList();
}

QVariantMap unwrapMap(const QVariant& v)
{
    if (v.metaType() == QMetaType::fromType<QJSValue>()) return v.value<QJSValue>().toVariant().toMap();
    return v.toMap();
}

std::vector<double> toDoubles(const QVariant& v)
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

bool monotonic(const std::vector<double>& x)
{
    double prev = -std::numeric_limits<double>::infinity();
    for (double v : x) {
        if (!(v >= prev)) return false;     // also false for NaN
        prev = v;
    }
    return true;
}

bool validSpan(double lo, double hi)
{
    const double span = hi - lo;
    return finite(lo) && finite(hi) && span > std::max(std::abs(lo), std::abs(hi)) * 1e-12 && span < 1e300;
}

double clampPx(double v) { return std::clamp(v, -1e6, 1e6); }

} // namespace

// ============================================================================

PlotView::PlotView(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    connect(this, &PlotView::styleChanged, this, [this] { update(); });
}

// ---------------------------------------------------------------- data

void PlotView::setData(const QVariant& xsV, const QVariant& seriesV)
{
    const std::vector<double> xs = toDoubles(xsV);
    std::vector<Series> out;
    for (const QVariant& item : unwrapList(seriesV)) {
        const QVariantMap m = unwrapMap(item);
        Series s;
        s.label = m.value("label").toString();
        s.unit = m.value("unit").toString();
        if (m.contains("color") && !m.value("color").toString().isEmpty()) s.color = QColor(m.value("color").toString());
        s.axis = m.value("axis").toString() == QLatin1String("right") ? 1 : 0;
        s.y = toDoubles(m.value("values"));
        const QVariant ownX = m.value("xs");
        s.x = ownX.isValid() && !ownX.isNull() ? toDoubles(ownX) : xs;
        s.monotonic = monotonic(s.x);
        out.push_back(std::move(s));
    }
    bool same = out.size() == m_series.size();
    for (size_t i = 0; same && i < out.size(); ++i) same = out[i].label == m_series[i].label;
    m_series = std::move(out);
    if (!same) {
        m_autoX = m_autoY = true;      // different curves: the old zoom means nothing
        m_cursorsPlaced = false;
        emit viewChanged();
    }
    emit dataChanged();
    update();
}

void PlotView::setSeries(int index, std::vector<double> x, std::vector<double> y, const QString& label,
                         const QString& unit)
{
    if (index < 0) return;
    if (size_t(index) >= m_series.size()) m_series.resize(size_t(index) + 1);
    Series& s = m_series[size_t(index)];
    s.x = std::move(x);
    s.y = std::move(y);
    s.label = label;
    s.unit = unit;
    s.monotonic = monotonic(s.x);
    emit dataChanged();
    update();
}

void PlotView::setSeriesSpecs(const QVariant& specs)
{
    m_series.clear();
    for (const QVariant& item : unwrapList(specs)) {
        const QVariantMap m = unwrapMap(item);
        Series s;
        s.label = m.value("label").toString();
        s.unit = m.value("unit").toString();
        if (!m.value("color").toString().isEmpty()) s.color = QColor(m.value("color").toString());
        s.axis = m.value("axis").toString() == QLatin1String("right") ? 1 : 0;
        m_series.push_back(std::move(s));
    }
    resetView();
    emit dataChanged();
}

void PlotView::append(int index, double x, double y)
{
    if (index < 0 || size_t(index) >= m_series.size()) return;
    Series& s = m_series[size_t(index)];
    if (!finite(x) || (!s.x.empty() && x < s.x.back())) s.monotonic = false;
    s.x.push_back(x);
    s.y.push_back(y);
    if (m_keepSpan > 0 && s.monotonic) {
        // drop history in batches, not one sample per call
        const size_t cut = size_t(std::lower_bound(s.x.begin(), s.x.end(), x - m_keepSpan) - s.x.begin());
        if (cut > 64 || cut * 4 > s.x.size()) {
            s.x.erase(s.x.begin(), s.x.begin() + long(cut));
            s.y.erase(s.y.begin(), s.y.begin() + long(cut));
        }
    }
    emit dataChanged();
    update();
}

void PlotView::clearData()
{
    for (Series& s : m_series) {
        s.x.clear();
        s.y.clear();
        s.monotonic = true;
    }
    resetView();
    emit dataChanged();
}

int PlotView::pointCount() const
{
    size_t n = 0;
    for (const Series& s : m_series) n += std::min(s.x.size(), s.y.size());
    return int(std::min<size_t>(n, INT_MAX));
}

bool PlotView::hasData() const
{
    for (const Series& s : m_series)
        if (std::min(s.x.size(), s.y.size()) > 0) return true;
    return false;
}

bool PlotView::hasRight() const
{
    return std::any_of(m_series.begin(), m_series.end(), [](const Series& s) { return s.axis == 1; });
}

QColor PlotView::colorOf(int i) const
{
    if (m_series[size_t(i)].color.isValid()) return m_series[size_t(i)].color;
    if (m_seriesColors.isEmpty()) return m_accent;
    const QVariant c = m_seriesColors[i % m_seriesColors.size()];
    return c.metaType() == QMetaType::fromType<QColor>() ? c.value<QColor>() : QColor(c.toString());
}

// ---------------------------------------------------------------- view

void PlotView::resetView()
{
    m_autoX = m_autoY = true;
    m_cursorsPlaced = false;
    emit viewChanged();
    update();
}

void PlotView::setZoomAxis(const QString& a)
{
    const QString v = (a == QLatin1String("x") || a == QLatin1String("y")) ? a : QString();
    if (v == m_zoomAxis) return;
    m_zoomAxis = v;
    emit viewChanged();
    update();
}

void PlotView::setCursors(bool on)
{
    if (on == m_cursors) return;
    m_cursors = on;
    m_cursorsPlaced = false;
    emit styleChanged();
}

void PlotView::computeAuto(double approxWidth, double plotHeight)
{
    const bool follow = m_followSpan > 0;
    if (m_autoX) {
        if (follow) {
            double now = m_xNow;
            if (!(now > 0)) {
                now = 0;
                for (const Series& s : m_series)
                    if (!s.x.empty() && finite(s.x.back())) now = std::max(now, s.x.back());
            }
            m_x1 = now;
            m_x0 = now - m_followSpan;
        } else {
            double lo = INFINITY, hi = -INFINITY;
            for (const Series& s : m_series) {
                const size_t n = std::min(s.x.size(), s.y.size());
                if (s.monotonic && n > 0) {
                    lo = std::min(lo, s.x.front());
                    hi = std::max(hi, s.x[n - 1]);
                } else {
                    for (size_t k = 0; k < n; ++k)
                        if (finite(s.x[k])) { lo = std::min(lo, s.x[k]); hi = std::max(hi, s.x[k]); }
                }
            }
            if (lo == INFINITY) { lo = 0; hi = 1; }
            const Nice n = niceTicks(lo, hi, std::max(2, int(approxWidth / 110)));
            m_x0 = n.min;
            m_x1 = n.max;
        }
        m_xRef = m_x1;
    }
    if (m_autoY) {
        for (int a = 0; a < 2; ++a) {
            double lo = INFINITY, hi = -INFINITY;
            for (const Series& s : m_series) {
                if (s.axis != a) continue;
                const size_t n = std::min(s.x.size(), s.y.size());
                size_t i0 = 0, i1 = n;
                if (follow && s.monotonic) {
                    i0 = size_t(std::lower_bound(s.x.begin(), s.x.begin() + long(n), m_x0) - s.x.begin());
                    i1 = size_t(std::upper_bound(s.x.begin(), s.x.begin() + long(n), m_x1) - s.x.begin());
                }
                for (size_t k = i0; k < i1; ++k) {
                    const double v = s.y[k];
                    if (!finite(v)) continue;
                    if (follow && !s.monotonic && (s.x[k] < m_x0 || s.x[k] > m_x1)) continue;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
            if (lo == INFINITY) { lo = 0; hi = 1; }
            const double span = hi - lo;
            const double pad = span > 0 ? span * (follow ? 0.08 : 0.06) : 0;
            const Nice n = niceTicks(lo - pad, hi + pad, std::max(2, int(plotHeight / (follow ? 50 : 48))));
            m_y0[a] = n.min;
            m_y1[a] = n.max;
        }
    }
}

double PlotView::toPx(double x) const
{
    return m_plot.left() + (x - m_x0) / (m_x1 - m_x0) * m_plot.width();
}

double PlotView::toPy(int a, double y) const
{
    return m_plot.bottom() - (y - m_y0[a]) / (m_y1[a] - m_y0[a]) * m_plot.height();
}

double PlotView::fromPx(double px) const
{
    return m_x0 + (px - m_plot.left()) / m_plot.width() * (m_x1 - m_x0);
}

double PlotView::fromPy(int a, double py) const
{
    return m_y0[a] + (m_plot.bottom() - py) / m_plot.height() * (m_y1[a] - m_y0[a]);
}

void PlotView::zoom(double fx, double fy, double px, double py) { zoomAxes(fx, fy, QPointF(px, py), 3); }

void PlotView::pan(double dxPx, double dyPx) { panAxes(dxPx, dyPx, true, 3); }

void PlotView::zoomAxes(double fx, double fy, const QPointF& anchor, int yMask)
{
    if (m_plot.isEmpty()) return;
    if (fx != 1) {
        const double ax = fromPx(anchor.x());
        const double lo = ax - (ax - m_x0) * fx, hi = ax + (m_x1 - ax) * fx;
        if (validSpan(lo, hi)) { m_x0 = lo; m_x1 = hi; m_autoX = false; }
    }
    if (fy != 1) {
        for (int a = 0; a < 2; ++a) {
            if (!(yMask & (1 << a)) || (a == 1 && !hasRight())) continue;
            const double ay = fromPy(a, anchor.y());
            const double lo = ay - (ay - m_y0[a]) * fy, hi = ay + (m_y1[a] - ay) * fy;
            if (validSpan(lo, hi)) { m_y0[a] = lo; m_y1[a] = hi; m_autoY = false; }
        }
    }
    emit viewChanged();
    update();
}

void PlotView::panAxes(double dx, double dy, bool xAxis, int yMask)
{
    if (m_plot.isEmpty()) return;
    if (xAxis && dx != 0) {
        const double d = -dx / m_plot.width() * (m_x1 - m_x0);
        m_x0 += d;
        m_x1 += d;
        m_autoX = false;
    }
    if (dy != 0) {
        for (int a = 0; a < 2; ++a) {
            if (!(yMask & (1 << a)) || (a == 1 && !hasRight())) continue;
            const double d = dy / m_plot.height() * (m_y1[a] - m_y0[a]);
            m_y0[a] += d;
            m_y1[a] += d;
            m_autoY = false;
        }
    }
    emit viewChanged();
    update();
}

// ---------------------------------------------------------------- lookups

long PlotView::nearest(const Series& s, double x) const
{
    const size_t n = std::min(s.x.size(), s.y.size());
    if (n == 0) return -1;
    if (s.monotonic) {
        const size_t i = size_t(std::lower_bound(s.x.begin(), s.x.begin() + long(n), x) - s.x.begin());
        if (i == 0) return 0;
        if (i >= n) return long(n - 1);
        return std::abs(s.x[i] - x) < std::abs(s.x[i - 1] - x) ? long(i) : long(i - 1);
    }
    long best = -1;
    double bd = INFINITY;
    for (size_t k = 0; k < n; ++k) {
        const double d = std::abs(s.x[k] - x);
        if (d < bd) { bd = d; best = long(k); }
    }
    return best;
}

double PlotView::valueAt(const Series& s, double x) const
{
    const size_t n = std::min(s.x.size(), s.y.size());
    if (n == 0) return NaN;
    if (!s.monotonic) return s.y[size_t(nearest(s, x))];
    const size_t i = size_t(std::lower_bound(s.x.begin(), s.x.begin() + long(n), x) - s.x.begin());
    if (i == 0) return s.y[0];
    if (i >= n) return s.y[n - 1];
    const double x0 = s.x[i - 1], x1 = s.x[i];
    if (x1 == x0) return s.y[i];
    return s.y[i - 1] + (s.y[i] - s.y[i - 1]) * (x - x0) / (x1 - x0);
}

QString PlotView::xText(double x, int digits) const
{
    if (m_xMode == QLatin1String("ago")) {
        const double d = m_xRef - x;
        return std::abs(d) < 0.005 ? QStringLiteral("现在") : QStringLiteral("−%1 s").arg(d, 0, 'f', 2);
    }
    return si(x, m_xUnit, digits);
}

// ---------------------------------------------------------------- paint

void PlotView::paint(QPainter* p)
{
    QElapsedTimer timer;
    timer.start();
    const double W = width(), H = height();
    QFont font(m_fontFamily);
    font.setPixelSize(m_fontSize);
    p->setFont(font);
    const QFontMetricsF fm(font);
    m_plot = QRectF();

    if (!hasData() && m_followSpan <= 0) {
        p->setPen(m_faint);
        p->drawText(QRectF(0, 0, W, H), Qt::AlignCenter, m_emptyText);
        m_lastPaintMs = timer.nsecsElapsed() / 1e6;
        return;
    }

    const bool ago = m_xMode == QLatin1String("ago");
    const bool right = hasRight();
    const double mt = 10, mb = m_xLabel.isEmpty() ? 26 : 40;
    const double ph = H - mt - mb;
    if (ph < 20 || W < 80) return;
    computeAuto(W - 80, ph);

    // Y tick labels first: the left / right margins fit the widest one.
    struct Label { double v; QString text; double w; };
    auto unitOf = [this](int a) {
        for (const Series& s : m_series) if (s.axis == a) return s.unit;
        return QString();
    };
    auto yLabels = [&](int a) {
        double step;
        std::vector<Label> out;
        const QString unit = unitOf(a);
        for (double v : ticksIn(m_y0[a], m_y1[a], std::max(2, int(ph / 48)), &step)) {
            const QString t = tick(v, step, &unit);
            out.push_back({v, t, fm.horizontalAdvance(t)});
        }
        return out;
    };
    const std::vector<Label> left = yLabels(0);
    const std::vector<Label> rightL = right ? yLabels(1) : std::vector<Label>();
    auto widest = [](const std::vector<Label>& l) {
        double w = 40;
        for (const Label& x : l) w = std::max(w, x.w);
        return w;
    };
    const double ml = std::ceil(widest(left) + 14);
    const double mr = right ? std::ceil(widest(rightL) + 14) : (ago ? 18 : 14);
    const double pw = W - ml - mr;
    if (pw < 20) return;
    m_plot = QRectF(ml, mt, pw, ph);

    // ---- grid, tick labels, frame (crisp, no antialiasing)
    p->setRenderHint(QPainter::Antialiasing, false);
    p->setRenderHint(QPainter::TextAntialiasing, true);
    for (const Label& l : left) {
        const double y = std::round(toPy(0, l.v));
        p->setPen(QPen(m_grid, 1));
        p->drawLine(QLineF(ml, y, ml + pw, y));
        p->setPen(m_muted);
        p->drawText(QRectF(0, y - 9, ml - 8, 18), Qt::AlignRight | Qt::AlignVCenter, l.text);
    }
    for (const Label& l : rightL) {
        const double y = std::round(toPy(1, l.v));
        p->setPen(m_muted);
        p->drawText(QRectF(ml + pw + 8, y - 9, mr - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, l.text);
    }
    {
        double step;
        std::vector<std::pair<double, QString>> xt;   // x value, text
        if (ago) {
            for (double d : ticksIn(m_xRef - m_x1, m_xRef - m_x0, std::max(2, int(pw / 110)), &step))
                xt.emplace_back(m_xRef - d, d == 0 ? QStringLiteral("现在") : "−" + tick(d, step, nullptr) + " s");
        } else {
            for (double v : ticksIn(m_x0, m_x1, std::max(2, int(pw / 110)), &step))
                xt.emplace_back(v, tick(v, step, &m_xUnit));
        }
        for (const auto& [v, text] : xt) {
            const double x = std::round(toPx(v));
            if (x < ml - 0.5 || x > ml + pw + 0.5) continue;
            p->setPen(QPen(m_grid, 1));
            p->drawLine(QLineF(x, mt, x, mt + ph));
            p->setPen(ago ? m_faint : m_muted);
            // centered under the tick, but kept inside the item at both ends
            const double tw = fm.horizontalAdvance(text);
            const double tx = std::clamp(x - tw / 2, 0.0, W - tw - 1);
            p->drawText(QRectF(tx, mt + ph + 5, tw + 1, 18), Qt::AlignLeft | Qt::AlignTop, text);
        }
    }
    if (!m_xLabel.isEmpty()) {
        p->setPen(m_faint);
        p->drawText(QRectF(ml, mt + ph + 21, pw, 18), Qt::AlignHCenter | Qt::AlignTop, m_xLabel);
    }
    p->setPen(QPen(m_axis, 1));
    p->drawLine(QLineF(ml, mt, ml, mt + ph));
    p->drawLine(QLineF(ml, mt + ph, ml + pw, mt + ph));
    if (right) p->drawLine(QLineF(ml + pw, mt, ml + pw, mt + ph));
    // selected zoom axis: tinted band behind its labels and a thick accent line
    if (!m_zoomAxis.isEmpty()) {
        QColor band = m_accent;
        band.setAlphaF(0.10f);
        const bool xs = m_zoomAxis == QLatin1String("x");
        if (xs) p->fillRect(QRectF(ml, mt + ph + 1, pw, mb - 2), band);
        else {
            p->fillRect(QRectF(1, mt, ml - 2, ph), band);
            if (right) p->fillRect(QRectF(ml + pw + 1, mt, mr - 2, ph), band);
        }
        p->setPen(QPen(m_accent, 3));
        if (xs) p->drawLine(QLineF(ml, mt + ph + 1, ml + pw, mt + ph + 1));
        else {
            p->drawLine(QLineF(ml - 1, mt, ml - 1, mt + ph));
            if (right) p->drawLine(QLineF(ml + pw + 1, mt, ml + pw + 1, mt + ph));
        }
    }

    // ---- series
    p->setRenderHint(QPainter::Antialiasing, true);
    p->save();
    p->setClipRect(QRectF(ml, mt - 2, pw, ph + 4));
    std::vector<QPointF> pts;
    pts.reserve(size_t(pw) * 4 + 16);
    auto flush = [&] {
        if (pts.size() >= 2) p->drawPolyline(pts.data(), int(pts.size()));
        else if (pts.size() == 1) p->drawPoint(pts[0]);
        pts.clear();
    };
    for (size_t i = 0; i < m_series.size(); ++i) {
        const Series& s = m_series[i];
        const size_t n = std::min(s.x.size(), s.y.size());
        if (n == 0) continue;
        const QColor color = colorOf(int(i));
        QPen pen(color, m_lineWidth);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p->setPen(pen);
        const int a = s.axis;
        size_t i0 = 0, i1 = n;
        if (s.monotonic) {
            i0 = size_t(std::lower_bound(s.x.begin(), s.x.begin() + long(n), m_x0) - s.x.begin());
            i1 = size_t(std::upper_bound(s.x.begin(), s.x.begin() + long(n), m_x1) - s.x.begin());
            if (i0 > 0) --i0;               // keep the segments that cross the edges
            if (i1 < n) ++i1;
        }
        const size_t visible = i1 - i0;
        auto point = [&](size_t k) { return QPointF(clampPx(toPx(s.x[k])), clampPx(toPy(a, s.y[k]))); };

        if (s.monotonic && visible > size_t(4 * pw)) {
            // Dense data reads like a scope trace: a 1 px cosmetic pen takes QPainter's fast line
            // path, where a thick antialiased pen would stroke thousands of overlapping segments.
            QPen thin(color, 1);
            thin.setCosmetic(true);
            p->setPen(thin);
            // M4 decimation: per pixel column keep first, min, max, last (in sample order),
            // which draws exactly what the full polyline would, peaks included.
            long col = LONG_MIN;
            size_t first = 0, last = 0, mn = 0, mx = 0;
            bool open = false;
            auto emitColumn = [&] {
                if (!open) return;
                size_t idx[4] = {first, mn, mx, last};
                std::sort(idx, idx + 4);
                for (int k = 0; k < 4; ++k)
                    if (k == 0 || idx[k] != idx[k - 1]) pts.push_back(point(idx[k]));
                open = false;
            };
            for (size_t k = i0; k < i1; ++k) {
                const double v = s.y[k];
                if (!finite(v)) { emitColumn(); flush(); continue; }
                const long c = long(std::floor(toPx(s.x[k])));
                if (!open || c != col) {
                    emitColumn();
                    col = c;
                    first = last = mn = mx = k;
                    open = true;
                } else {
                    last = k;
                    if (v < s.y[mn]) mn = k;
                    if (v > s.y[mx]) mx = k;
                }
            }
            emitColumn();
            flush();
        } else {
            const bool dedupe = visible > size_t(4 * pw);   // big unordered sets: skip same-pixel repeats
            QPoint lastPx(INT_MIN, INT_MIN);
            for (size_t k = i0; k < i1; ++k) {
                if (!finite(s.x[k]) || !finite(s.y[k])) { flush(); continue; }
                const QPointF q = point(k);
                if (dedupe) {
                    const QPoint r = q.toPoint();
                    if (r == lastPx) continue;
                    lastPx = r;
                }
                pts.push_back(q);
            }
            flush();
        }

        if (m_markers && visible <= 400) {
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            for (size_t k = i0; k < i1; ++k)
                if (finite(s.x[k]) && finite(s.y[k])) p->drawEllipse(point(k), 2.4, 2.4);
            p->setBrush(Qt::NoBrush);
        }
    }
    p->restore();

    // ---- legend (more than one curve)
    if (m_legend && m_series.size() > 1) {
        double total = 0;
        for (const Series& s : m_series) total += fm.horizontalAdvance(s.label) + 30;
        double lx = ml + pw - 8 - total;
        for (size_t i = 0; i < m_series.size(); ++i) {
            p->fillRect(QRectF(lx, mt + 8, 14, 3), colorOf(int(i)));
            p->setPen(m_muted);
            p->drawText(QRectF(lx + 18, mt + 1, 400, 18), Qt::AlignLeft | Qt::AlignVCenter, m_series[i].label);
            lx += fm.horizontalAdvance(m_series[i].label) + 30;
        }
    }

    auto box = [&](const QStringList& lines, double anchorX, double top, bool preferRight, bool leftAligned) {
        double bw = 120;
        for (const QString& l : lines) bw = std::max(bw, fm.horizontalAdvance(l) + 22);
        const double bh = 10 + lines.size() * 16;
        double bx = leftAligned ? anchorX : (preferRight ? anchorX + 12 : anchorX - 12 - bw);
        if (!leftAligned && bx + bw > ml + pw) bx = anchorX - 12 - bw;
        if (bx < ml) bx = leftAligned ? ml : anchorX + 12;
        p->setPen(QPen(m_border, 1));
        p->setBrush(m_surface);
        p->drawRoundedRect(QRectF(bx, top, bw, bh), 6, 6);
        p->setBrush(Qt::NoBrush);
        for (int k = 0; k < lines.size(); ++k) {
            p->setPen(k == 0 ? m_muted : m_text);
            p->drawText(QRectF(bx + 10, top + 5 + k * 16, bw - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, lines[k]);
        }
    };

    // ---- measurement cursors
    if (m_cursors) {
        auto inView = [&](double x) { return x >= m_x0 && x <= m_x1; };
        if (!m_cursorsPlaced || (!inView(m_cx[0]) && !inView(m_cx[1]))) {
            m_cx[0] = m_x0 + (m_x1 - m_x0) / 3;
            m_cx[1] = m_x0 + (m_x1 - m_x0) * 2 / 3;
            m_cursorsPlaced = true;
        }
        QPen dash(m_accent, 1.2, Qt::DashLine);
        for (int k = 0; k < 2; ++k) {
            if (!inView(m_cx[k])) continue;
            const double x = toPx(m_cx[k]);
            p->setPen(dash);
            p->drawLine(QLineF(x, mt, x, mt + ph));
            p->setPen(m_accent);
            p->drawText(QRectF(x + 3, mt, 20, 14), Qt::AlignLeft | Qt::AlignTop, QString::number(k + 1));
        }
        const double dx = m_cx[1] - m_cx[0];
        QStringList lines{QStringLiteral("光标"),
                          "X1  " + xText(m_cx[0], 5), "X2  " + xText(m_cx[1], 5),
                          "ΔX  " + si(dx, ago ? QStringLiteral("s") : m_xUnit, 5)};
        if ((ago || m_xUnit == QLatin1String("s")) && dx != 0) lines << "1/ΔX  " + si(1 / std::abs(dx), "Hz", 5);
        for (const Series& s : m_series) {
            const double d = valueAt(s, m_cx[1]) - valueAt(s, m_cx[0]);
            if (finite(d)) lines << s.label + "  Δ " + si(d, s.unit, 5);
        }
        box(lines, ml + 8, mt + 8, true, true);
    }

    // ---- rubber band
    if (m_drag == Drag::Box) {
        QRectF r = QRectF(m_press, m_last).normalized().intersected(m_plot);
        if (r.width() < kMinBox && r.height() >= kMinBox) r = QRectF(ml, r.top(), pw, r.height());
        else if (r.height() < kMinBox && r.width() >= kMinBox) r = QRectF(r.left(), mt, r.width(), ph);
        QColor fill = m_accent;
        fill.setAlphaF(0.12f);
        p->fillRect(r, fill);
        p->setPen(QPen(m_accent, 1));
        p->drawRect(r);
    }

    // ---- hover readout
    const bool hovering = m_drag == Drag::None && m_plot.contains(m_hover);
    if (hovering) {
        const double xh = fromPx(m_hover.x());
        double best = NaN, bd = INFINITY;
        for (const Series& s : m_series) {
            const long k = nearest(s, xh);
            if (k >= 0 && finite(s.x[size_t(k)]) && std::abs(s.x[size_t(k)] - xh) < bd) {
                bd = std::abs(s.x[size_t(k)] - xh);
                best = s.x[size_t(k)];
            }
        }
        if (finite(best)) {
            const double hx = toPx(best);
            p->setRenderHint(QPainter::Antialiasing, false);
            p->setPen(QPen(m_cursor, 1));
            p->drawLine(QLineF(std::round(hx), mt, std::round(hx), mt + ph));
            p->setRenderHint(QPainter::Antialiasing, true);
            QStringList lines{ago ? xText(best, 5) : (m_xLabel.isEmpty() ? QStringLiteral("x") : m_xLabel) + "  " + xText(best, 5)};
            for (size_t i = 0; i < m_series.size(); ++i) {
                const Series& s = m_series[i];
                const long k = nearest(s, best);
                if (k < 0) continue;
                const double v = s.y[size_t(k)];
                if (!finite(v) || std::abs(s.x[size_t(k)] - best) > (m_x1 - m_x0) * 0.02) continue;
                const double y = toPy(s.axis, v);
                if (y >= mt - 4 && y <= mt + ph + 4) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(colorOf(int(i)));
                    p->drawEllipse(QPointF(toPx(s.x[size_t(k)]), y), 4, 4);
                    p->setBrush(Qt::NoBrush);
                }
                lines << s.label + "  " + si(v, s.unit, 6);
            }
            box(lines, hx, mt + 6, true, false);
        }
    }

    // ---- zoom state / how-to hint
    QString hint;
    if (m_zoomAxis == QLatin1String("x")) hint = QStringLiteral("X 轴已选中：滚轮只缩放 X · 再点 X 轴取消");
    else if (m_zoomAxis == QLatin1String("y")) hint = QStringLiteral("Y 轴已选中：滚轮只缩放 Y · 再点 Y 轴取消");
    else if (hovering && m_interactive && !m_cursors)
        hint = QStringLiteral("点 X 轴或 Y 轴后滚轮只缩放该轴 · 拖框放大 · 右键平移");

    // "自适应" button: back to automatic ranges (always shown, emphasised when zoomed)
    m_fitBtn = QRectF();
    if (m_interactive) {
        const QString label = QStringLiteral("自适应");
        const double bw = fm.horizontalAdvance(label) + 20, bh = 22;
        m_fitBtn = QRectF(ml + pw - bw - 8, mt + ph - bh - 8, bw, bh);
        const bool zoomed = !autoRange();
        const bool hot = m_fitBtn.contains(m_hover);
        QColor fill = zoomed ? m_accent : m_surface;
        if (zoomed && hot) fill = fill.darker(110);
        p->setPen(QPen(zoomed ? m_accent : (hot ? m_muted : m_border), 1));
        p->setBrush(fill);
        p->drawRoundedRect(m_fitBtn, 5, 5);
        p->setBrush(Qt::NoBrush);
        p->setPen(zoomed ? QColor(Qt::white) : (hot ? m_text : m_muted));
        p->drawText(m_fitBtn, Qt::AlignCenter, label);
    }
    if (!hint.isEmpty()) {
        p->setPen(m_faint);
        p->drawText(QRectF(ml + 8, mt + ph - 20, pw - 16 - m_fitBtn.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter, hint);
    }

    m_lastPaintMs = timer.nsecsElapsed() / 1e6;
}

// ---------------------------------------------------------------- input

PlotView::Region PlotView::regionAt(const QPointF& p) const
{
    if (m_plot.isEmpty()) return Region::None;
    if (m_plot.contains(p)) return Region::Plot;
    const bool inY = p.y() >= m_plot.top() && p.y() <= m_plot.bottom();
    const bool inX = p.x() >= m_plot.left() && p.x() <= m_plot.right();
    if (inX && p.y() > m_plot.bottom()) return Region::XAxis;
    if (inY && p.x() < m_plot.left()) return Region::YLeft;
    if (inY && p.x() > m_plot.right() && hasRight()) return Region::YRight;
    return Region::None;
}

void PlotView::updateCursorShape(Region r)
{
    switch (r) {
    case Region::Plot:   setCursor(Qt::CrossCursor); break;
    case Region::XAxis:
    case Region::YLeft:
    case Region::YRight: setCursor(Qt::PointingHandCursor); break;   // click to select, drag to scale
    default:             unsetCursor(); break;
    }
}

void PlotView::mousePressEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const Region r = regionAt(pos);
    if (!m_interactive || r == Region::None) { e->ignore(); return; }
    m_press = m_last = pos;
    m_pressOnFit = false;
    if (e->button() == Qt::LeftButton && m_fitBtn.contains(pos)) {
        m_pressOnFit = true;
        m_drag = Drag::None;
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        // Double click, detected here as well: Qt Quick does not always deliver
        // MouseButtonDblClick to a painted item that grabbed the first press.
        const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
        if (m_clock.isValid() && m_clock.elapsed() < interval && QLineF(pos, m_lastClick).length() < 6) {
            m_clock.invalidate();
            m_drag = Drag::None;
            resetView();
            e->accept();
            return;
        }
        m_clock.start();
        m_lastClick = pos;
        m_drag = Drag::None;
        if (r == Region::Plot && m_cursors) {
            for (int k = 0; k < 2; ++k)
                if (std::abs(toPx(m_cx[k]) - pos.x()) <= 6) { m_drag = Drag::Cursor; m_dragCursor = k; }
        }
        if (m_drag == Drag::None) {
            if (r == Region::Plot) m_drag = Drag::Box;
            else if (r == Region::XAxis) m_drag = Drag::AxisX;
            else { m_drag = Drag::AxisY; m_dragAxis = r == Region::YLeft ? 0 : 1; }
        }
    } else if (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_panX = r == Region::Plot || r == Region::XAxis;
        m_panMask = r == Region::XAxis ? 0 : r == Region::YLeft ? 1 : r == Region::YRight ? 2 : 3;
        setCursor(Qt::ClosedHandCursor);
    } else {
        e->ignore();
        return;
    }
    setKeepMouseGrab(true);
    e->accept();
    update();
}

void PlotView::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const QPointF d = pos - m_last;
    switch (m_drag) {
    case Drag::Box:
        m_last = pos;
        update();
        break;
    case Drag::Pan:
        panAxes(d.x(), d.y(), m_panX, m_panMask);
        m_last = pos;
        break;
    case Drag::AxisX:
        zoomAxes(std::exp(-d.x() / kDragScale), 1, m_press, 0);
        m_last = pos;
        break;
    case Drag::AxisY:
        zoomAxes(1, std::exp(d.y() / kDragScale), m_press, 1 << m_dragAxis);
        m_last = pos;
        break;
    case Drag::Cursor:
        m_cx[m_dragCursor] = fromPx(std::clamp(pos.x(), m_plot.left(), m_plot.right()));
        update();
        break;
    case Drag::None:
        break;
    }
    m_hover = pos;
    e->accept();
}

void PlotView::mouseReleaseEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    if (m_pressOnFit) {
        m_pressOnFit = false;
        if (m_fitBtn.contains(pos)) resetView();
        e->accept();
        return;
    }
    const bool click = QLineF(m_press, pos).length() < 4;
    if (click && (m_drag == Drag::AxisX || m_drag == Drag::AxisY)) {
        // a click (no drag) on an axis selects it for wheel zoom; clicking it again deselects
        const QString a = m_drag == Drag::AxisX ? QStringLiteral("x") : QStringLiteral("y");
        setZoomAxis(m_zoomAxis == a ? QString() : a);
    }
    if (m_drag == Drag::Box) {
        const QRectF r = QRectF(m_press, pos).normalized().intersected(m_plot);
        const bool zx = r.width() >= kMinBox, zy = r.height() >= kMinBox;
        if (zx) {
            const double lo = fromPx(r.left()), hi = fromPx(r.right());
            if (validSpan(lo, hi)) { m_x0 = lo; m_x1 = hi; m_autoX = false; }
        }
        if (zy) {
            for (int a = 0; a < (hasRight() ? 2 : 1); ++a) {
                const double lo = fromPy(a, r.bottom()), hi = fromPy(a, r.top());
                if (validSpan(lo, hi)) { m_y0[a] = lo; m_y1[a] = hi; m_autoY = false; }
            }
        }
        if (zx || zy) emit viewChanged();
    }
    m_drag = Drag::None;
    setKeepMouseGrab(false);
    updateCursorShape(regionAt(pos));
    e->accept();
    update();
}

void PlotView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (!m_interactive || regionAt(e->position()) == Region::None) { e->ignore(); return; }
    m_drag = Drag::None;
    resetView();
    e->accept();
}

void PlotView::wheelEvent(QWheelEvent* e)
{
    const QPointF pos = e->position();
    const Region r = regionAt(pos);
    const QPoint ad = e->angleDelta();
    const double steps = (ad.y() != 0 ? ad.y() : ad.x()) / 120.0;   // Shift+wheel arrives as x on some systems
    if (!m_interactive || r == Region::None || steps == 0) { e->ignore(); return; }
    const double f = std::pow(kWheelStep, steps);
    double fx = f, fy = f;
    int mask = 3;
    if (r == Region::XAxis || m_zoomAxis == QLatin1String("x") || (e->modifiers() & Qt::ControlModifier)) fy = 1;
    else if (m_zoomAxis == QLatin1String("y") || (e->modifiers() & Qt::ShiftModifier)) fx = 1;
    if (r == Region::YLeft) { fx = 1; fy = f; mask = 1; }
    if (r == Region::YRight) { fx = 1; fy = f; mask = 2; }
    zoomAxes(fx, fy, pos, mask);
    e->accept();
}

void PlotView::hoverMoveEvent(QHoverEvent* e)
{
    m_hover = e->position();
    if (m_drag == Drag::None) {
        if (m_fitBtn.contains(m_hover)) setCursor(Qt::PointingHandCursor);
        else updateCursorShape(regionAt(m_hover));
    }
    update();
}

void PlotView::hoverLeaveEvent(QHoverEvent*)
{
    m_hover = QPointF(-1, -1);
    unsetCursor();
    update();
}
