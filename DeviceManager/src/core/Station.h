#pragma once

#include "Device.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include <functional>

class PluginRegistry;

// The active station: the instruments found in the chassis (or a simulated chassis), the
// test-task instances, and any devices listed by hand. Exposed to QML as a list model of
// Device objects; kind() tells instruments from tasks.
//
// station.json
//   "discover": "sim" | "ni" | ""      where instruments come from ("" = only "devices")
//   "sim_inventory": [ {name, model, resource, slot, options} ]   the simulated chassis
//   "devices": [ {name, plugin, resource, ...} ]                  added by hand
// Instruments get their driver from the plugin whose plugin.json lists the model in "models";
// a model without a driver is still listed (status "nodriver"). Every plugin with
// category "task" gets one instance, named after the plugin id, whose config carries
// "instruments": {role: device name} resolved from the plugin's "requires".
class Station : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY loaded)
    Q_PROPERTY(QString description READ description NOTIFY loaded)
    Q_PROPERTY(QString path READ path NOTIFY loaded)
    Q_PROPERTY(QString defaultPage READ defaultPage NOTIFY loaded)
    Q_PROPERTY(QString dataDir READ dataDir NOTIFY loaded)
    Q_PROPERTY(QString discoverMode READ discoverMode NOTIFY loaded)
    Q_PROPERTY(QString discoverNote READ discoverNote NOTIFY loaded)
    Q_PROPERTY(int count READ count NOTIFY loaded)
    Q_PROPERTY(int instrumentCount READ instrumentCount NOTIFY loaded)
public:
    enum Roles { DeviceRole = Qt::UserRole + 1, NameRole, PluginRole, ResourceRole, StatusRole, KindRole, ModelRole };

    // Returns the instruments found in the chassis as [{name, model, resource, slot, ...}].
    using Discoverer = std::function<QJsonArray(QString* note)>;

    Station(PluginRegistry* registry, QObject* parent = nullptr);
    ~Station() override;

    // Parses station.json. Returns false and sets *error on a malformed file.
    bool load(const QString& path, QString* error);
    void setDiscoverer(Discoverer d) { m_discoverer = std::move(d); }
    // Extra plugin roots declared in the station file (absolute paths).
    QStringList pluginPaths() const { return m_pluginPaths; }
    // Builds the device list (manual + detected + tasks) and opens every device.
    // Call after the registry has scanned.
    void openAll();
    void closeAll();
    void reopenPlugin(const QString& pluginId);

    QString name() const { return m_name; }
    QString description() const { return m_description; }
    QString path() const { return m_path; }
    QString defaultPage() const { return m_defaultPage; }
    // Measurement database / exports directory ("" = host default).
    QString dataDir() const { return m_dataDir; }
    QString discoverMode() const { return m_discover; }
    QString discoverNote() const { return m_discoverNote; }
    int count() const { return int(m_devices.size()); }
    int instrumentCount() const;

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
    Device* addDevice(const QString& name, const QString& plugin, QJsonObject cfg);
    QString driverForModel(const QString& model) const;
    void addInstruments(const QJsonArray& found, bool simulate);
    void addTasks();
    bool openDevice(Device* d);
    void clearDevices();

    PluginRegistry* m_registry;
    Discoverer m_discoverer;
    QString m_name, m_description, m_path, m_defaultPage, m_dataDir, m_discover, m_discoverNote;
    QStringList m_pluginPaths;
    QJsonArray m_manual, m_simInventory;
    QList<Device*> m_devices;
};
