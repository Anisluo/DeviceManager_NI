#include "AnalysisPlot.h"
#include "PlotMath.h"
#include "qcustomplot.h"

#include <QApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

AnalysisPlot::AnalysisPlot(QQuickItem* parent) : QQuickPaintedItem(parent), m_plot(std::make_unique<QCustomPlot>())
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    m_plot->setAttribute(Qt::WA_DontShowOnScreen);
    // Quick owns input and the rubber band. QCP only draws committed views, avoiding
    // QWidget replots followed by another full toPixmap render on every mouse move.
    m_plot->setInteractions(QCP::Interactions());
    m_plot->setSelectionRectMode(QCP::srmNone);
    m_plot->setPlottingHint(QCP::phFastPolylines, true);
    m_plot->setNotAntialiasedElements(QCP::aePlottables);
    m_plot->legend->setVisible(true);
    connect(this, &AnalysisPlot::styleChanged, this, [this] { applyStyle(); scheduleRender(); });
    connect(m_plot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this, [this] { scheduleRender(); });
    connect(m_plot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this, [this] { scheduleRender(); });
    applyStyle();
}

AnalysisPlot::~AnalysisPlot() = default;

void AnalysisPlot::applyStyle()
{
    m_plot->setBackground(m_background);
    for (auto* axis : {m_plot->xAxis, m_plot->yAxis, m_plot->xAxis2, m_plot->yAxis2}) {
        const QColor color = m_foreground;
        axis->setBasePen(QPen(color, 1)); axis->setTickPen(QPen(color));
        axis->setSubTickPen(QPen(m_grid)); axis->setTickLabelColor(color); axis->setLabelColor(color);
        axis->grid()->setPen(QPen(m_grid, 1, Qt::DotLine));
    }
    m_plot->legend->setBrush(m_background); m_plot->legend->setTextColor(m_foreground);
    m_plot->legend->setBorderPen(QPen(m_grid));
    for (int i = 0; i < m_plot->graphCount(); ++i)
        m_plot->graph(i)->setPen(QPen(m_colors.isEmpty() ? QColor(i ? "#5485AA" : "#D97757") : QColor(m_colors[i % m_colors.size()].toString()), 1.0));
}

void AnalysisPlot::setCapture(const QVariant& value)
{
    const auto cap = plotmath::unwrapMap(value);
    const auto xs = plotmath::toDoubles(cap.value("xs"));
    m_plot->clearGraphs();
    for (const auto& item : plotmath::unwrapList(cap.value("series"))) {
        const auto s = plotmath::unwrapMap(item);
        const auto ys = plotmath::toDoubles(s.value("values"));
        QVector<double> x, y;
        x.reserve(int(std::min(xs.size(), ys.size()))); y.reserve(x.capacity());
        for (size_t i = 0; i < xs.size() && i < ys.size(); ++i)
            if (std::isfinite(xs[i]) && std::isfinite(ys[i])) { x.push_back(xs[i]); y.push_back(ys[i]); }
        auto* graph = m_plot->addGraph(); graph->setName(s.value("label").toString());
        graph->setAdaptiveSampling(true); graph->setData(x, y, true);
        const bool visible = s.contains("visible") ? s.value("visible").toBool() : m_visibility.value(graph->name(),true);
        m_visibility[graph->name()] = visible; graph->setVisible(visible);
        if (!visible) graph->removeFromLegend();
    }
    const QString unit = cap.value("x_unit").toString();
    m_plot->xAxis->setLabel(cap.value("x_label").toString() + (unit.isEmpty() ? "" : " ["+unit+"]"));
    m_plot->yAxis->setLabel(cap.value("y_unit").toString());
    applyStyle(); fit();
}

void AnalysisPlot::fit()
{
    bool hasVisible=false;
    for(int i=0;i<m_plot->graphCount();++i)hasVisible=hasVisible || m_plot->graph(i)->visible();
    if (hasVisible) {
        m_plot->rescaleAxes(true);
        m_plot->xAxis->scaleRange(1.02); m_plot->yAxis->scaleRange(1.10);
    }
    scheduleRender();
}

void AnalysisPlot::setSeriesVisible(int index,bool visible)
{
    if(index<0 || index>=m_plot->graphCount())return;
    auto* graph=m_plot->graph(index);
    if(graph->visible()==visible)return;
    m_visibility[graph->name()]=visible;graph->setVisible(visible);
    if(visible)graph->addToLegend();else graph->removeFromLegend();
    scheduleRender();
}

void AnalysisPlot::scheduleRender()
{
    if (m_pending) return;
    m_pending = true;
    QTimer::singleShot(8, this, [this] { m_pending = false; render(); });
}

void AnalysisPlot::render()
{
    if (width() < 10 || height() < 10) return;
    m_plot->resize(int(width()), int(height()));
    QImage frame = m_plot->toPixmap(int(width()), int(height())).toImage();
    { QMutexLocker lock(&m_mutex); m_frame = std::move(frame); }
    updateAxisMarker();
    update();
    emit frameRendered();
}

void AnalysisPlot::paint(QPainter* painter)
{
    QImage frame;
    QRectF box;
    QLineF axisMarker;
    { QMutexLocker lock(&m_mutex); frame = m_frame; box = m_box; axisMarker = m_axisMarker; }
    if (!frame.isNull()) painter->drawImage(boundingRect(), frame);
    if (!axisMarker.isNull()) {
        painter->setPen(QPen(QColor("#D97757"), 3));painter->drawLine(axisMarker);
    }
    if (!box.isEmpty()) {
        painter->setPen(QPen(QColor("#5485AA"), 1, Qt::DashLine));
        painter->setBrush(QColor(84, 133, 170, 35));
        painter->drawRect(box);
    }
}

void AnalysisPlot::geometryChange(const QRectF& n, const QRectF& o)
{
    QQuickPaintedItem::geometryChange(n, o); scheduleRender();
}

void AnalysisPlot::updateAxisMarker()
{
    const QRectF area=m_plot->axisRect()->rect();
    QLineF line;
    if(m_axis==Axis::X)line=QLineF(area.bottomLeft(),area.bottomRight());
    else if(m_axis==Axis::Y)line=QLineF(area.topLeft(),area.bottomLeft());
    QMutexLocker lock(&m_mutex);m_axisMarker=line;
}

void AnalysisPlot::mousePressEvent(QMouseEvent* e)
{
    const QRectF area = m_plot->axisRect()->rect();
    const auto pos = e->position();
    Axis hit = Axis::None;
    if (pos.x() >= area.left() && pos.x() <= area.right() && pos.y() >= area.bottom() && pos.y() <= height()) hit = Axis::X;
    else if (pos.x() >= 0 && pos.x() <= area.left() && pos.y() >= area.top() && pos.y() <= area.bottom()) hit = Axis::Y;
    if (hit != Axis::None && e->button() == Qt::LeftButton) {
        m_axis = m_axis == hit ? Axis::None : hit;
        updateAxisMarker(); update(); emit axisSelectionChanged(); e->accept(); return;
    }
    if (!area.contains(pos)) { e->ignore(); return; }
    m_drag = e->button() == Qt::LeftButton ? Drag::Box : Drag::Pan;
    if(m_drag==Drag::Pan)setCursor(Qt::ClosedHandCursor);
    m_press = m_last = e->position();
    setKeepMouseGrab(true);
    e->accept();
}
void AnalysisPlot::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag == Drag::None) { e->ignore(); return; }
    if (m_drag == Drag::Box) {
        const QRectF box = QRectF(m_press, e->position()).normalized().intersected(m_plot->axisRect()->rect());
        { QMutexLocker lock(&m_mutex); m_box = box; }
        update(); // Cached graph plus cheap overlay; no QCustomPlot rendering while selecting.
    } else {
        const auto at = e->position();
        m_plot->xAxis->moveRange(m_plot->xAxis->pixelToCoord(m_last.x())-m_plot->xAxis->pixelToCoord(at.x()));
        m_plot->yAxis->moveRange(m_plot->yAxis->pixelToCoord(m_last.y())-m_plot->yAxis->pixelToCoord(at.y()));
        m_last = at;
    }
    e->accept();
}
void AnalysisPlot::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_drag == Drag::Box) {
        const auto box = QRectF(m_press,e->position()).normalized().intersected(m_plot->axisRect()->rect());
        if (box.width() >= 5 && !e->modifiers().testFlag(Qt::ShiftModifier))
            m_plot->xAxis->setRange(m_plot->xAxis->pixelToCoord(box.left()),m_plot->xAxis->pixelToCoord(box.right()));
        if (box.height() >= 5 && !e->modifiers().testFlag(Qt::ControlModifier))
            m_plot->yAxis->setRange(m_plot->yAxis->pixelToCoord(box.bottom()),m_plot->yAxis->pixelToCoord(box.top()));
    }
    { QMutexLocker lock(&m_mutex); m_box = QRectF(); }
    m_drag = Drag::None; setKeepMouseGrab(false); unsetCursor(); update(); e->accept();
}
void AnalysisPlot::mouseUngrabEvent()
{
    { QMutexLocker lock(&m_mutex); m_box=QRectF(); }
    m_drag=Drag::None;setKeepMouseGrab(false);unsetCursor();update();
    QQuickPaintedItem::mouseUngrabEvent();
}
void AnalysisPlot::mouseDoubleClickEvent(QMouseEvent* e) { fit(); e->accept(); }
void AnalysisPlot::wheelEvent(QWheelEvent* e)
{
    const QRectF area = m_plot->axisRect()->rect();
    const auto pos = e->position();
    const bool onSelectedAxis = (m_axis == Axis::X && pos.x() >= area.left() && pos.x() <= area.right() && pos.y() >= area.bottom())
        || (m_axis == Axis::Y && pos.x() <= area.left() && pos.y() >= area.top() && pos.y() <= area.bottom());
    if (!area.contains(pos) && !onSelectedAxis) { e->ignore(); return; }
    const double steps = e->angleDelta().y() ? e->angleDelta().y()/120.0 : e->pixelDelta().y()/40.0;
    const double factor = std::pow(.85, steps);
    const bool zoomX = m_axis == Axis::X || (m_axis == Axis::None && !e->modifiers().testFlag(Qt::ShiftModifier));
    const bool zoomY = m_axis == Axis::Y || (m_axis == Axis::None && !e->modifiers().testFlag(Qt::ControlModifier));
    if (zoomX)
        m_plot->xAxis->scaleRange(factor,m_plot->xAxis->pixelToCoord(e->position().x()));
    if (zoomY)
        m_plot->yAxis->scaleRange(factor,m_plot->yAxis->pixelToCoord(e->position().y()));
    e->accept();
}
void AnalysisPlot::hoverMoveEvent(QHoverEvent* e)
{
    m_hover = QStringLiteral("x = %1    y = %2").arg(m_plot->xAxis->pixelToCoord(e->position().x()), 0, 'g', 8)
        .arg(m_plot->yAxis->pixelToCoord(e->position().y()), 0, 'g', 8);
    emit hoverTextChanged();
}
