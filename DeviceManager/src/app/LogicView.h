#pragma once

#include <QColor>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QVariant>

#include <vector>

// Digital waveform view: one strip per pin on a shared time axis, in the manner of a logic
// analyzer. A strip spans its pin's levels, so only time zooms: wheel around the pointer, left
// drag a time range, right / middle drag pans. Two cursors measure time: drag a cursor or its flag
// (it snaps to a nearby edge, never the other cursor's), or click the ruler above the strips: left
// places cursor 1, right cursor 2. Flags keep apart, and a cursor out of view keeps a flag at the edge.
//
//   LogicView {}  +  setData([{ name, note, lo, hi, t: [s], v: [V] }], tEnd)
// A lane holds its value from one (t, v) point to the next, up to tEnd.
class LogicView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor faintColor MEMBER m_faint NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor axisColor MEMBER m_axis NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QVariantList seriesColors MEMBER m_seriesColors NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER m_fontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    Q_PROPERTY(int laneCount READ laneCount NOTIFY dataChanged)
    Q_PROPERTY(double viewMin READ viewMin NOTIFY viewChanged)
    Q_PROPERTY(double viewMax READ viewMax NOTIFY viewChanged)
    Q_PROPERTY(double cursor1 READ cursor1 NOTIFY viewChanged)
    Q_PROPERTY(double cursor2 READ cursor2 NOTIFY viewChanged)
public:
    static constexpr double kRuler = 24, kAxis = 26, kLane = 72;   // heights in pixels

    explicit LogicView(QQuickItem* parent = nullptr);

    void paint(QPainter* p) override;

    Q_INVOKABLE void setData(const QVariant& lanes, double tEnd);
    Q_INVOKABLE void fit();                  // the whole time range

    int laneCount() const { return int(m_lanes.size()); }
    double viewMin() const { return m_t0; }
    double viewMax() const { return m_t1; }
    double cursor1() const { return m_cursor[0]; }
    double cursor2() const { return m_cursor[1]; }
    QRectF plotRect() const;
    QRectF flagRect(int cursor) const;

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
    struct Lane {
        QString name, note;
        double lo = 0, hi = 1;
        std::vector<double> t, v;
    };
    enum class Drag { None, Box, Pan, Cursor };

    double toPx(double t) const;
    double fromPx(double px) const;
    // the nearest edge within a few pixels that the other cursor is not on, else t
    double snapped(double t, int cursor) const;
    QColor laneColor(int i) const;
    QColor cursorColor(int k) const;
    void setSpan(double t0, double t1);

    std::vector<Lane> m_lanes;
    double m_tEnd = 1, m_t0 = 0, m_t1 = 1;
    bool m_fitted = true;
    double m_cursor[2] = {0, 0};

    QColor m_text = QColor("#29261B"), m_muted = QColor("#6E6A5E"), m_faint = QColor("#9A9587"),
           m_grid = QColor("#E9E6DC"), m_axis = QColor("#C9C4B6"), m_accent = QColor("#D97757");
    QVariantList m_seriesColors;
    QString m_fontFamily = QStringLiteral("Segoe UI");
    int m_fontSize = 11;
    QString m_emptyText = QStringLiteral("暂无数据");

    Drag m_drag = Drag::None;
    int m_dragCursor = 0;
    bool m_dragEither = false;               // cursors on top of each other: the drag direction picks
    QPointF m_press, m_last, m_hover{-1, -1};
};
