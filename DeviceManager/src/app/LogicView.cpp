#include "LogicView.h"
#include "PlotMath.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace plotmath;

namespace {

constexpr double kLabels = 170;         // width of the pin label column
constexpr double kRight = 16;
constexpr double kWheelStep = 0.85;     // zoom factor per wheel notch
constexpr double kMinBox = 6;           // a narrower drag is a click
constexpr double kGrab = 6;             // pixels: grab a cursor, snap to an edge

} // namespace

LogicView::LogicView(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    connect(this, &LogicView::styleChanged, this, [this] { update(); });
}

void LogicView::setData(const QVariant& lanesV, double tEnd)
{
    std::vector<Lane> lanes;
    for (const QVariant& item : unwrapList(lanesV)) {
        const QVariantMap m = unwrapMap(item);
        Lane l;
        l.name = m.value("name").toString();
        l.note = m.value("note").toString();
        l.lo = m.value("lo").toDouble();
        l.hi = m.value("hi").toDouble();
        l.t = toDoubles(m.value("t"));
        l.v = toDoubles(m.value("v"));
        l.t.resize(std::min(l.t.size(), l.v.size()));
        l.v.resize(l.t.size());
        lanes.push_back(std::move(l));
    }
    bool same = lanes.size() == m_lanes.size();
    for (size_t i = 0; same && i < lanes.size(); ++i) same = lanes[i].name == m_lanes[i].name;
    const bool first = m_lanes.empty();
    m_lanes = std::move(lanes);
    m_tEnd = tEnd > 0 ? tEnd : 1;
    if (!same || m_fitted) {                 // another pattern: the old zoom means nothing
        m_t0 = 0;
        m_t1 = m_tEnd;
        m_fitted = true;
    }
    if (!same || first) {
        m_cursor[0] = m_tEnd / 3;
        m_cursor[1] = m_tEnd * 2 / 3;
    }
    emit dataChanged();
    emit viewChanged();
    update();
}

void LogicView::fit()
{
    m_t0 = 0;
    m_t1 = m_tEnd;
    m_fitted = true;
    emit viewChanged();
    update();
}

void LogicView::setSpan(double t0, double t1)
{
    if (!(t1 - t0 > 1e-13)) return;          // well below one vector of the fastest timeset
    m_t0 = t0;
    m_t1 = t1;
    m_fitted = false;
    emit viewChanged();
    update();
}

QRectF LogicView::plotRect() const
{
    return QRectF(kLabels, kRuler, std::max(1.0, width() - kLabels - kRight), std::max(1.0, height() - kRuler - kAxis));
}

double LogicView::toPx(double t) const
{
    const QRectF r = plotRect();
    return r.left() + (t - m_t0) / (m_t1 - m_t0) * r.width();
}

double LogicView::fromPx(double px) const
{
    const QRectF r = plotRect();
    return m_t0 + (px - r.left()) / r.width() * (m_t1 - m_t0);
}

QRectF LogicView::flagRect(int c) const
{
    const QRectF r = plotRect();
    const double x0 = toPx(m_cursor[c]), other = toPx(m_cursor[1 - c]);
    const bool shown = x0 >= r.left() && x0 <= r.right();
    const double w = shown ? 22 : 32;
    double x = std::clamp(x0, r.left() + w / 2, r.right() - w / 2);
    // close together: side by side, the earlier one on the left
    const double gap = x0 - other;
    if (std::abs(gap) < 24) x += (gap < 0 || (gap == 0 && c == 0) ? -1 : 1) * (12 - std::abs(gap) / 2);
    return QRectF(x - w / 2, r.top() - 17, w, 16);
}

double LogicView::snapped(double t, int cursor) const
{
    const double taken = m_cursor[1 - cursor];
    const double reach = kGrab / plotRect().width() * (m_t1 - m_t0);
    double best = t, distance = reach;
    for (const Lane& l : m_lanes) {
        const auto at = std::lower_bound(l.t.begin(), l.t.end(), t);
        for (auto it : {at, at == l.t.begin() ? at : at - 1}) {
            if (it == l.t.end() || it == l.t.begin()) continue;     // the first point is no edge
            if (*it != taken && std::abs(*it - t) < distance) { distance = std::abs(*it - t); best = *it; }
        }
    }
    return best;
}

QColor LogicView::laneColor(int i) const
{
    if (m_seriesColors.isEmpty()) return m_accent;
    const QVariant c = m_seriesColors[i % m_seriesColors.size()];
    return c.metaType() == QMetaType::fromType<QColor>() ? c.value<QColor>() : QColor(c.toString());
}

QColor LogicView::cursorColor(int k) const { return k == 0 ? m_accent : laneColor(1); }

void LogicView::paint(QPainter* p)
{
    const double W = width(), H = height();
    QFont font(m_fontFamily);
    font.setPixelSize(m_fontSize);
    p->setFont(font);
    if (m_lanes.empty()) {
        p->setPen(m_faint);
        p->drawText(QRectF(0, 0, W, H), Qt::AlignCenter, m_emptyText);
        return;
    }
    const QRectF r = plotRect();
    const double laneH = r.height() / double(m_lanes.size());
    const QFontMetricsF fm(font);

    // ---- time grid and labels
    double step = 0;
    const std::vector<double> ticks = ticksIn(m_t0, m_t1, std::max(2, int(r.width() / 110)), &step);
    const QString unit = QStringLiteral("s");
    for (double v : ticks) {
        const double x = std::round(toPx(v));
        p->setPen(QPen(m_grid, 1));
        p->drawLine(QLineF(x, r.top(), x, r.bottom()));
        const QString text = tick(v, step, &unit);
        const double tw = fm.horizontalAdvance(text);
        p->setPen(m_muted);
        p->drawText(QRectF(std::clamp(x - tw / 2, 0.0, W - tw - 1), r.bottom() + 5, tw + 1, 18), Qt::AlignLeft | Qt::AlignTop, text);
    }

    // ---- lanes
    QFont bold = font;
    bold.setBold(true);
    bold.setPixelSize(m_fontSize + 1);
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const Lane& l = m_lanes[i];
        const double top = r.top() + double(i) * laneH, mid = top + laneH / 2;
        const double yHi = top + 12, yLo = top + laneH - 12;
        const double span = l.hi > l.lo ? l.hi - l.lo : 1;
        auto toPy = [&](double v) { return std::round(yLo - (v - l.lo) / span * (yLo - yHi)); };

        p->setPen(QPen(m_grid, 1));
        p->drawLine(QLineF(0, top + laneH, W, top + laneH));
        QPen limit(m_grid, 1, Qt::DashLine);
        p->setPen(limit);
        p->drawLine(QLineF(r.left(), yHi, r.right(), yHi));
        p->drawLine(QLineF(r.left(), yLo, r.right(), yLo));

        p->setFont(bold);
        p->setPen(m_text);
        p->drawText(QRectF(6, mid - 19, 104, 20), Qt::AlignLeft | Qt::AlignVCenter, l.name);
        p->setFont(font);
        p->setPen(m_faint);
        p->drawText(QRectF(6, mid + 1, 106, 18), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(l.note, Qt::ElideRight, 106));
        p->setPen(m_muted);
        p->drawText(QRectF(kLabels - 60, yHi - 9, 54, 18), Qt::AlignRight | Qt::AlignVCenter, si(l.hi, "V", 3));
        p->drawText(QRectF(kLabels - 60, yLo - 9, 54, 18), Qt::AlignRight | Qt::AlignVCenter, si(l.lo, "V", 3));

        if (l.t.empty()) continue;
        p->save();
        p->setClipRect(QRectF(r.left(), top, r.width(), laneH));
        p->setPen(QPen(laneColor(int(i)), 1.5));
        // the point before the view gives the level at its left edge
        auto from = std::upper_bound(l.t.begin(), l.t.end(), m_t0);
        if (from != l.t.begin()) --from;
        size_t k = size_t(from - l.t.begin());
        double y = toPy(l.v[k]), x = std::max(toPx(l.t[k]), r.left());
        std::vector<QPointF> pts{QPointF(x, y)};
        const double end = toPx(std::min(m_t1, m_tEnd));
        for (++k; k < l.t.size() && l.t[k] <= m_t1; ++k) {
            const double nx = toPx(l.t[k]), ny = toPy(l.v[k]);
            pts.emplace_back(nx, y);
            pts.emplace_back(nx, ny);
            y = ny;
        }
        pts.emplace_back(end, y);
        p->drawPolyline(pts.data(), int(pts.size()));
        p->restore();
    }

    p->setPen(QPen(m_axis, 1));
    p->drawLine(QLineF(r.left(), r.top(), r.left(), r.bottom()));
    p->drawLine(QLineF(0, r.top(), W, r.top()));
    p->drawLine(QLineF(0, r.bottom(), W, r.bottom()));

    // ---- time range being dragged
    if (m_drag == Drag::Box) {
        const double a = std::clamp(m_press.x(), r.left(), r.right()), b = std::clamp(m_last.x(), r.left(), r.right());
        QColor fill = m_accent;
        fill.setAlphaF(0.12f);
        p->fillRect(QRectF(std::min(a, b), r.top(), std::abs(a - b), r.height()), fill);
    }

    // ---- hover: the time beside the pointer, unless a flag is there
    if (m_drag == Drag::None && r.contains(m_hover)) {
        p->setPen(QPen(m_axis, 1));
        p->drawLine(QLineF(std::round(m_hover.x()), r.top(), std::round(m_hover.x()), r.bottom()));
        const QString text = si(fromPx(m_hover.x()), "s", 6);
        const double tw = fm.horizontalAdvance(text);
        const QRectF box(std::clamp(m_hover.x() + 6, r.left(), r.right() - tw), 4, tw + 1, 16);
        if (!box.intersects(flagRect(0)) && !box.intersects(flagRect(1))) {
            p->setPen(m_muted);
            p->drawText(box, Qt::AlignLeft | Qt::AlignVCenter, text);
        }
    }

    // ---- cursors, with a flag in the ruler
    for (int c = 0; c < 2; ++c) {
        const double x = std::round(toPx(m_cursor[c]));
        const QColor color = cursorColor(c);
        QString label = QStringLiteral("C%1").arg(c + 1);
        if (x < r.left()) label = QStringLiteral("◀") + label;
        else if (x > r.right()) label += QStringLiteral("▶");
        else {
            p->setPen(QPen(color, 1.2, Qt::DashLine));
            p->drawLine(QLineF(x, r.top(), x, r.bottom()));
        }
        const QRectF flag = flagRect(c);
        p->fillRect(flag, color);
        p->setPen(QColor(Qt::white));
        p->drawText(flag, Qt::AlignCenter, label);
    }
}

void LogicView::mousePressEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const QRectF r = plotRect();
    if (m_lanes.empty() || pos.x() < r.left() || pos.x() > r.right() || pos.y() > r.bottom()) { e->ignore(); return; }
    m_press = m_last = pos;
    m_drag = Drag::None;
    m_dragEither = false;
    if (pos.y() < r.top()) {                 // ruler: a flag grabs its cursor, elsewhere places one
        if (e->button() == Qt::MiddleButton) { e->ignore(); return; }
        m_drag = Drag::Cursor;
        if (flagRect(0).adjusted(-1, -2, 1, 2).contains(pos)) m_dragCursor = 0;
        else if (flagRect(1).adjusted(-1, -2, 1, 2).contains(pos)) m_dragCursor = 1;
        else {
            m_dragCursor = e->button() == Qt::LeftButton ? 0 : 1;
            m_cursor[m_dragCursor] = snapped(fromPx(pos.x()), m_dragCursor);
            emit viewChanged();
        }
    } else if (e->button() == Qt::LeftButton) {
        double grabbed = kGrab + 1;
        for (int c = 0; c < 2; ++c) {
            const double d = std::abs(toPx(m_cursor[c]) - pos.x());
            if (d < grabbed) { grabbed = d; m_dragCursor = c; m_drag = Drag::Cursor; }
        }
        if (m_drag == Drag::None) m_drag = Drag::Box;
        else m_dragEither = std::abs(toPx(m_cursor[0]) - toPx(m_cursor[1])) < 1;
    } else {
        m_drag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
    }
    setKeepMouseGrab(true);
    e->accept();
    update();
}

void LogicView::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const QRectF r = plotRect();
    if (m_drag == Drag::Pan) {
        const double d = (m_last.x() - pos.x()) / r.width() * (m_t1 - m_t0);
        setSpan(m_t0 + d, m_t1 + d);
    } else if (m_drag == Drag::Cursor) {
        if (m_dragEither && pos.x() != m_press.x()) {
            const int left = m_cursor[0] <= m_cursor[1] ? 0 : 1;
            m_dragCursor = pos.x() < m_press.x() ? left : 1 - left;
            m_dragEither = false;
        }
        m_cursor[m_dragCursor] = snapped(fromPx(std::clamp(pos.x(), r.left(), r.right())), m_dragCursor);
        emit viewChanged();
    }
    m_last = pos;
    m_hover = pos;
    e->accept();
    update();
}

void LogicView::mouseReleaseEvent(QMouseEvent* e)
{
    const QRectF r = plotRect();
    if (m_drag == Drag::Box && std::abs(e->position().x() - m_press.x()) >= kMinBox) {
        const double a = fromPx(std::clamp(m_press.x(), r.left(), r.right()));
        const double b = fromPx(std::clamp(e->position().x(), r.left(), r.right()));
        setSpan(std::min(a, b), std::max(a, b));
    }
    m_drag = Drag::None;
    setKeepMouseGrab(false);
    unsetCursor();
    e->accept();
    update();
}

void LogicView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (plotRect().contains(e->position())) fit();
    e->accept();
}

void LogicView::wheelEvent(QWheelEvent* e)
{
    const QPoint ad = e->angleDelta();
    const double steps = (ad.y() != 0 ? ad.y() : ad.x()) / 120.0;
    if (m_lanes.empty() || steps == 0 || !plotRect().contains(e->position())) { e->ignore(); return; }
    const double f = std::pow(kWheelStep, steps), at = fromPx(e->position().x());
    setSpan(at - (at - m_t0) * f, at + (m_t1 - at) * f);
    e->accept();
}

void LogicView::hoverMoveEvent(QHoverEvent* e)
{
    m_hover = e->position();
    const QRectF r = plotRect();
    bool onCursor = false;
    for (double c : m_cursor) onCursor = onCursor || std::abs(toPx(c) - m_hover.x()) <= kGrab;
    if (onCursor && r.contains(m_hover)) setCursor(Qt::SizeHorCursor);
    else if (r.contains(m_hover)) setCursor(Qt::CrossCursor);
    else unsetCursor();
    update();
}

void LogicView::hoverLeaveEvent(QHoverEvent*)
{
    m_hover = QPointF(-1, -1);
    unsetCursor();
    update();
}
