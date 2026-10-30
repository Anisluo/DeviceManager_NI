#include "PluginRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace {

QStringList libraryCandidates(const QString& entry)
{
#if defined(Q_OS_WIN)
    return {entry + ".dll", "lib" + entry + ".dll"};
#elif defined(Q_OS_MACOS)
    return {entry + ".dylib", "lib" + entry + ".dylib", entry + ".so"};
#else
    return {entry + ".so", "lib" + entry + ".so"};
#endif
}

} // namespace

PluginRegistry::PluginRegistry(QObject* parent) : QAbstractListModel(parent)
{
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &PluginRegistry::onFileChanged);
}

void PluginRegistry::setShadowDir(const QString& dir)
{
    m_shadowDir = dir;
    // Remove shadow copies left behind by a previous run.
    QDir d(dir);
    for (const QString& f : d.entryList(QDir::Files))
        d.remove(f);
}

void PluginRegistry::rescan()
{
    beginResetModel();
    m_plugins.clear();
    QSet<QString> seen;
    for (const QString& root : m_roots) {
        QDir rootDir(root);
        if (!rootDir.exists()) continue;
        const auto subdirs = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& sub : subdirs) {
            const QString dir = rootDir.absoluteFilePath(sub);
            QFile f(QDir(dir).filePath("plugin.json"));
            if (!f.open(QIODevice::ReadOnly)) continue;

            QJsonParseError perr;
            const QJsonObject m = QJsonDocument::fromJson(f.readAll(), &perr).object();
            PluginInfo p;
            p.dir = dir;
            p.id = m.value("id").toString(sub);
            if (seen.contains(p.id)) continue;   // first root wins (source tree before install tree)
            seen.insert(p.id);

            p.manifest = m;
            p.name = m.value("name").toString(p.id);
            p.version = m.value("version").toString();
            p.type = m.value("type").toString("cpp");
            p.entry = m.value("entry").toString(p.id);
            p.category = m.value("category").toString("instrument");
            p.description = m.value("description").toString();
            for (const auto& c : m.value("capabilities").toArray()) p.capabilities << c.toString();
            p.defaultConfig = m.value("default_config").toObject();
            if (perr.error != QJsonParseError::NoError)
                p.error = QStringLiteral("plugin.json: %1").arg(perr.errorString());

            const QString page = QDir(dir).filePath(m.value("page").toString("page.json"));
            if (QFileInfo::exists(page)) p.pagePath = page;
            const QString readme = QDir(dir).filePath("README.md");
            if (QFileInfo::exists(readme)) p.readmePath = readme;

            if (p.type == "cpp") {
                p.libraryPath = resolveLibrary(p);
                if (p.libraryPath.isEmpty() && p.error.isEmpty())
                    p.error = QStringLiteral("library '%1' not found (plugin not built?)").arg(p.entry);
            } else if (p.type == "python") {
                // entry: "module:Class" -> <dir>/module.py
                const QString file = QDir(dir).filePath(p.entry.section(':', 0, 0) + ".py");
                if (QFileInfo::exists(file)) p.libraryPath = QFileInfo(file).absoluteFilePath();
                else if (p.error.isEmpty()) p.error = QStringLiteral("Python 文件不存在：%1").arg(file);
            } else if (p.error.isEmpty()) {
                p.error = QStringLiteral("未知插件类型 '%1'").arg(p.type);
            }
            m_plugins.append(p);
        }
    }
    endResetModel();
    watchFiles();
    emit countChanged();
}

QString PluginRegistry::resolveLibrary(const PluginInfo& p) const
{
    QStringList dirs{p.dir, QDir(p.dir).filePath("bin")};
    for (const QString& b : m_binRoots) dirs << QDir(b).filePath(p.id);
    for (const QString& d : dirs)
        for (const QString& name : libraryCandidates(p.entry)) {
            const QString path = QDir(d).filePath(name);
            if (QFileInfo::exists(path)) return QFileInfo(path).absoluteFilePath();
        }
    return {};
}

void PluginRegistry::watchFiles()
{
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    QStringList files;
    for (const auto& p : m_plugins) {
        if (!p.libraryPath.isEmpty()) files << p.libraryPath;
        if (!p.pagePath.isEmpty()) files << p.pagePath;
        if (p.type == "python")       // helper modules next to the entry file
            for (const QString& f : QDir(p.dir).entryList({"*.py"}, QDir::Files))
                if (QDir(p.dir).filePath(f) != p.libraryPath) files << QDir(p.dir).filePath(f);
    }
    if (!files.isEmpty()) m_watcher.addPaths(files);
}

void PluginRegistry::onFileChanged(const QString& path)
{
    // Linkers and editors write in several steps; wait until the file settles.
    QTimer*& t = m_debounce[path];
    if (!t) {
        t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(600);
        connect(t, &QTimer::timeout, this, [this, path] {
            if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
                m_watcher.addPath(path);   // atomic-save editors replace the file
            for (const auto& p : m_plugins) {
                const bool pythonHelper = p.type == "python" && path.endsWith(".py")
                                          && QFileInfo(path).absolutePath() == QFileInfo(p.dir).absoluteFilePath();
                if (p.libraryPath == path || pythonHelper) {
                    m_libs.remove(p.id);
                    emit message("info", QStringLiteral("插件库已更新：%1，重新加载").arg(p.id));
                    emit libraryChanged(p.id);
                } else if (p.pagePath == path) {
                    emit pageChanged(p.id);
                }
            }
        });
    }
    t->start();
}

const PluginInfo* PluginRegistry::find(const QString& id) const
{
    for (const auto& p : m_plugins)
        if (p.id == id) return &p;
    return nullptr;
}

std::shared_ptr<PluginModule> PluginRegistry::library(const QString& id, QString* error)
{
    if (auto it = m_libs.find(id); it != m_libs.end()) return it.value();
    const PluginInfo* p = find(id);
    if (!p) { if (error) *error = QStringLiteral("unknown plugin '%1'").arg(id); return {}; }
    if (p->libraryPath.isEmpty()) { if (error) *error = p->error; return {}; }

    std::shared_ptr<PluginModule> lib;
    if (p->type == "python") {
        if (!m_pythonLoader) { if (error) *error = QStringLiteral("宿主未启用 Python 支持"); return {}; }
        lib = m_pythonLoader(*p, error);
    } else {
        lib = PluginLibrary::load(p->libraryPath, m_shadowDir, error);
    }
    if (lib) m_libs.insert(id, lib);
    return lib;
}

QVariantMap PluginRegistry::get(const QString& id) const
{
    const PluginInfo* p = find(id);
    if (!p) return {};
    QVariantMap m = p->manifest.toVariantMap();
    m["id"] = p->id;
    m["dir"] = QDir::toNativeSeparators(p->dir);
    m["library"] = QDir::toNativeSeparators(p->libraryPath);
    m["page"] = QDir::toNativeSeparators(p->pagePath);
    m["error"] = p->error;
    return m;
}

QString PluginRegistry::readme(const QString& id) const
{
    const PluginInfo* p = find(id);
    if (!p || p->readmePath.isEmpty()) return {};
    QFile f(p->readmePath);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

int PluginRegistry::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_plugins.size());
}

QVariant PluginRegistry::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_plugins.size()) return {};
    const PluginInfo& p = m_plugins.at(index.row());
    switch (role) {
    case IdRole:           return p.id;
    case NameRole:         return p.name;
    case VersionRole:      return p.version;
    case TypeRole:         return p.type;
    case CategoryRole:     return p.category;
    case DescriptionRole:  return p.description;
    case CapabilitiesRole: return p.capabilities;
    case DirRole:          return p.dir;
    case LibraryRole:      return p.libraryPath;
    case ErrorRole:        return p.error;
    }
    return {};
}

QHash<int, QByteArray> PluginRegistry::roleNames() const
{
    return {{IdRole, "pluginId"}, {NameRole, "name"}, {VersionRole, "version"}, {TypeRole, "type"},
            {CategoryRole, "category"}, {DescriptionRole, "description"},
            {CapabilitiesRole, "capabilities"}, {DirRole, "dir"}, {LibraryRole, "library"},
            {ErrorRole, "error"}};
}
