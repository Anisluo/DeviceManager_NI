#pragma once

#include "PluginLibrary.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

struct PluginInfo {
    QString id;
    QString name;
    QString version;
    QString type;          // cpp | python
    QString entry;
    QString category;      // instrument | task | tool
    QString description;
    QStringList capabilities;
    QString dir;           // directory holding plugin.json
    QString pagePath;      // absolute path of page.json ("" if none)
    QString readmePath;
    QString libraryPath;   // resolved shared library, or the .py file for python plugins
    QJsonObject manifest;  // plugin.json content
    QJsonObject defaultConfig;
    QString error;
};

// Discovers plugins (directories with plugin.json) under a list of roots and
// lazily loads their libraries. Watches libraries and page files so the UI can
// hot-reload after a rebuild or a page.json edit.
class PluginRegistry : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, NameRole, VersionRole, TypeRole, CategoryRole,
                 DescriptionRole, CapabilitiesRole, DirRole, LibraryRole, ErrorRole };

    explicit PluginRegistry(QObject* parent = nullptr);

    void setSearchRoots(const QStringList& roots) { m_roots = roots; }
    void setBinaryRoots(const QStringList& roots) { m_binRoots = roots; }
    void setShadowDir(const QString& dir);
    QStringList searchRoots() const { return m_roots; }

    void rescan();
    int count() const { return int(m_plugins.size()); }
    const PluginInfo* find(const QString& id) const;

    // Loads (or returns the cached) implementation of a plugin.
    std::shared_ptr<PluginModule> library(const QString& id, QString* error);

    using PythonLoader = std::function<std::shared_ptr<PluginModule>(const PluginInfo&, QString*)>;
    void setPythonLoader(PythonLoader loader) { m_pythonLoader = std::move(loader); }
    // Drops the cached library; existing holders keep it alive until released.
    void forgetLibrary(const QString& id) { m_libs.remove(id); }

    Q_INVOKABLE QVariantMap get(const QString& id) const;
    Q_INVOKABLE QString readme(const QString& id) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();
    void libraryChanged(const QString& id);   // shared library rebuilt on disk
    void pageChanged(const QString& id);      // page.json edited on disk
    void message(const QString& level, const QString& text);

private:
    void watchFiles();
    void onFileChanged(const QString& path);
    QString resolveLibrary(const PluginInfo& p) const;

    QStringList m_roots;
    QStringList m_binRoots;
    QString m_shadowDir;
    QList<PluginInfo> m_plugins;
    QHash<QString, std::shared_ptr<PluginModule>> m_libs;
    PythonLoader m_pythonLoader;
    QFileSystemWatcher m_watcher;
    QHash<QString, QTimer*> m_debounce;   // path -> pending notification
};
