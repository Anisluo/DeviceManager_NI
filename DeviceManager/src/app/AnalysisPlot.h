#pragma once

#include <QQuickPaintedItem>
#include <QImage>
#include <QMutex>
#include <QHash>
#include <memory>

class QCustomPlot;

// QCustomPlot is a QWidget and stays on the GUI thread. Only its cached QImage crosses
// into Quick's render thread; paint() never accesses the widget or graph data.
class AnalysisPlot : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground MEMBER m_foreground NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QVariantList seriesColors MEMBER m_colors NOTIFY styleChanged)
    Q_PROPERTY(QString hoverText READ hoverText NOTIFY hoverTextChanged)
    Q_PROPERTY(QString selectedAxis READ selectedAxis NOTIFY axisSelectionChanged)
public:
    explicit AnalysisPlot(QQuickItem* parent = nullptr);
    ~AnalysisPlot() override;
    void paint(QPainter* painter) override;
    Q_INVOKABLE void setCapture(const QVariant& capture);
    Q_INVOKABLE void fit();
    Q_INVOKABLE void setSeriesVisible(int index, bool visible);
    QString hoverText() const { return m_hover; }
    QString selectedAxis() const { return m_axis == Axis::X ? QStringLiteral("X") : m_axis == Axis::Y ? QStringLiteral("Y") : QStringLiteral("XY"); }
    QCustomPlot* plot() const { return m_plot.get(); }
signals:
    void styleChanged();
    void hoverTextChanged();
    void frameRendered();
    void axisSelectionChanged();
protected:
    void geometryChange(const QRectF& n, const QRectF& o) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
private:
    void scheduleRender();
    void render();
    void applyStyle();
    void updateAxisMarker();
    std::unique_ptr<QCustomPlot> m_plot;
    QImage m_frame;
    QMutex m_mutex;
    bool m_pending = false;
    enum class Drag { None, Box, Pan };
    Drag m_drag = Drag::None;
    enum class Axis { None, X, Y };
    Axis m_axis = Axis::None;
    QPointF m_press, m_last;
    QRectF m_box;
    QLineF m_axisMarker;
    QString m_hover;
    QColor m_background{"#FBFAF7"}, m_foreground{"#29261B"}, m_grid{"#E9E6DC"};
    QVariantList m_colors;
    QHash<QString,bool> m_visibility;
};
