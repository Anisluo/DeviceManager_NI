#include "app/AppController.h"
#include "app/PlotView.h"
#include "app/SyntaxHighlighter.h"
#include "app/WindowChrome.h"
#include "core/Device.h"
#include "python/PythonRuntime.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>

static QStringList existing(const QStringList& dirs)
{
    QStringList out;
    for (const QString& d : dirs) {
        const QString c = QDir::cleanPath(d);
        if (!c.isEmpty() && QFileInfo(c).isDir() && !out.contains(c)) out << c;
    }
    return out;
}

// Python installation matching the python3XY.dll the host was built against:
// DM_PYTHONHOME, the build machine's path, the Windows registry (python.org installer), common folders.
static QString findPythonHome()
{
    const QString ver = QStringLiteral(DM_PYTHON_VERSION);             // e.g. "3.13"
    QStringList candidates;
    if (qEnvironmentVariableIsSet("DM_PYTHONHOME")) candidates << qEnvironmentVariable("DM_PYTHONHOME");
    candidates << QStringLiteral(DM_PYTHON_HOME);
#ifdef Q_OS_WIN
    for (const char* root : {"HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE"}) {
        QSettings reg(QStringLiteral("%1\\Software\\Python\\PythonCore\\%2\\InstallPath").arg(root, ver),
                      QSettings::NativeFormat);
        const QString p = reg.value(".").toString();
        if (!p.isEmpty()) candidates << p;
    }
    const QString nodot = QString(ver).remove('.');
    candidates << qEnvironmentVariable("LOCALAPPDATA") + "/Programs/Python/Python" + nodot
               << "C:/Program Files/Python" + nodot << "C:/Python" + nodot;
#endif
    for (const QString& c : candidates)
        if (!c.isEmpty() && QFileInfo::exists(QDir(c).filePath("Lib/os.py"))) return QDir::cleanPath(c);
    return candidates.value(0);
}

// Items created by Repeater/Loader are linked through the visual tree, not QObject children.
static QQuickItem* findItem(QQuickItem* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (QQuickItem* c : root->childItems())
        if (QQuickItem* hit = findItem(c, name)) return hit;
    return nullptr;
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName("ChipTest");
    QGuiApplication::setApplicationName("DeviceManager");
    QGuiApplication::setApplicationVersion(DM_VERSION);
    QQuickStyle::setStyle("Basic");
    QIcon appIcon;
    for (int s : {16, 24, 32, 48, 64, 128, 256})
        appIcon.addFile(QStringLiteral(":/icons/app_%1.png").arg(s), QSize(s, s));
    QGuiApplication::setWindowIcon(appIcon);          // window, taskbar, Alt+Tab

    QCommandLineParser cli;
    cli.setApplicationDescription("插件化芯片测控系统");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption stationOpt({"s", "station"}, "工位配置文件 station.json", "file");
    QCommandLineOption viewOpt("view", "启动后显示的页面：station | device:<name> | plugin:<id>", "view");
    QCommandLineOption themeOpt("theme", "light | dark", "theme");
    QCommandLineOption shotOpt("screenshot", "截图到 <png> 并退出（同时写出 <png>.log），用于 CI", "png");
    QCommandLineOption delayOpt("screenshot-delay", "截图前等待毫秒数", "ms", "3000");
    QCommandLineOption actionOpt("action",
        "驱动页面控件 <id>=<value>[@ms]，等同用户输入；可重复（用于 CI / 演示）", "spec");
    QCommandLineOption runOpt("run", "启动后运行序列脚本 <file>[@ms]（相对 sequences/）", "file");
    QCommandLineOption consoleOpt("console", "启动时展开底部面板：log | script", "tab");
    QCommandLineOption nativeFrameOpt("native-frame", "使用系统标题栏（不绘制自定义标题栏）");
    cli.addOptions({stationOpt, viewOpt, themeOpt, shotOpt, delayOpt, actionOpt, runOpt, consoleOpt, nativeFrameOpt});
    cli.process(app);

    // Layout: <prefix>/bin/DeviceManager.exe, <prefix>/devices, <prefix>/stations.
    // In a build tree the source devices/ dir is searched first so page.json edits
    // show up live, and the built libraries are found under build/devices/<id>/.
    // An installed copy (cmake --install / release zip) has <prefix>/stations and ignores the
    // source tree it was built from, unless DM_DEV=1.
    const QString exeDir = QCoreApplication::applicationDirPath();
    const bool installed = QFileInfo(exeDir + "/../stations").isDir() && qEnvironmentVariable("DM_DEV") != "1";
    auto dev = [installed](const char* path) { return installed ? QString() : QString::fromUtf8(path); };
    const QStringList pluginRoots = existing({dev(DM_DEVICES_SOURCE_DIR), exeDir + "/../devices"});
    const QStringList binaryRoots = existing({dev(DM_DEVICES_BUILD_DIR), exeDir + "/../devices"});
    QString stationsDir = existing({dev(DM_STATIONS_SOURCE_DIR), exeDir + "/../stations"}).value(0);
    QString sequencesDir = existing({dev(DM_SEQUENCES_SOURCE_DIR), exeDir + "/../sequences"}).value(0);

    // Embedded Python. Declared before the controller so it is destroyed after every
    // Python object (plugins, script namespaces) the controller owns.
    // matplotlib (reports) renders off-screen and keeps its font cache next to the build.
    qputenv("MPLBACKEND", "Agg");
    if (!qEnvironmentVariableIsSet("MPLCONFIGDIR")) qputenv("MPLCONFIGDIR", QFile::encodeName(exeDir + "/.mplconfig"));
    PythonRuntime python;
    const QString pyHome = findPythonHome();
    QString pyError;
    python.start(pyHome, existing({dev(DM_SDK_PYTHON_SOURCE_DIR), exeDir + "/../sdk/python"}), &pyError);

    AppController controller(pluginRoots, binaryRoots, exeDir + "/.shadow", stationsDir, sequencesDir, &python);
    LogModel::installMessageHandler(controller.log());
    controller.setDefaultDataDir(QDir::cleanPath(exeDir + "/../data"));
    if (python.isRunning())
        controller.log()->append("info", "python", QStringLiteral("Python %1 已就绪（%2）").arg(python.version(), pyHome));
    else
        controller.log()->append("error", "python", pyError);
    if (cli.isSet(themeOpt)) controller.setDarkTheme(cli.value(themeOpt) == "dark");

    QQmlApplicationEngine engine;
    Device::setJsEngine(&engine);
    qmlRegisterUncreatableType<Device>("DeviceManager.Core", 1, 0, "Device", "created by the station");
    qmlRegisterUncreatableType<Station>("DeviceManager.Core", 1, 0, "Station", "singleton");
    qmlRegisterUncreatableType<PluginRegistry>("DeviceManager.Core", 1, 0, "PluginRegistry", "singleton");
    qmlRegisterUncreatableType<LogModel>("DeviceManager.Core", 1, 0, "LogModel", "singleton");
    qmlRegisterUncreatableType<ScriptEngine>("DeviceManager.Core", 1, 0, "ScriptEngine", "singleton");
    qmlRegisterUncreatableType<DataStore>("DeviceManager.Core", 1, 0, "DataStore", "singleton");
    qmlRegisterType<SyntaxHighlighter>("DeviceManager.Core", 1, 0, "SyntaxHighlighter");
    qmlRegisterType<PlotView>("DeviceManager.Core", 1, 0, "PlotView");
    qmlRegisterSingletonInstance("DeviceManager.Core", 1, 0, "App", &controller);
    WindowChrome chrome;
    qmlRegisterSingletonInstance("DeviceManager.Core", 1, 0, "WindowChrome", &chrome);

    QString station = cli.value(stationOpt);
    if (station.isEmpty()) {
        const QString last = QSettings().value("station/last").toString();
        station = QFileInfo::exists(last) ? last : QDir(stationsDir).filePath("sim_bench.json");
    }
    controller.loadStation(station);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{"startView", cli.value(viewOpt)}, {"startConsole", cli.value(consoleOpt)}});
    engine.loadFromModule("DeviceManager", "Main");
    // Themed title bar on Windows (native snap / resize / shadow kept); shows the window.
    chrome.attach(qobject_cast<QQuickWindow*>(engine.rootObjects().value(0)),
                  !cli.isSet(nativeFrameOpt) && qEnvironmentVariable("DM_NATIVE_FRAME") != "1");

    for (const QString& spec : cli.values(actionOpt)) {
        // "vset=1.2@1500" -> after 1500 ms call apply("1.2") on the widget with id "vset"
        const int eq = spec.indexOf('='), at = spec.lastIndexOf('@');
        const QString id = spec.left(eq);
        const QString value = spec.mid(eq + 1, at > eq ? at - eq - 1 : -1);
        const int delay = at > eq ? spec.mid(at + 1).toInt() : 1500;
        QTimer::singleShot(delay, &app, [&engine, id, value] {
            auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
            QQuickItem* w = win ? findItem(win->contentItem(), "widget:" + id) : nullptr;
            if (!w) { qWarning("--action: widget '%s' not found", qPrintable(id)); return; }
            QMetaObject::invokeMethod(w, "apply", Q_ARG(QVariant, value));
            qInfo("--action: %s = %s", qPrintable(id), qPrintable(value));
        });
    }

    if (cli.isSet(runOpt)) {
        const QString spec = cli.value(runOpt);
        const int at = spec.lastIndexOf('@');
        const QString file = at > 0 ? spec.left(at) : spec;
        QTimer::singleShot(at > 0 ? spec.mid(at + 1).toInt() : 1000, &app,
                           [&controller, file] { controller.script()->runFile(file); });
    }

    if (cli.isSet(shotOpt)) {
        const QString png = cli.value(shotOpt);
        QTimer::singleShot(cli.value(delayOpt).toInt(), &app, [&engine, &controller, png] {
            if (auto* win = engine.rootObjects().isEmpty() ? nullptr
                          : qobject_cast<QQuickWindow*>(engine.rootObjects().first()))
                win->grabWindow().save(png);
            QFile log(png + ".log");
            if (log.open(QIODevice::WriteOnly | QIODevice::Text)) {
                LogModel* m = controller.log();
                for (int i = 0; i < m->rowCount(); ++i) {
                    const QModelIndex ix = m->index(i);
                    log.write(QStringLiteral("%1 %2 [%3] %4\n")
                                  .arg(m->data(ix, LogModel::TimeRole).toString(),
                                       m->data(ix, LogModel::LevelRole).toString(),
                                       m->data(ix, LogModel::SourceRole).toString(),
                                       m->data(ix, LogModel::MessageRole).toString()).toUtf8());
                }
            }
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
