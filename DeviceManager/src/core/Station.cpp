#include "Station.h"
#include "PluginRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>

Station::Station(PluginRegistry* registry, QObject* parent)
    : QAbstractListModel(parent), m_registry(registry) {}

Station::~Station() { clearDevices(); }

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
    m_pluginPaths.clear();
    for (const auto& p : s.value("plugin_paths").toArray())
        m_pluginPaths << QDir::cleanPath(base.absoluteFilePath(p.toString()));

    for (const auto& v : s.value("devices").toArray()) {
        const QJsonObject d = v.toObject();
        const QString name = d.value("name").toString();
        const QString plugin = d.value("plugin").toString();
        if (name.isEmpty() || plugin.isEmpty()) continue;

        // Everything except name/plugin is handed to plugin_create as config.
        QJsonObject cfg = d;
        cfg.remove("name");
        cfg.remove("plugin");
        auto* dev = new Device(name, plugin, cfg, this);
        QQmlEngine::setObjectOwnership(dev, QQmlEngine::CppOwnership);
        connect(dev, &Device::log, this, [this, dev](const QString& level, const QString& msg) {
            emit deviceLog(level, dev->name(), msg);
        });
        connect(dev, &Device::statusChanged, this, [this, dev] {
            const int row = int(m_devices.indexOf(dev));
            if (row >= 0) emit dataChanged(index(row), index(row), {StatusRole});
        });
        m_devices.append(dev);
    }
    endResetModel();
    emit loaded();
    return true;
}

bool Station::openDevice(Device* d)
{
    const PluginInfo* info = m_registry->find(d->pluginId());
    if (!info) {
        d->fail(QStringLiteral("插件 '%1' 未找到").arg(d->pluginId()));
        return false;
    }
    d->mergeDefaults(info->defaultConfig);
    QString err;
    auto lib = m_registry->library(d->pluginId(), &err);
    if (!lib) {
        d->fail(QStringLiteral("加载插件 %1 失败：%2").arg(d->pluginId(), err));
        return false;
    }
    if (!d->open(lib)) return false;
    emit deviceLog("info", d->name(), QStringLiteral("已连接 %1 @ %2").arg(d->pluginId(), d->resource()));
    return true;
}

void Station::openAll()
{
    for (Device* d : m_devices) openDevice(d);
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
    }
    return {};
}

QHash<int, QByteArray> Station::roleNames() const
{
    return {{DeviceRole, "device"}, {NameRole, "name"}, {PluginRole, "pluginId"},
            {ResourceRole, "resource"}, {StatusRole, "status"}};
}
