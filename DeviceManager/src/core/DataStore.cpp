#include "DataStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>

namespace {

QString now() { return QDateTime::currentDateTime().toString(Qt::ISODateWithMs); }

QString toJson(const QVariant& v)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));
}

} // namespace

DataStore::DataStore(QObject* parent) : QAbstractListModel(parent) {}

DataStore::~DataStore() { closeThreadConnection(); }

void DataStore::closeThreadConnection()
{
    const QString name = QStringLiteral("dm_store_%1").arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name)) {
        QSqlDatabase::database(name, false).close();
        QSqlDatabase::removeDatabase(name);
    }
}

QString DataStore::connectionName() const
{
    const QString name = QStringLiteral("dm_store_%1").arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name) && QSqlDatabase::database(name, false).databaseName() != m_path) {
        QSqlDatabase::database(name, false).close();     // the station switched databases
        QSqlDatabase::removeDatabase(name);
    }
    if (!QSqlDatabase::contains(name)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(m_path);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
        if (db.open()) {
            QSqlQuery q(db);
            q.exec("PRAGMA journal_mode=WAL");
            q.exec("PRAGMA synchronous=NORMAL");
        }
    }
    return name;
}

bool DataStore::open(const QString& path, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (abs == m_path) return true;
    closeThreadConnection();           // UI-thread connection to the previous file
    m_path = abs;
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    if (!db.isOpen()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    const char* schema[] = {
        "CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, kind TEXT,"
        " station TEXT, script TEXT, started TEXT, finished TEXT, status TEXT, columns TEXT DEFAULT '[]',"
        " meta TEXT DEFAULT '{}', points INTEGER DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS points (run_id INTEGER, seq INTEGER, t REAL, data TEXT,"
        " PRIMARY KEY (run_id, seq)) WITHOUT ROWID",
        // A host that crashed mid-run leaves runs in "running": mark them.
        "UPDATE runs SET status = 'aborted' WHERE status = 'running'",
    };
    for (const char* sql : schema)
        if (!q.exec(QString::fromLatin1(sql))) {
            if (error) *error = q.lastError().text();
            return false;
        }
    refresh();
    emit opened();
    return true;
}

QString DataStore::exportDir() const
{
    return m_path.isEmpty() ? QString() : QFileInfo(m_path).absoluteDir().filePath("exports");
}

void DataStore::refresh()
{
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.exec("SELECT id, name, kind, started, status, points, script FROM runs ORDER BY id DESC LIMIT 2000");
    beginResetModel();
    m_runs.clear();
    while (q.next())
        m_runs.append(RunRow{q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(),
                             q.value(4).toString(), q.value(6).toString(), q.value(5).toInt()});
    endResetModel();
    emit countChanged();
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

qint64 DataStore::createRun(const QString& name, const QString& kind, const QString& script, const QVariantMap& meta)
{
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("INSERT INTO runs (name, kind, station, script, started, status, meta) VALUES (?, ?, ?, ?, ?, 'running', ?)");
    q.addBindValue(name);
    q.addBindValue(kind);
    q.addBindValue(m_station);
    q.addBindValue(script);
    q.addBindValue(now());
    q.addBindValue(toJson(meta));
    if (!q.exec()) {
        emit message("error", QStringLiteral("数据库写入失败：%1").arg(q.lastError().text()));
        return 0;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    {
        QMutexLocker lk(&m_mutex);
        m_columns[id] = {};
        m_nextSeq[id] = 0;
    }
    QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    return id;
}

void DataStore::appendPoint(qint64 runId, const QStringList& order, const QVariantMap& values)
{
    if (runId <= 0) return;
    QStringList columnsToSave;
    int seq;
    {
        QMutexLocker lk(&m_mutex);
        QStringList& cols = m_columns[runId];
        bool grew = false;
        for (const QString& c : order)
            if (!cols.contains(c)) { cols << c; grew = true; }
        if (grew) columnsToSave = cols;
        seq = m_nextSeq[runId]++;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    QSqlQuery q(db);
    q.prepare("INSERT INTO points (run_id, seq, t, data) VALUES (?, ?, ?, ?)");
    q.addBindValue(runId);
    q.addBindValue(seq);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch() / 1000.0);
    q.addBindValue(toJson(values));
    if (!q.exec()) {
        emit message("error", QStringLiteral("数据库写入失败：%1").arg(q.lastError().text()));
        return;
    }
    QSqlQuery u(db);
    if (!columnsToSave.isEmpty()) {
        u.prepare("UPDATE runs SET points = ?, columns = ? WHERE id = ?");
        u.addBindValue(seq + 1);
        u.addBindValue(toJson(columnsToSave));
    } else {
        u.prepare("UPDATE runs SET points = ? WHERE id = ?");
        u.addBindValue(seq + 1);
    }
    u.addBindValue(runId);
    u.exec();
    notify(runId);
}

void DataStore::finishRun(qint64 runId, const QString& status)
{
    if (runId <= 0) return;
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("UPDATE runs SET finished = ?, status = ? WHERE id = ?");
    q.addBindValue(now());
    q.addBindValue(status);
    q.addBindValue(runId);
    q.exec();
    {
        QMutexLocker lk(&m_mutex);
        m_columns.remove(runId);
        m_nextSeq.remove(runId);
    }
    QMetaObject::invokeMethod(this, [this, runId] { refresh(); emit runChanged(runId); }, Qt::QueuedConnection);
}

// Coalesce point notifications: at most ~5 model refreshes per second while a scan runs.
void DataStore::notify(qint64 id)
{
    QMetaObject::invokeMethod(this, [this, id] {
        m_dirtyRuns.insert(id);
        if (m_notifyPending) return;
        m_notifyPending = true;
        QTimer::singleShot(200, this, [this] {
            m_notifyPending = false;
            const auto ids = m_dirtyRuns;
            m_dirtyRuns.clear();
            refresh();
            for (qint64 r : ids) emit runChanged(r);
        });
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

QVariantMap DataStore::run(qint64 id) const
{
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("SELECT id, name, kind, station, script, started, finished, status, columns, meta, points FROM runs WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return {};
    QVariantMap m;
    m["id"] = q.value(0);
    m["name"] = q.value(1);
    m["kind"] = q.value(2);
    m["station"] = q.value(3);
    m["script"] = q.value(4);
    m["started"] = q.value(5);
    m["finished"] = q.value(6);
    m["status"] = q.value(7);
    m["columns"] = QJsonDocument::fromJson(q.value(8).toString().toUtf8()).toVariant();
    m["meta"] = QJsonDocument::fromJson(q.value(9).toString().toUtf8()).toVariant();
    m["points"] = q.value(10);
    const QDateTime a = QDateTime::fromString(m["started"].toString(), Qt::ISODateWithMs);
    const QDateTime b = QDateTime::fromString(m["finished"].toString(), Qt::ISODateWithMs);
    m["duration"] = a.isValid() && b.isValid() ? a.msecsTo(b) / 1000.0 : QVariant();
    return m;
}

QStringList DataStore::columns(qint64 id) const
{
    return run(id).value("columns").toStringList();
}

QVariantList DataStore::column(qint64 id, const QString& name) const
{
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("SELECT json_extract(data, ?) FROM points WHERE run_id = ? ORDER BY seq");
    q.addBindValue(QStringLiteral("$.\"%1\"").arg(QString(name).replace('"', "\\\"")));
    q.addBindValue(id);
    q.exec();
    QVariantList out;
    while (q.next()) out << q.value(0);
    return out;
}

QVariantList DataStore::rows(qint64 id, int offset, int limit) const
{
    const QStringList cols = columns(id);
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("SELECT data FROM points WHERE run_id = ? ORDER BY seq LIMIT ? OFFSET ?");
    q.addBindValue(id);
    q.addBindValue(limit);
    q.addBindValue(offset);
    q.exec();
    QVariantList out;
    while (q.next()) {
        const QJsonObject o = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object();
        QVariantList cells;
        for (const QString& c : cols) cells << o.value(c).toVariant();
        out << QVariant(cells);
    }
    return out;
}

bool DataStore::deleteRun(qint64 id)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    QSqlQuery q(db);
    db.transaction();
    q.prepare("DELETE FROM points WHERE run_id = ?");
    q.addBindValue(id);
    q.exec();
    q.prepare("DELETE FROM runs WHERE id = ?");
    q.addBindValue(id);
    const bool ok = q.exec();
    db.commit();
    refresh();
    return ok;
}

bool DataStore::renameRun(qint64 id, const QString& name)
{
    QSqlQuery q(QSqlDatabase::database(connectionName()));
    q.prepare("UPDATE runs SET name = ? WHERE id = ?");
    q.addBindValue(name);
    q.addBindValue(id);
    const bool ok = q.exec();
    refresh();
    return ok;
}

QString DataStore::exportCsv(qint64 id, const QString& path)
{
    const QVariantMap r = run(id);
    if (r.isEmpty()) return {};
    QString out = path;
    if (out.isEmpty()) {
        QDir().mkpath(exportDir());
        QString safe = r["name"].toString();
        safe.replace(QRegularExpression(R"([\\/:*?"<>|\s]+)"), "_");
        out = QDir(exportDir()).filePath(QStringLiteral("run%1_%2.csv").arg(id).arg(safe));
    }
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit message("error", QStringLiteral("无法写入 %1").arg(out));
        return {};
    }
    auto cell = [](const QVariant& v) {
        if (!v.isValid() || v.isNull()) return QString();
        if (v.typeId() == QMetaType::Double) return QString::number(v.toDouble(), 'g', 12);
        QString s = v.toString();
        if (s.contains(',') || s.contains('"') || s.contains('\n')) s = '"' + s.replace("\"", "\"\"") + '"';
        return s;
    };
    const QStringList cols = r["columns"].toStringList();
    f.write("\xEF\xBB\xBF");                          // BOM so Excel reads UTF-8
    f.write(cols.join(',').toUtf8() + "\n");
    for (int offset = 0;; offset += 5000) {
        const QVariantList chunk = rows(id, offset, 5000);
        for (const QVariant& row : chunk) {
            QStringList cells;
            for (const QVariant& v : row.toList()) cells << cell(v);
            f.write(cells.join(',').toUtf8() + "\n");
        }
        if (chunk.size() < 5000) break;
    }
    emit message("info", QStringLiteral("已导出 CSV：%1").arg(QDir::toNativeSeparators(out)));
    return out;
}

// ---------------------------------------------------------------------------

int DataStore::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_runs.size());
}

QVariant DataStore::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_runs.size()) return {};
    const RunRow& r = m_runs.at(index.row());
    switch (role) {
    case IdRole:      return r.id;
    case NameRole:    return r.name;
    case KindRole:    return r.kind;
    case StartedRole: return r.started;
    case StatusRole:  return r.status;
    case PointsRole:  return r.points;
    case ScriptRole:  return r.script;
    }
    return {};
}

QHash<int, QByteArray> DataStore::roleNames() const
{
    return {{IdRole, "runId"}, {NameRole, "name"}, {KindRole, "kind"}, {StartedRole, "started"},
            {StatusRole, "status"}, {PointsRole, "points"}, {ScriptRole, "script"}};
}
