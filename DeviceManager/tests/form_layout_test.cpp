#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <cstdio>

class ThemeApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkTheme READ darkTheme CONSTANT)
public:
    bool darkTheme() const { return false; }
};

static void settle() { QEventLoop loop; QTimer::singleShot(35,&loop,&QEventLoop::quit);loop.exec(); }

int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");qputenv("QT_QUICK_BACKEND","software");
    QGuiApplication app(argc,argv);ThemeApp themeApp;
    qmlRegisterSingletonInstance("DeviceManager.Core",1,0,"App",&themeApp);
    const QString dir=QStringLiteral(DM_QML_DIR);
    qmlRegisterSingletonType(QUrl::fromLocalFile(dir+"/Theme.qml"),"DeviceManager",1,0,"Theme");
    for(const auto& name:{"DmTextField","DmComboBox","DmSwitch"})
        qmlRegisterType(QUrl::fromLocalFile(dir+"/controls/"+name+".qml"),"DeviceManager",1,0,name);
    QQmlEngine engine;
    const QString code=QStringLiteral(R"(
import QtQuick
import QtQuick.Layouts
import "%1" as Page
Item {
    width: 1800; height: 700
    QtObject {
        id: context
        property var form: ({})
        property QtObject device: QtObject { property bool online: true }
        signal reinitRequested()
        signal flushRequested()
        function read(init, cb) {}
    }
    ColumnLayout {
        anchors.fill: parent
        Page.NumberWidget { objectName: "number"; Layout.fillWidth: true; ctx: context; spec: ({id:"n",label:"采样频率",value:10000,unit:"Hz"}) }
        Page.ComboWidget { objectName: "combo"; Layout.fillWidth: true; ctx: context; spec: ({id:"c",label:"滤波算法",options:["butter","fir"],value:"butter"}) }
        Page.TextWidget { objectName: "text"; Layout.fillWidth: true; ctx: context; spec: ({id:"t",label:"CSV 路径",value:"C:/example/signal.csv"}) }
        Item { Layout.fillHeight: true }
    }
})").arg(QUrl::fromLocalFile(dir+"/page").toString());
    QQmlComponent component(&engine);component.setData(code.toUtf8(),QUrl());
    std::unique_ptr<QObject> rootObject(component.create());
    if(!rootObject){qWarning()<<component.errors();return 1;}
    auto* root=qobject_cast<QQuickItem*>(rootObject.get());QQuickWindow win;
    root->setParentItem(win.contentItem());win.resize(1800,700);win.show();settle();
    for(int width:{1800,640,320,240}){
        win.setWidth(width);root->setWidth(width);settle();
        for(const auto& name:{"number","combo","text"}){
            auto* row=root->findChild<QQuickItem*>(name);
            auto* editor=row?row->findChild<QQuickItem*>("editor"):nullptr;
            if(!editor)return 2;
            const int cap=QString(name)=="number"?200:QString(name)=="combo"?240:440;
            const double right=editor->mapToItem(root,QPointF(editor->width(),0)).x();
            std::printf("width=%d %s: editor %.1f x %.1f, right=%.1f\n",width,name,editor->width(),editor->height(),right);
            if(editor->width()>cap+.1 || editor->height()>34.1 || editor->height()<33.9 || editor->width()<40 || right>width+.1)return 3;
        }
    }
    win.hide();root->setParentItem(nullptr);return 0;
}

#include "form_layout_test.moc"
