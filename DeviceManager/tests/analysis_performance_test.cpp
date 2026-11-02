// Real Quick event dispatch -> required QCustomPlot cache -> painted scene -> frameSwapped.
// Offscreen software rendering does not capture or operate the user's desktop.
#include "../src/app/AnalysisPlot.h"
#include "qcustomplot.h"
#include <QApplication>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>

class MeasuredPlot : public AnalysisPlot {
public:
    explicit MeasuredPlot(QQuickItem* parent):AnalysisPlot(parent) {
        connect(this,&AnalysisPlot::frameRendered,this,[this]{++generation;},Qt::DirectConnection);
    }
    std::atomic<int> generation{0}, paintedGeneration{0}, paints{0};
    void paint(QPainter* p) override {
        AnalysisPlot::paint(p);paintedGeneration.store(generation.load());++paints;
    }
};

struct Rig {
    QQuickWindow win;
    MeasuredPlot *time=nullptr,*spectrum=nullptr,*response=nullptr;
    QJsonObject results;
    bool passed=true;
    void mouse(QEvent::Type type,QPointF p,Qt::MouseButton b,Qt::MouseButtons bs) {
        QMouseEvent event(type,p,win.mapToGlobal(p),b,bs,Qt::NoModifier);
        QCoreApplication::sendEvent(&win,&event);
    }
    void click(QPointF p) {mouse(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton);mouse(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton);}
    void wheel(QPointF p,int direction) {
        QWheelEvent event(p,win.mapToGlobal(p),QPoint(),QPoint(0,direction*120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QCoreApplication::sendEvent(&win,&event);
    }
    void settle() {QEventLoop loop;QTimer::singleShot(65,&loop,&QEventLoop::quit);loop.exec();}
    double latency(MeasuredPlot* plot,bool needsGraph,const std::function<void()>& input) {
        const int target=plot->generation.load()+(needsGraph?1:0),baseline=plot->paints.load();
        QElapsedTimer elapsed;QEventLoop loop;QTimer timeout;timeout.setSingleShot(true);
        std::atomic<double> latency{-1};
        auto c=QObject::connect(&win,&QQuickWindow::frameSwapped,&loop,[&]{
            if(plot->paints.load()>baseline && plot->paintedGeneration.load()>=target) {
                if(latency.load()<0)latency.store(elapsed.nsecsElapsed()/1e6);
                QMetaObject::invokeMethod(&loop,"quit",Qt::QueuedConnection);
            }
        },Qt::DirectConnection);
        QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
        elapsed.start();timeout.start(750);input();loop.exec();QObject::disconnect(c);
        const double value=latency.load()<0?750:latency.load();
        if(value>100)passed=false;
        return value;
    }
    void record(const QString& name,const std::vector<double>& values) {
        auto sorted=values;std::sort(sorted.begin(),sorted.end());QJsonArray all;
        for(double v:values)all.append(v);
        results[name]=QJsonObject{{"runs",int(values.size())},{"max_ms",sorted.back()},
            {"p95_ms",sorted[size_t(std::ceil(.95*sorted.size()))-1]},{"median_ms",sorted[sorted.size()/2]},{"samples_ms",all}};
        std::printf("%s: max=%.3f ms, p95=%.3f ms (%zu runs)\n",qPrintable(name),sorted.back(),sorted[size_t(std::ceil(.95*sorted.size()))-1],values.size());
    }
};

int main(int argc,char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen");qputenv("QT_QUICK_BACKEND","software");qputenv("QSG_RENDER_LOOP","basic");
    QApplication app(argc,argv);Rig r;r.win.resize(1600,1080);
    r.time=new MeasuredPlot(r.win.contentItem());r.spectrum=new MeasuredPlot(r.win.contentItem());r.response=new MeasuredPlot(r.win.contentItem());
    for(auto* p:{r.time,r.spectrum,r.response})p->setSize(QSizeF(1600,360));
    r.spectrum->setY(360);r.response->setY(720);
    QVariantList x,a,b; x.reserve(100000);a.reserve(100000);b.reserve(100000);
    for(int i=0;i<100000;++i){x.push_back(i*.000005);a.push_back(std::sin(i*.0314159)+.3*std::sin(i*.6283185));b.push_back(std::sin(i*.0314159));}
    auto cap=[&](const QVariantList& xx,const QVariantList& y0,const QVariantList& y1){return QVariantMap{{"xs",xx},{"series",QVariantList{
        QVariantMap{{"label","raw"},{"values",y0}},QVariantMap{{"label","filtered"},{"values",y1}}}}};};
    r.time->setCapture(cap(x,a,b));
    QVariantList fx,fa,fb;
    for(int i=0;i<50001;++i){fx.push_back(i*2.0);fa.push_back(-80+20*std::sin(i*.003));fb.push_back(-90+15*std::sin(i*.003));}
    r.spectrum->setCapture(cap(fx,fa,fb));r.response->setCapture(cap(fx,fb,fb));
    // No display downsampling: stress all three QCP plots with full-sized data.
    if(r.time->plot()->graph(0)->dataCount()!=100000 || r.spectrum->plot()->graph(0)->dataCount()!=50001)return 2;
    r.win.show();r.settle();
    auto* p=r.spectrum;
    const auto local=[&](QPointF v){return p->mapToScene(v);};
    auto axes=[&]{return QRectF(p->plot()->axisRect()->rect());};
    std::vector<double> selectX,selectY,wheelX,wheelY,boxMove,boxRelease,pan,reset;
    for(int i=0;i<20;++i){
        p->fit();r.settle();
        selectX.push_back(r.latency(p,false,[&]{r.click(local(QPointF(axes().center().x(),axes().bottom()+8)));}));
        if(p->selectedAxis()!=QStringLiteral("X"))return 3;
        double fixed=p->plot()->yAxis->range().size();
        wheelX.push_back(r.latency(p,true,[&]{r.wheel(local(axes().center()),i%2?1:-1);}));
        if(std::abs(p->plot()->yAxis->range().size()-fixed)>1e-6)return 4;
        selectY.push_back(r.latency(p,false,[&]{r.click(local(QPointF(axes().left()-8,axes().center().y())));}));
        fixed=p->plot()->xAxis->range().size();
        wheelY.push_back(r.latency(p,true,[&]{r.wheel(local(QPointF(axes().left()-8,axes().center().y())),i%2?1:-1);}));
        if(std::abs(p->plot()->xAxis->range().size()-fixed)>1e-6)return 5;
        r.click(local(QPointF(axes().left()-8,axes().center().y())));r.settle();
        auto area=axes();auto begin=local(area.topLeft()+QPointF(area.width()*.15,area.height()*.2));
        auto end=local(area.topLeft()+QPointF(area.width()*.85,area.height()*.8));
        r.mouse(QEvent::MouseButtonPress,begin,Qt::LeftButton,Qt::LeftButton);
        const int renders=p->generation.load();
        for(int j=1;j<=3;++j){auto at=begin+(end-begin)*(j/3.0);
            boxMove.push_back(r.latency(p,false,[&]{r.mouse(QEvent::MouseMove,at,Qt::NoButton,Qt::LeftButton);}));}
        if(p->generation.load()!=renders)return 6;
        boxRelease.push_back(r.latency(p,true,[&]{r.mouse(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);}));
        auto center=local(axes().center());r.mouse(QEvent::MouseButtonPress,center,Qt::RightButton,Qt::RightButton);
        pan.push_back(r.latency(p,true,[&]{r.mouse(QEvent::MouseMove,center+QPointF(20,8),Qt::NoButton,Qt::RightButton);}));
        r.mouse(QEvent::MouseButtonRelease,center+QPointF(20,8),Qt::RightButton,Qt::NoButton);
        reset.push_back(r.latency(p,true,[&]{
            r.mouse(QEvent::MouseButtonPress,center,Qt::LeftButton,Qt::LeftButton);
            r.mouse(QEvent::MouseButtonDblClick,center,Qt::LeftButton,Qt::LeftButton);
        }));
        r.mouse(QEvent::MouseButtonRelease,center,Qt::LeftButton,Qt::NoButton);
    }
    r.record("select_x",selectX);r.record("select_y",selectY);r.record("wheel_x",wheelX);r.record("wheel_y",wheelY);
    r.record("box_move",boxMove);r.record("box_release",boxRelease);r.record("pan",pan);r.record("reset",reset);
    QJsonObject report{{"passed",r.passed},{"budget_ms",100},{"source_samples",100000},{"time_graph_points",100000},
        {"spectrum_graph_points",50001},{"curves_per_plot",2},{"plots",3},{"window_width",1600},{"window_height",1080},
        {"qt_version",qVersion()},{"backend","offscreen software / basic render loop"},
        {"measurement","QQuickWindow input dispatch to frameSwapped after required graph-cache generation has been painted; excludes OS input queue and physical monitor scanout"},{"operations",r.results}};
    const QString path=argc>1?QString::fromLocal8Bit(argv[1]):QStringLiteral("analysis_mouse_performance.json");
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if(!file.open(QIODevice::WriteOnly))return 7;
    file.write(QJsonDocument(report).toJson());
    r.win.hide();return r.passed?0:1;
}
