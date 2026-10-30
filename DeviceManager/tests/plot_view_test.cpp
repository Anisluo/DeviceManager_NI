// PlotView: paint time with 1,000,000-point series and the mouse zoom gestures, driven by real
// QMouseEvent / QWheelEvent through a QQuickWindow (offscreen, software backend).
//   plot_view_test <output dir for PNGs>
#include "../src/app/LogicView.h"
#include "../src/app/PlotView.h"

#include <QDir>
#include <QGuiApplication>
#include <QThread>
#include <QImage>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <random>

static int g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_failures; } \
    } while (0)

static bool near(double a, double b, double rel = 1e-6) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)); }

struct Rig {
    QQuickWindow win;
    PlotView* plot = nullptr;
    QString out;

    QImage render() { return win.grabWindow(); }              // full sync + paint
    QRectF pr() const { return plot->plotRect(); }
    double xspan() const { return plot->viewXMax() - plot->viewXMin(); }
    double yspan() const { return plot->viewYMax() - plot->viewYMin(); }

    void mouse(QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
        QMouseEvent e(t, p, win.mapToGlobal(p), b, bs, Qt::NoModifier);
        QCoreApplication::sendEvent(&win, &e);
    }
    void wheel(QPointF p, int dy, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QWheelEvent e(p, win.mapToGlobal(p), QPoint(), QPoint(0, dy), Qt::NoButton, m, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&win, &e);
    }
    void drag(QPointF a, QPointF b, Qt::MouseButton button = Qt::LeftButton) {
        mouse(QEvent::MouseButtonPress, a, button, button);
        mouse(QEvent::MouseMove, (a + b) / 2, Qt::NoButton, button);
        mouse(QEvent::MouseMove, b, Qt::NoButton, button);
        mouse(QEvent::MouseButtonRelease, b, button, Qt::NoButton);
    }
    void doubleClick(QPointF p) {
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
    }
    void reset() { plot->resetView(); render(); }
    void save(const QString& name) { render().save(out + "/" + name); }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QGuiApplication app(argc, argv);

    Rig r;
    r.out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();
    QDir().mkpath(r.out);
    r.win.resize(1200, 420);
    r.win.setColor(QColor("#FBFAF7"));
    r.plot = new PlotView(r.win.contentItem());
    PlotView& plot = *r.plot;
    plot.setSize(QSizeF(1200, 420));
    plot.setProperty("xLabel", "t");
    plot.setProperty("xUnit", "s");
    plot.setProperty("seriesColors", QVariantList{QColor("#D97757"), QColor("#4E79A7")});

    // ---- 2 x 1,000,000 points, 50 Hz sine + noise, one 5 A spike in the middle
    const size_t N = 1000000;
    std::vector<double> x(N), ia(N), ib(N);
    std::mt19937 rng(1);
    std::normal_distribution<double> noise(0, 0.05);
    for (size_t k = 0; k < N; ++k) {
        x[k] = double(k) * 1e-6;
        ia[k] = std::sin(2 * M_PI * 50 * x[k]) + noise(rng);
        ib[k] = std::sin(2 * M_PI * 50 * x[k] - 2 * M_PI / 3) + noise(rng);
    }
    ia[N / 2] = 5.0;
    plot.setSeries(0, x, ia, "ia", "A");
    plot.setSeries(1, x, ib, "ib", "A");
    r.win.show();

    QImage img = r.render();
    img = r.render();                                   // second paint: steady state
    const double paint2M = plot.lastPaintMs();
    std::printf("paint, 2 x 1M points, full view: %.2f ms\n", paint2M);
    CHECK(plot.pointCount() == int(2 * N));
    CHECK(plot.autoRange());
    CHECK(plot.viewXMin() <= 0 && plot.viewXMax() >= 0.999999);
    CHECK(plot.viewYMax() >= 5);
    CHECK(paint2M < 250);
    r.save("plot_full.png");

    // the decimated line still reaches the 5 A spike: series color above the 4 A level
    {
        const QRectF pr = r.pr();
        const double px = pr.left() + 0.5 / (plot.viewXMax() - plot.viewXMin()) * pr.width();
        const double y4 = pr.bottom() - (4.0 - plot.viewYMin()) / r.yspan() * pr.height();
        bool spike = false;
        for (int xx = int(px) - 2; xx <= int(px) + 2 && !spike; ++xx)
            for (int yy = int(pr.top()); yy < int(y4) && !spike; ++yy) {
                const QColor c = img.pixelColor(xx, yy);
                spike = c.red() > 180 && c.green() < 150 && c.blue() < 130;
            }
        CHECK(spike);
    }
    const QPointF c = r.pr().center();

    // ---- wheel: both axes around the pointer
    double xs0 = r.xspan(), ys0 = r.yspan();
    r.wheel(c, 120);
    CHECK(near(r.xspan(), xs0 * 0.85));
    CHECK(near(r.yspan(), ys0 * 0.85));
    CHECK(!plot.autoRange());

    // ---- double click: back to automatic ranges
    r.render();
    r.doubleClick(c);
    r.render();
    CHECK(plot.autoRange());
    CHECK(near(r.xspan(), xs0));

    // ---- Ctrl + wheel: X only; Shift + wheel: Y only
    r.wheel(c, 240, Qt::ControlModifier);
    CHECK(near(r.xspan(), xs0 * 0.85 * 0.85));
    CHECK(near(r.yspan(), ys0));
    r.reset();
    r.wheel(c, 120, Qt::ShiftModifier);
    CHECK(near(r.xspan(), xs0));
    CHECK(near(r.yspan(), ys0 * 0.85));
    r.reset();

    // ---- wheel over the X axis labels: X only; over the Y axis labels: Y only
    r.wheel(QPointF(c.x(), r.pr().bottom() + 10), 120);
    CHECK(near(r.xspan(), xs0 * 0.85));
    CHECK(near(r.yspan(), ys0));
    r.reset();
    r.wheel(QPointF(r.pr().left() - 10, c.y()), 120);
    CHECK(near(r.xspan(), xs0));
    CHECK(near(r.yspan(), ys0 * 0.85));
    r.reset();

    // ---- box zoom: a box zooms both; a flat box only X; a tall box only Y
    {
        const QRectF pr = r.pr();
        r.drag(QPointF(pr.left() + pr.width() * 0.25, pr.top() + pr.height() * 0.25),
               QPointF(pr.left() + pr.width() * 0.75, pr.top() + pr.height() * 0.75));
        CHECK(near(r.xspan(), xs0 * 0.5, 1e-3));
        CHECK(near(r.yspan(), ys0 * 0.5, 1e-3));
        r.render();
        r.save("plot_box_zoom.png");
        r.reset();
        r.drag(QPointF(pr.left() + pr.width() * 0.40, c.y()), QPointF(pr.left() + pr.width() * 0.60, c.y() + 2));
        CHECK(near(r.xspan(), xs0 * 0.2, 1e-3));
        CHECK(near(r.yspan(), ys0));
        r.render();
        const double zoomedMs = plot.lastPaintMs();
        std::printf("paint, X zoomed to 20%% (2 x 200k visible): %.2f ms\n", zoomedMs);
        CHECK(zoomedMs < 250);
        r.reset();
        r.drag(QPointF(c.x(), pr.top() + pr.height() * 0.2), QPointF(c.x() + 2, pr.top() + pr.height() * 0.6));
        CHECK(near(r.xspan(), xs0));
        CHECK(near(r.yspan(), ys0 * 0.4, 1e-3));
        r.reset();
    }

    // ---- dragging an axis scales only that axis
    r.drag(QPointF(c.x(), r.pr().bottom() + 10), QPointF(c.x() + 150, r.pr().bottom() + 10));
    CHECK(near(r.xspan(), xs0 * std::exp(-1.0), 1e-6));
    CHECK(near(r.yspan(), ys0));
    r.reset();
    r.drag(QPointF(r.pr().left() - 10, c.y()), QPointF(r.pr().left() - 10, c.y() - 150));
    CHECK(near(r.xspan(), xs0));
    CHECK(near(r.yspan(), ys0 * std::exp(-1.0), 1e-6));
    r.reset();

    // ---- click an axis to select it; then the wheel zooms only that axis and a box still zooms
    //      both; "自适应" restores
    {
        auto click = [&](QPointF p) {
            r.mouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
            r.mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
            r.render();
        };
        const QRectF pr = r.pr();
        const QPointF a(pr.left() + pr.width() * 0.25, pr.top() + pr.height() * 0.25);
        const QPointF b(pr.left() + pr.width() * 0.75, pr.top() + pr.height() * 0.75);
        click(QPointF(c.x(), pr.bottom() + 10));
        CHECK(plot.zoomAxis() == "x");
        r.save("plot_axis_x_selected.png");
        r.wheel(c, 120);                                   // wheel over the plot, X only
        CHECK(r.xspan() < xs0 * 0.99);
        CHECK(near(r.yspan(), ys0));
        r.render();
        r.save("plot_axis_x_zoomed.png");
        CHECK(!plot.autoRange());
        r.reset();
        r.drag(a, b);                                      // a box still zooms both
        CHECK(near(r.xspan(), xs0 * 0.5, 1e-3));
        CHECK(near(r.yspan(), ys0 * 0.5, 1e-3));
        r.render();
        const QRectF fit = plot.fitButtonRect();
        CHECK(!fit.isEmpty());
        QThread::msleep(500);                                 // not a double click
        click(fit.center());
        CHECK(plot.autoRange());
        CHECK(near(r.xspan(), xs0));
        CHECK(plot.zoomAxis() == "x");                     // the selection stays after a fit
        QThread::msleep(500);
        click(QPointF(pr.left() - 10, c.y()));             // select Y instead
        CHECK(plot.zoomAxis() == "y");
        r.wheel(c, 120);
        CHECK(near(r.xspan(), xs0));
        CHECK(r.yspan() < ys0 * 0.99);
        r.reset();
        QThread::msleep(500);
        click(QPointF(pr.left() - 10, c.y()));             // click again: deselect
        CHECK(plot.zoomAxis().isEmpty());
        r.reset();
        QThread::msleep(500);
    }

    // ---- right drag pans, span unchanged
    {
        const double x0 = plot.viewXMin(), w = r.pr().width();
        r.drag(c, c + QPointF(100, 0), Qt::RightButton);
        CHECK(near(r.xspan(), xs0));
        CHECK(near(plot.viewXMin(), x0 - 100 / w * xs0, 1e-6));
        r.reset();
    }

    // ---- zoom deep into the spike, hover readout and measurement cursors
    {
        const double px = r.pr().left() + 0.5 / r.xspan() * r.pr().width();
        for (int k = 0; k < 20; ++k) r.wheel(QPointF(px, c.y()), 120, Qt::ControlModifier);
        r.render();
        std::printf("paint, deep zoom (%.3g s visible): %.2f ms\n", r.xspan(), plot.lastPaintMs());
        r.mouse(QEvent::MouseMove, QPointF(px + 30, c.y()), Qt::NoButton, Qt::NoButton);   // hover
        r.save("plot_zoom_hover.png");
        plot.setCursors(true);
        r.mouse(QEvent::MouseMove, QPointF(-10, -10), Qt::NoButton, Qt::NoButton);
        r.save("plot_cursors.png");
        plot.setCursors(false);
        r.reset();
    }

    // ---- QML path: setData with QVariant lists (null -> gap)
    {
        QVariantList xs, ys;
        for (int k = 0; k < 1000; ++k) {
            xs << k * 0.001;
            ys << (k == 500 ? QVariant() : QVariant(std::cos(k * 0.01)));
        }
        QVariantMap s{{"label", "v"}, {"unit", "V"}, {"values", ys}};
        plot.setData(xs, QVariantList{s});
        CHECK(plot.pointCount() == 1000);
        CHECK(plot.autoRange());
        r.render();
        r.save("plot_variant.png");
    }

    // ---- trend (follow) mode: Y-only zoom keeps following, X zoom freezes
    {
        plot.setProperty("xMode", "ago");
        plot.setProperty("followSpan", 10.0);
        plot.setProperty("xLabel", "");
        plot.setSeriesSpecs(QVariantList{QVariantMap{{"label", "speed"}, {"unit", "rpm"}}});
        for (int k = 0; k <= 200; ++k) plot.append(0, 1000 + k * 0.1, 1000 + 50 * std::sin(k * 0.1));
        plot.setProperty("xNow", 1020.0);
        r.render();
        CHECK(near(plot.viewXMin(), 1010) && near(plot.viewXMax(), 1020));
        r.wheel(r.pr().center(), 120, Qt::ShiftModifier);
        for (int k = 201; k <= 250; ++k) plot.append(0, 1000 + k * 0.1, 1000);
        plot.setProperty("xNow", 1025.0);
        r.render();
        CHECK(near(plot.viewXMax(), 1025));                  // still following
        r.save("plot_trend.png");
        r.wheel(r.pr().center(), 120, Qt::ControlModifier);
        const double frozen = plot.viewXMax();
        plot.setProperty("xNow", 1030.0);
        r.render();
        CHECK(near(plot.viewXMax(), frozen));                // frozen after an X zoom
        r.doubleClick(r.pr().center());
        r.render();
        CHECK(near(plot.viewXMax(), 1030));
    }

    // ---- LogicView: one strip per pin, time zoom only, two cursors
    {
        plot.setVisible(false);
        auto* logic = new LogicView(r.win.contentItem());
        logic->setSize(QSizeF(1200, 50 + 72 * 3));
        logic->setProperty("seriesColors", QVariantList{QColor("#D97757"), QColor("#4E79A7"), QColor("#59A14F")});
        const auto lane = [](const char* name, double hi, QVariantList t, QVariantList v) {
            return QVariantMap{{"name", name}, {"note", "ch0 · RL"}, {"lo", 0.0}, {"hi", hi}, {"t", t}, {"v", v}};
        };
        // A: rises at 10 us, falls at 20 us; B: rises at 30 us, falls at 70 us; C: undriven halfway
        logic->setData(QVariantList{lane("A", 3.3, {0.0, 10e-6, 20e-6}, {0.0, 3.3, 0.0}),
                                    lane("B", 5.0, {0.0, 30e-6, 70e-6}, {0.0, 5.0, 0.0}),
                                    lane("C", 1.8, {0.0}, {0.9})}, 100e-6);
        r.render();
        CHECK(logic->laneCount() == 3);
        CHECK(near(logic->viewMin() + 1, 1) && near(logic->viewMax(), 100e-6));
        const QRectF lr = logic->plotRect();
        CHECK(near(lr.height(), 72 * 3));
        auto px = [&](double t) { return lr.left() + (t - logic->viewMin()) / (logic->viewMax() - logic->viewMin()) * lr.width(); };

        // wheel: time only, around the pointer
        const double mid = (logic->viewMin() + logic->viewMax()) / 2;
        r.wheel(QPointF(px(mid), lr.center().y()), 120);
        CHECK(logic->viewMax() - logic->viewMin() < 100e-6 * 0.9);
        CHECK(near(logic->viewMin() + logic->viewMax(), 2 * mid, 1e-6));
        logic->fit();

        // left drag: a time range
        r.drag(QPointF(px(25e-6), lr.center().y()), QPointF(px(75e-6), lr.center().y()));
        CHECK(near(logic->viewMin(), 25e-6, 1e-3) && near(logic->viewMax(), 75e-6, 1e-3));
        // right drag pans
        const double span = logic->viewMax() - logic->viewMin(), before = logic->viewMin();
        r.drag(QPointF(px(50e-6), lr.center().y()), QPointF(px(50e-6) + 100, lr.center().y()), Qt::RightButton);
        CHECK(near(logic->viewMax() - logic->viewMin(), span, 1e-6) && logic->viewMin() < before);
        logic->fit();

        // cursors: dragged ones snap to an edge, Δt is exact
        r.render();
        r.drag(QPointF(px(logic->cursor1()), lr.center().y()), QPointF(px(10e-6) + 3, lr.center().y()));
        CHECK(near(logic->cursor1(), 10e-6, 1e-9));
        r.drag(QPointF(px(logic->cursor2()), lr.center().y()), QPointF(px(70e-6) - 3, lr.center().y()));
        CHECK(near(logic->cursor2(), 70e-6, 1e-9));
        CHECK(near(logic->cursor2() - logic->cursor1(), 60e-6, 1e-9));
        r.render();
        r.win.grabWindow().save(r.out + "/logic_view.png");
        // away from an edge the cursor stays where it is put
        r.drag(QPointF(px(logic->cursor2()), lr.center().y()), QPointF(px(45e-6), lr.center().y()));
        CHECK(near(logic->cursor2(), 45e-6, 1e-3));
        // ruler: left places C1, right places C2
        const double ruler = lr.top() - 8;
        r.mouse(QEvent::MouseButtonPress, QPointF(px(30e-6), ruler), Qt::LeftButton, Qt::LeftButton);
        r.mouse(QEvent::MouseButtonRelease, QPointF(px(30e-6), ruler), Qt::LeftButton, Qt::NoButton);
        CHECK(near(logic->cursor1(), 30e-6, 1e-9));
        r.mouse(QEvent::MouseButtonPress, QPointF(px(20e-6), ruler), Qt::RightButton, Qt::RightButton);
        r.mouse(QEvent::MouseButtonRelease, QPointF(px(20e-6), ruler), Qt::RightButton, Qt::NoButton);
        CHECK(near(logic->cursor2(), 20e-6, 1e-9));

        // a flag grabs its own cursor: dragging C2 by its flag leaves C1 alone
        r.mouse(QEvent::MouseButtonPress, QPointF(px(70e-6), ruler), Qt::RightButton, Qt::RightButton);
        r.mouse(QEvent::MouseButtonRelease, QPointF(px(70e-6), ruler), Qt::RightButton, Qt::NoButton);
        r.render();
        CHECK(near(logic->cursor2(), 70e-6, 1e-9));
        r.drag(logic->flagRect(1).center(), QPointF(px(20e-6), ruler));
        CHECK(near(logic->cursor1(), 30e-6, 1e-9) && near(logic->cursor2(), 20e-6, 1e-9));
        // never onto the other cursor's edge, so Δt cannot collapse by snapping
        r.drag(logic->flagRect(1).center(), QPointF(px(30e-6) + 2, ruler));
        CHECK(logic->cursor2() != logic->cursor1());
        r.render();
        CHECK(!logic->flagRect(0).intersects(logic->flagRect(1)));      // both flags stay visible
        // on top of each other: the drag direction picks the cursor
        r.drag(QPointF(px(logic->cursor2()), lr.center().y()), QPointF(px(logic->cursor1()), lr.center().y()));
        const double c1 = logic->cursor1(), c2 = logic->cursor2();
        CHECK(std::abs(px(c1) - px(c2)) < 1);
        r.drag(QPointF(px(c1), lr.center().y()), QPointF(px(c1) + 150, lr.center().y()));
        CHECK(near(logic->cursor1(), c1) || near(logic->cursor2(), c2));
        CHECK(logic->cursor1() != c1 || logic->cursor2() != c2);
        r.render();
        // out of view: the flag stays at the edge
        r.drag(QPointF(px(80e-6), lr.center().y()), QPointF(px(95e-6), lr.center().y()));
        r.render();
        CHECK(logic->flagRect(0).right() <= lr.right() + 1 && logic->flagRect(0).left() >= lr.left() - 1);
        logic->fit();
        r.mouse(QEvent::MouseButtonPress, QPointF(px(30e-6), ruler), Qt::LeftButton, Qt::LeftButton);
        r.mouse(QEvent::MouseButtonRelease, QPointF(px(30e-6), ruler), Qt::LeftButton, Qt::NoButton);

        // a new simulation of the same pins keeps zoom and cursors
        r.drag(QPointF(px(25e-6), lr.center().y()), QPointF(px(55e-6), lr.center().y()));
        const double zoomed = logic->viewMin();
        logic->setData(QVariantList{lane("A", 3.3, {0.0}, {0.0}), lane("B", 5.0, {0.0}, {0.0}), lane("C", 1.8, {0.0}, {0.0})}, 100e-6);
        CHECK(near(logic->viewMin(), zoomed) && near(logic->cursor1(), 30e-6, 1e-9));
        logic->setData(QVariantList{lane("A", 3.3, {0.0}, {0.0})}, 50e-6);       // another pattern
        CHECK(near(logic->viewMax(), 50e-6) && logic->laneCount() == 1);
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL OK", g_failures);
    return g_failures ? 1 : 0;
}
