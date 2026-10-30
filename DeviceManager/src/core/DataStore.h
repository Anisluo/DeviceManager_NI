#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QVariant>

// Measurement database (SQLite, WAL). One "run" per script / scan / shmoo; each
// dm.record() call is one point. Point values are stored as a JSON object so
// runs can have any columns; runs.columns keeps their order.
//
//   runs(id, name, kind, station, script, started, finished, status, columns, meta, points)
//   points(run_id, seq, t, data)
//
// Writes may come from any thread (each thread gets its own connection); the
// list model and the Q_INVOKABLE readers are for the UI thread.
class DataStore : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY opened)
    Q_PROPERTY(QString exportDir READ exportDir NOTIFY opened)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, NameRole, KindRole, StartedRole, StatusRole, PointsRole, ScriptRole };

    explicit DataStore(QObject* parent = nullptr);
    ~DataStore() override;

    bool open(const QString& path, QString* error);
    // Call from a worker thread before it ends (connections are per thread).
    void closeThreadConnection();
    QString path() const { return m_path; }
    QString exportDir() const;
    int count() const { return int(m_runs.size()); }
    void setStationName(const QString& name) { m_station = name; }

    // ---- writers (thread-safe) ----
    qint64 createRun(const QString& name, const QString& kind, const QString& script, const QVariantMap& meta);
    void appendPoint(qint64 runId, const QStringList& order, const QVariantMap& values);
    void finishRun(qint64 runId, const QString& status);

    // ---- readers (UI thread) ----
    Q_INVOKABLE QVariantMap run(qint64 id) const;
    Q_INVOKABLE QStringList columns(qint64 id) const;
    Q_INVOKABLE QVariantList column(qint64 id, const QString& name) const;
    Q_INVOKABLE QVariantList rows(qint64 id, int offset = 0, int limit = 500) const;   // each row: cells aligned with columns
    Q_INVOKABLE bool deleteRun(qint64 id);
    Q_INVOKABLE bool renameRun(qint64 id, const QString& name);
    Q_INVOKABLE QString exportCsv(qint64 id, const QString& path = {});               // returns written path or ""
    Q_INVOKABLE void refresh();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void opened();
    void countChanged();
    void runChanged(qint64 id);           // points added or status changed
    void message(const QString& level, const QString& text);

private:
    QString connectionName() const;       // per-thread connection, created on demand
    void notify(qint64 id);

    struct RunRow { qint64 id; QString name, kind, started, status, script; int points; };
    QString m_path;
    QString m_station;
    QList<RunRow> m_runs;
    mutable QMutex m_mutex;
    QHash<qint64, QStringList> m_columns;  // cache of runs.columns for runs being written
    QHash<qint64, int> m_nextSeq;
    bool m_notifyPending = false;
    QSet<qint64> m_dirtyRuns;
};
