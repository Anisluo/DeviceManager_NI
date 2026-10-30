#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QVariant>

#include <vector>

// XY / trend plot drawn in C++ (QPainter, so it also renders on the software backend).
// Large data stays fast: the visible range is found by binary search and drawn with
// min/max-per-pixel-column decimation, so a million points cost about as much as a thousand.
//
// Mouse: click the X or Y axis to select it (highlighted; click again to deselect), then a
// left-drag box zooms only that axis; with no axis selected a box zooms both (a flat box X
// only, a tall one Y only). The "自适应" button in the plot (or a double click) returns to
// automatic ranges. The wheel zooms around the pointer (Ctrl = X only, Shift = Y only, over
// an axis = that axis); dragging on an axis scales it; right / middle drag pans.
//
//   PlotView { xLabel: "t"; xUnit: "s" }  +  setData(xs, [{ label, values, xs?, unit, color, axis }])
//   trend:   xMode: "ago"; followSpan: 30; append(series, t, v); xNow: Date.now() / 1000
class PlotView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString xLabel MEMBER m_xLabel NOTIFY styleChanged)
    Q_PROPERTY(QString xUnit MEMBER m_xUnit NOTIFY styleChanged)
    Q_PROPERTY(QString xMode MEMBER m_xMode NOTIFY styleChanged)           // "linear" | "ago"
    Q_PROPERTY(bool markers MEMBER m_markers NOTIFY styleChanged)
    Q_PROPERTY(bool legend MEMBER m_legend NOTIFY styleChanged)
    Q_PROPERTY(bool cursors MEMBER m_cursors WRITE setCursors NOTIFY styleChanged)
    Q_PROPERTY(bool interactive MEMBER m_interactive NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor faintColor MEMBER m_faint NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor axisColor MEMBER m_axis NOTIFY styleChanged)
    Q_PROPERTY(QColor surfaceColor MEMBER m_surface NOTIFY styleChanged)
    Q_PROPERTY(QColor borderColor MEMBER m_border NOTIFY styleChanged)
    Q_PROPERTY(QColor cursorColor MEMBER m_cursor NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QVariantList seriesColors MEMBER m_seriesColors NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER m_fontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    Q_PROPERTY(double lineWidth MEMBER m_lineWidth NOTIFY styleChanged)
    Q_PROPERTY(double followSpan MEMBER m_followSpan NOTIFY styleChanged)  // trend window [s]; 0 = off
    Q_PROPERTY(double xNow MEMBER m_xNow NOTIFY styleChanged)              // right edge while following
    Q_PROPERTY(double keepSpan MEMBER m_keepSpan NOTIFY styleChanged)      // append(): history kept
    Q_PROPERTY(bool autoRange READ autoRange NOTIFY viewChanged)
    Q_PROPERTY(QString zoomAxis READ zoomAxis WRITE setZoomAxis NOTIFY viewChanged)   // "" | "x" | "y"
    Q_PROPERTY(int pointCount READ pointCount NOTIFY dataChanged)
    Q_PROPERTY(double lastPaintMs READ lastPaintMs)               // written on the render thread
    // current view, for tests and for pages that want to show it
    Q_PROPERTY(double viewXMin READ viewXMin NOTIFY viewChanged)
    Q_PROPERTY(double viewXMax READ viewXMax NOTIFY viewChanged)
    Q_PROPERTY(double viewYMin READ viewYMin NOTIFY viewChanged)
    Q_PROPERTY(double viewYMax READ viewYMax NOTIFY viewChanged)
public:
    explicit PlotView(QQuickItem* parent = nullptr);

    void paint(QPainter* p) override;

    Q_INVOKABLE void setData(const QVariant& xs, const QVariant& series);
    Q_INVOKABLE void setSeriesSpecs(const QVariant& specs);    // labels / units / colors / axes, no data
    Q_INVOKABLE void append(int series, double x, double y);
    Q_INVOKABLE void clearData();
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void zoom(double fx, double fy, double px, double py);   // factors < 1 zoom in, anchor in pixels
    Q_INVOKABLE void pan(double dxPx, double dyPx);

    // C++ entry point (tests, future native sources): one series without QVariant conversion
    void setSeries(int index, std::vector<double> x, std::vector<double> y, const QString& label,
                   const QString& unit = {});

    bool autoRange() const { return m_autoX && m_autoY; }
    QString zoomAxis() const { return m_zoomAxis; }
    void setZoomAxis(const QString& a);
    QRectF fitButtonRect() const { return m_fitBtn; }
    int pointCount() const;
    double lastPaintMs() const { return m_lastPaintMs; }
    double viewXMin() const { return m_x0; }
    double viewXMax() const { return m_x1; }
    double viewYMin() const { return m_y0[0]; }
    double viewYMax() const { return m_y1[0]; }
    QRectF plotRect() const { return m_plot; }
    void setCursors(bool on);

signals:
    void styleChanged();
    void viewChanged();
    void dataChanged();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void hoverMoveEvent(QHoverEvent* e) override;
    void hoverLeaveEvent(QHoverEvent* e) override;

private:
    struct Series {
        QString label, unit;
        QColor color;                 // invalid: palette color
        int axis = 0;                 // 0 left, 1 right
        std::vector<double> x, y;
        bool monotonic = true;        // x non-decreasing: binary search + decimation
    };
    enum class Region { None, Plot, XAxis, YLeft, YRight };
    enum class Drag { None, Box, Pan, AxisX, AxisY, Cursor };

    void computeAuto(double approxWidth, double plotHeight);
    bool hasData() const;
    bool hasRight() const;
    QColor colorOf(int i) const;
    Region regionAt(const QPointF& p) const;
    double toPx(double x) const;
    double toPy(int axis, double y) const;
    double fromPx(double px) const;
    double fromPy(int axis, double py) const;
    void zoomAxes(double fx, double fy, const QPointF& anchor, int yMask);
    void panAxes(double dx, double dy, bool xAxis, int yMask);
    void updateCursorShape(Region r);
    // nearest sample index of series s to x, -1 when empty
    long nearest(const Series& s, double x) const;
    double valueAt(const Series& s, double x) const;    // linear interpolation (monotonic) or nearest
    QString xText(double x, int digits) const;

    std::vector<Series> m_series;

    // style
    QString m_xLabel, m_xUnit, m_xMode = QStringLiteral("linear");
    bool m_markers = true, m_legend = true, m_cursors = false, m_interactive = true;
    QColor m_text = QColor("#29261B"), m_muted = QColor("#6E6A5E"), m_faint = QColor("#9A9587"),
           m_grid = QColor("#E9E6DC"), m_axis = QColor("#C9C4B6"), m_surface = QColor("#FBFAF7"),
           m_border = QColor("#E3DFD3"), m_cursor = QColor("#CFCABB"), m_accent = QColor("#D97757");
    QVariantList m_seriesColors;
    QString m_fontFamily = QStringLiteral("Segoe UI");
    int m_fontSize = 11;
    QString m_emptyText = QStringLiteral("暂无数据");
    double m_lineWidth = 1.6;
    double m_followSpan = 0, m_xNow = 0, m_keepSpan = 600;

    // view
    bool m_autoX = true, m_autoY = true;
    double m_x0 = 0, m_x1 = 1, m_y0[2] = {0, 0}, m_y1[2] = {1, 1};
    double m_xRef = 0;                 // "ago" labels are relative to this
    QRectF m_plot;                     // plot area of the last paint
    QRectF m_fitBtn;                   // "自适应" button of the last paint
    QString m_zoomAxis;                // axis selected by a click: "" | "x" | "y"
    bool m_pressOnFit = false;
    double m_lastPaintMs = 0;

    // interaction
    Drag m_drag = Drag::None;
    int m_dragAxis = 0, m_dragCursor = 0, m_panMask = 3;
    bool m_panX = true;
    QPointF m_press, m_last, m_hover{-1, -1};
    double m_cx[2] = {0, 0};          // measurement cursors (x values)
    bool m_cursorsPlaced = false;
    QElapsedTimer m_clock;            // double-click detection
    QPointF m_lastClick;
};
