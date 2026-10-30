// PlotView: paint time with 1,000,000-point series and the mouse zoom gestures, driven by real
// QMouseEvent / QWheelEvent through a QQuickWindow (offscreen, software backend).
//   plot_view_test <output dir for PNGs>
#include "../src/app/PlotView.h"

#include <QDir>
#include <QGuiApplication>
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

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL OK", g_failures);
    return g_failures ? 1 : 0;
}
