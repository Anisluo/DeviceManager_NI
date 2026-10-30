#pragma once

#include "Device.h"

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>

class PluginRegistry;

// The active station (station.json): which devices exist, their plugins and
// resources. Exposed to QML as a list model of Device objects.
class Station : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY loaded)
    Q_PROPERTY(QString description READ description NOTIFY loaded)
    Q_PROPERTY(QString path READ path NOTIFY loaded)
    Q_PROPERTY(QString defaultPage READ defaultPage NOTIFY loaded)
    Q_PROPERTY(QString dataDir READ dataDir NOTIFY loaded)
    Q_PROPERTY(int count READ count NOTIFY loaded)
public:
    enum Roles { DeviceRole = Qt::UserRole + 1, NameRole, PluginRole, ResourceRole, StatusRole };

    Station(PluginRegistry* registry, QObject* parent = nullptr);
    ~Station() override;

    // Parses station.json. Returns false and sets *error on a malformed file.
    bool load(const QString& path, QString* error);
    // Extra plugin roots declared in the station file (absolute paths).
    QStringList pluginPaths() const { return m_pluginPaths; }
    // Creates plugin instances for all devices (after the registry has scanned).
    void openAll();
    void closeAll();
    void reopenPlugin(const QString& pluginId);

    QString name() const { return m_name; }
    QString description() const { return m_description; }
    QString path() const { return m_path; }
    QString defaultPage() const { return m_defaultPage; }
    // Measurement database / exports directory ("" = host default).
    QString dataDir() const { return m_dataDir; }
    int count() const { return int(m_devices.size()); }

    Q_INVOKABLE Device* device(const QString& name) const;
    Q_INVOKABLE QStringList devicesForPlugin(const QString& pluginId) const;
    Q_INVOKABLE bool reopen(const QString& name);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void loaded();
    void deviceLog(const QString& level, const QString& source, const QString& message);

private:
    bool openDevice(Device* d);
    void clearDevices();

    PluginRegistry* m_registry;
    QString m_name, m_description, m_path, m_defaultPage, m_dataDir;
    QStringList m_pluginPaths;
    QList<Device*> m_devices;
};
