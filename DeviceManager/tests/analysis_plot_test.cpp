#include "../src/app/AnalysisPlot.h"
#include "qcustomplot.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QWheelEvent>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <cstdio>

class TestPlot : public AnalysisPlot {
public:
    using AnalysisPlot::wheelEvent;
    using AnalysisPlot::mousePressEvent;
    using AnalysisPlot::mouseMoveEvent;
    using AnalysisPlot::mouseReleaseEvent;
    using AnalysisPlot::mouseUngrabEvent;
};

static void flushFrames()
{
    QEventLoop loop; QTimer::singleShot(35,&loop,&QEventLoop::quit); loop.exec();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc,argv);
    TestPlot view;
    view.setSize(QSizeF(1000,400));
    QVariantList x,y;
    for (int i=0;i<10000;++i) { x.push_back(i*.001); y.push_back(std::sin(i*.01)); }
    QElapsedTimer timer; timer.start();
    view.setCapture(QVariantMap{{"xs",x},{"series",QVariantList{QVariantMap{{"label","signal"},{"values",y}}}}, {"x_label","Time"},{"x_unit","s"}});
    flushFrames();
    auto* plot=view.plot();
    if (plot->graphCount()!=1 || plot->graph(0)->dataCount()!=10000) return 1;
    const double before=plot->xAxis->range().size();
    const QPointF at(plot->axisRect()->rect().center());
    QWheelEvent wheel(at,at,QPoint(),QPoint(0,120),Qt::NoButton,Qt::ControlModifier,Qt::NoScrollPhase,false);
    view.wheelEvent(&wheel); flushFrames();
    if (!(plot->xAxis->range().size()<before)) return 2;
    view.fit(); flushFrames();
    if (std::abs(plot->xAxis->range().size()-before)>1e-9) return 3;
    auto click = [&](QPointF pos) {
        QMouseEvent down(QEvent::MouseButtonPress,pos,pos,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,pos,pos,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        view.mousePressEvent(&down); view.mouseReleaseEvent(&up); flushFrames();
    };
    auto scroll = [&](QPointF pos) {
        QWheelEvent e(pos,pos,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        view.wheelEvent(&e); flushFrames();
    };
    QRectF axes=plot->axisRect()->rect();
    click(QPointF(axes.center().x(),axes.bottom()+8));
    if (view.selectedAxis()!=QStringLiteral("X")) return 8;
    const double fixedY=plot->yAxis->range().size();
    scroll(at);
    if (!(plot->xAxis->range().size()<before) || std::abs(plot->yAxis->range().size()-fixedY)>1e-9) return 9;
    view.fit(); flushFrames();
    axes=plot->axisRect()->rect();
    const QPointF yAxis(axes.left()-8,axes.center().y());
    click(yAxis);
    if (view.selectedAxis()!=QStringLiteral("Y")) return 10;
    scroll(yAxis);
    if (std::abs(plot->xAxis->range().size()-before)>1e-9 || !(plot->yAxis->range().size()<fixedY)) return 11;
    click(yAxis);
    if (view.selectedAxis()!=QStringLiteral("XY")) return 12;
    view.fit(); flushFrames();
    // Reproduce a live drag with event-loop processing between every move. Selecting
    // must not render the complete QCP graph or modify its ranges before release.
    int frames=0;
    QObject::connect(&view,&AnalysisPlot::frameRendered,[&] { ++frames; });
    const QRectF area=plot->axisRect()->rect();
    const QPointF begin=area.topLeft()+QPointF(area.width()*.2,area.height()*.2);
    const QPointF end=area.topLeft()+QPointF(area.width()*.7,area.height()*.7);
    QMouseEvent press(QEvent::MouseButtonPress,begin,begin,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    view.mousePressEvent(&press);
    QElapsedTimer drag; drag.start();
    for (int i=1;i<=200;++i) {
        const QPointF pos=begin+(end-begin)*(i/200.0);
        QMouseEvent move(QEvent::MouseMove,pos,pos,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        view.mouseMoveEvent(&move); QApplication::processEvents();
    }
    if (frames!=0 || std::abs(plot->xAxis->range().size()-before)>1e-9) return 6;
    QMouseEvent release(QEvent::MouseButtonRelease,end,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    view.mouseReleaseEvent(&release); flushFrames();
    if (frames!=1 || !(plot->xAxis->range().size()<before*.6)) return 7;
    std::printf("Box zoom: 200 moves + release, %d graph render, %lld ms\n",frames,drag.elapsed());
    // Visibility toggles preserve ranges and survive new captures. Fit considers
    // only checked curves; unchecking every curve is supported.
    QVariantList tinyX{0.0,1.0,2.0},low{0.0,1.0,0.0},high{0.0,100.0,0.0};
    QVariantMap curves{{"xs",tinyX},{"series",QVariantList{
        QVariantMap{{"label","raw"},{"values",high}},QVariantMap{{"label","filtered"},{"values",low}}}}};
    view.setCapture(curves);flushFrames();
    const double originalSpan=plot->yAxis->range().size();
    view.setSeriesVisible(0,false);flushFrames();
    if(plot->graph(0)->visible() || plot->legend->itemCount()!=1 || std::abs(plot->yAxis->range().size()-originalSpan)>1e-9)return 13;
    view.fit();flushFrames();
    if(plot->yAxis->range().size()>2)return 14;
    view.setCapture(curves);flushFrames();
    if(plot->graph(0)->visible() || !plot->graph(1)->visible())return 15;
    view.setSeriesVisible(1,false);flushFrames();
    if(plot->legend->itemCount()!=0 || plot->graph(1)->visible())return 16;
    view.setSeriesVisible(0,true);flushFrames();
    if(!plot->graph(0)->visible() || plot->legend->itemCount()!=1)return 17;
    const QPointF panAt(plot->axisRect()->rect().center());
    QMouseEvent panDown(QEvent::MouseButtonPress,panAt,panAt,Qt::RightButton,Qt::RightButton,Qt::NoModifier);
    QMouseEvent panUp(QEvent::MouseButtonRelease,panAt,panAt,Qt::RightButton,Qt::NoButton,Qt::NoModifier);
    view.mousePressEvent(&panDown);
    if(view.cursor().shape()!=Qt::ClosedHandCursor)return 18;
    view.mouseReleaseEvent(&panUp);
    if(view.cursor().shape()==Qt::ClosedHandCursor)return 19;
    view.mousePressEvent(&panDown);view.mouseUngrabEvent();
    if(view.cursor().shape()==Qt::ClosedHandCursor)return 20;
    QImage image(1000,400,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
    { QPainter painter(&image);view.paint(&painter); }
    if (image.pixelColor(500,200).alpha()==0) return 4;
    view.setCapture(QVariantMap{}); flushFrames();
    if (plot->graphCount()!=0) return 5;
    std::printf("QCustomPlot integration OK; 10000 points, zoom/reset/cache: %lld ms\n", timer.elapsed());
    return 0;
}
