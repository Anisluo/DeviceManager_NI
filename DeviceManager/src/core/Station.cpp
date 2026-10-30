#include "Station.h"
#include "PluginRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>

Station::Station(PluginRegistry* registry, QObject* parent)
    : QAbstractListModel(parent), m_registry(registry)
{
    Device::setResolver([this](const QString& name) { return device(name); });
    connect(this, &Station::loaded, this, &Station::devicesChanged);
}

Station::~Station()
{
    Device::setResolver({});
    clearDevices();
}

bool Station::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开工位文件 %1").arg(path);
        return false;
    }
    QJsonParseError perr;
    const QJsonObject s = QJsonDocument::fromJson(f.readAll(), &perr).object();
    if (perr.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("%1: %2 (offset %3)").arg(path, perr.errorString()).arg(perr.offset);
        return false;
    }

    beginResetModel();
    clearDevices();
    const QDir base = QFileInfo(path).absoluteDir();
    m_path = QFileInfo(path).absoluteFilePath();
    m_name = s.value("name").toString(QFileInfo(path).baseName());
    m_description = s.value("description").toString();
    m_defaultPage = s.value("default_page").toString();
    m_dataDir = s.contains("data_dir") ? QDir::cleanPath(base.absoluteFilePath(s.value("data_dir").toString())) : QString();
    m_discover = s.value("discover").toString();
    m_discoverNote.clear();
    m_manual = s.value("devices").toArray();
    m_simInventory = s.value("sim_inventory").toArray();
    m_pluginPaths.clear();
    for (const auto& p : s.value("plugin_paths").toArray())
        m_pluginPaths << QDir::cleanPath(base.absoluteFilePath(p.toString()));
    endResetModel();
    emit loaded();
    return true;
}

Device* Station::addDevice(const QString& name, const QString& plugin, QJsonObject cfg)
{
    if (name.isEmpty() || device(name)) return nullptr;
    auto* dev = new Device(name, plugin, std::move(cfg), this);
    QQmlEngine::setObjectOwnership(dev, QQmlEngine::CppOwnership);
    connect(dev, &Device::log, this, [this, dev](const QString& level, const QString& msg) {
        emit deviceLog(level, dev->name(), msg);
    });
    connect(dev, &Device::statusChanged, this, [this, dev] {
        const int row = int(m_devices.indexOf(dev));
        if (row >= 0) emit dataChanged(index(row), index(row), {StatusRole});
    });
    m_devices.append(dev);
    return dev;
}

QString Station::driverForModel(const QString& model) const
{
    for (int i = 0; i < m_registry->rowCount(); ++i) {
        const QString id = m_registry->data(m_registry->index(i), PluginRegistry::IdRole).toString();
        const PluginInfo* p = m_registry->find(id);
        if (!p) continue;
        for (const auto& m : p->manifest.value("models").toArray())
            if (m.toString().compare(model, Qt::CaseInsensitive) == 0) return p->id;
    }
    return {};
}

void Station::addInstruments(const QJsonArray& found, bool simulate)
{
    for (const auto& v : found) {
        QJsonObject cfg = v.toObject();
        const QString name = cfg.value("name").toString(cfg.value("resource").toString());
        const QString driver = cfg.contains("driver") ? cfg.value("driver").toString()
                                                      : driverForModel(cfg.value("model").toString());
        cfg.remove("name");
        cfg.remove("driver");
        cfg.insert("kind", "instrument");
        if (!cfg.contains("simulate")) cfg.insert("simulate", simulate);
        addDevice(name, driver, cfg);
    }
}

void Station::addTasks()
{
    for (int i = 0; i < m_registry->rowCount(); ++i) {
        const QString id = m_registry->data(m_registry->index(i), PluginRegistry::IdRole).toString();
        const PluginInfo* p = m_registry->find(id);
        if (!p || p->category != QLatin1String("task") || !p->error.isEmpty() || device(p->id)) continue;
        // "requires": {"digital": {"models": ["PXIe-6571"]}} -> "instruments": {"digital": "<device>"}
        QJsonObject instruments;
        const QJsonObject req = p->manifest.value("requires").toObject();
        for (auto r = req.begin(); r != req.end(); ++r) {
            QJsonValue pick;                                // null: not found
            const QJsonArray models = r.value().toObject().value("models").toArray();
            for (Device* d : std::as_const(m_devices)) {
                if (d->kind() != QLatin1String("instrument")) continue;
                for (const auto& m : models)
                    if (m.toString().compare(d->model(), Qt::CaseInsensitive) == 0 && pick.isNull()) pick = d->name();
            }
            instruments.insert(r.key(), pick);
        }
        QJsonObject cfg{{"kind", "task"}, {"resource", ""}, {"instruments", instruments},
                        {"model", p->name}, {"simulate", m_discover == QLatin1String("sim")}};
        addDevice(p->id, p->id, cfg);
    }
}

bool Station::openDevice(Device* d)
{
    if (d->pluginId().isEmpty()) {
        d->unsupported(QStringLiteral("检测到 %1，但没有对应的驱动插件").arg(d->model()));
        return false;
    }
    const PluginInfo* info = m_registry->find(d->pluginId());
    if (!info) {
        d->fail(QStringLiteral("插件 '%1' 未找到").arg(d->pluginId()));
        return false;
    }
    d->mergeDefaults(info->defaultConfig);
    d->mergeDefaults(QJsonObject{{"plugin_dir", info->dir}});
    // "uses": ["other_plugin"] -> where that plugin lives, for drivers that load its library
    QJsonObject uses;
    for (const auto& u : info->manifest.value("uses").toArray())
        if (const PluginInfo* other = m_registry->find(u.toString()))
            uses.insert(other->id, QJsonObject{{"dir", other->dir}, {"library", other->libraryPath}});
    if (!uses.isEmpty()) d->mergeDefaults(QJsonObject{{"uses", uses}});
    QString err;
    auto lib = m_registry->library(d->pluginId(), &err);
    if (!lib) {
        d->fail(QStringLiteral("加载插件 %1 失败：%2").arg(d->pluginId(), err));
        return false;
    }
    if (!d->open(lib)) return false;
    emit deviceLog("info", d->name(), d->kind() == QLatin1String("task")
                                          ? QStringLiteral("测试任务 %1 已就绪").arg(info->name)
                                          : QStringLiteral("已连接 %1 @ %2").arg(d->pluginId(), d->resource()));
    return true;
}

void Station::openAll()
{
    beginResetModel();
    clearDevices();
    m_discoverNote.clear();
    // Hand-listed devices first: they keep their names and options.
    for (const auto& v : std::as_const(m_manual)) {
        QJsonObject cfg = v.toObject();
        const QString name = cfg.value("name").toString();
        const QString plugin = cfg.value("plugin").toString();
        if (name.isEmpty() || plugin.isEmpty()) continue;
        cfg.remove("name");
        cfg.remove("plugin");
        const PluginInfo* info = m_registry->find(plugin);
        if (info && info->category == QLatin1String("task")) continue;   // tasks are created below
        if (!cfg.contains("kind")) cfg.insert("kind", "instrument");
        addDevice(name, plugin, cfg);
    }
    if (m_discover == QLatin1String("sim")) {
        addInstruments(m_simInventory, true);
        m_discoverNote = QStringLiteral("仿真机箱：%1 个仪器").arg(m_simInventory.size());
    } else if (m_discover == QLatin1String("ni")) {
        QString note;
        const QJsonArray found = m_discoverer ? m_discoverer(&note) : QJsonArray();
        if (!m_discoverer) note = QStringLiteral("没有可用的检测方法（需要嵌入式 Python）");
        addInstruments(found, false);
        m_discoverNote = note.isEmpty() ? QStringLiteral("检测到 %1 个仪器").arg(found.size()) : note;
        emit deviceLog(found.isEmpty() ? "warning" : "info", "station", m_discoverNote);
    }
    addTasks();
    endResetModel();
    emit loaded();
    for (Device* d : std::as_const(m_devices))
        if (d->kind() != QLatin1String("task")) openDevice(d);
    for (Device* d : std::as_const(m_devices))             // tasks after the instruments they use
        if (d->kind() == QLatin1String("task")) openDevice(d);
}

void Station::addNewTasks()
{
    if (m_path.isEmpty()) return;
    const QList<Device*> before = m_devices;
    beginResetModel();
    addTasks();
    endResetModel();
    emit devicesChanged();
    for (Device* d : std::as_const(m_devices))
        if (!before.contains(d)) openDevice(d);
}

void Station::closeAll()
{
    for (Device* d : m_devices) d->close();
}

void Station::reopenPlugin(const QString& pluginId)
{
    for (Device* d : m_devices)
        if (d->pluginId() == pluginId) {
            d->close();
            openDevice(d);
        }
}

bool Station::reopen(const QString& name)
{
    Device* d = device(name);
    if (!d) return false;
    d->close();
    return openDevice(d);
}

void Station::clearDevices()
{
    for (Device* d : m_devices) d->close();
    qDeleteAll(m_devices);
    m_devices.clear();
}

int Station::instrumentCount() const
{
    int n = 0;
    for (Device* d : m_devices)
        if (d->kind() == QLatin1String("instrument")) ++n;
    return n;
}

Device* Station::device(const QString& name) const
{
    for (Device* d : m_devices)
        if (d->name() == name) return d;
    return nullptr;
}

QStringList Station::devicesForPlugin(const QString& pluginId) const
{
    QStringList names;
    for (Device* d : m_devices)
        if (d->pluginId() == pluginId) names << d->name();
    return names;
}

int Station::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_devices.size());
}

QVariant Station::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_devices.size()) return {};
    Device* d = m_devices.at(index.row());
    switch (role) {
    case DeviceRole:   return QVariant::fromValue(d);
    case NameRole:     return d->name();
    case PluginRole:   return d->pluginId();
    case ResourceRole: return d->resource();
    case StatusRole:   return d->status();
    case KindRole:     return d->kind();
    case ModelRole:    return d->model();
    }
    return {};
}

QHash<int, QByteArray> Station::roleNames() const
{
    return {{DeviceRole, "device"}, {NameRole, "name"}, {PluginRole, "pluginId"},
            {ResourceRole, "resource"}, {StatusRole, "status"}, {KindRole, "kind"}, {ModelRole, "instrumentModel"}};
}
