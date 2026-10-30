#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <atomic>

class DataStore;
class QThread;
class ScriptWorker;

// Runs sequence scripts and console input on a dedicated Python thread so the
// UI never blocks. One job at a time; Stop raises KeyboardInterrupt in the script.
// Rows passed to dm.record() are collected here as the run's data table and
// written to the DataStore (one run per script, or per dm.new_run()).
class ScriptEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString pythonVersion READ pythonVersion CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY runningChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY runningChanged)
    Q_PROPERTY(bool background READ background NOTIFY runningChanged)   // started by a page widget that shows its own progress
    Q_PROPERTY(QStringList columns READ columns NOTIFY recordsChanged)
    Q_PROPERTY(int recordCount READ recordCount NOTIFY recordsChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString sequencesDir READ sequencesDir CONSTANT)
    Q_PROPERTY(qint64 currentRunId READ currentRunId NOTIFY recordsChanged)
public:
    ScriptEngine(bool available, QString pythonVersion, QString sequencesDir, DataStore* store,
                 QObject* parent = nullptr);
    ~ScriptEngine() override;

    bool available() const { return m_available; }
    QString pythonVersion() const { return m_pythonVersion; }
    bool running() const { return m_running; }
    QString currentName() const { return m_currentName; }
    QString currentPath() const { return m_currentPath; }
    bool background() const { return m_background; }
    QStringList columns() const { return m_columns; }
    int recordCount() const { return int(m_records.size()); }
    double progress() const { return m_progress; }
    QString progressText() const { return m_progressText; }
    QString sequencesDir() const { return m_sequencesDir; }
    qint64 currentRunId() const { return m_uiRunId; }

    // name is used in tracebacks; for files pass the path.
    Q_INVOKABLE bool run(const QString& code, const QString& name, bool background = false);
    Q_INVOKABLE bool runFile(const QString& path);
    // Console input, executed in a namespace that persists across calls.
    Q_INVOKABLE bool exec(const QString& source);
    Q_INVOKABLE void stop();
    // Background Python job without echo or data run (report / Parquet export).
    Q_INVOKABLE bool runTask(const QString& code, const QString& label);

    Q_INVOKABLE QVariantList record(int row) const;   // cells aligned with columns
    Q_INVOKABLE QVariantList column(const QString& name) const;
    Q_INVOKABLE void clearRecords();

    Q_INVOKABLE QStringList sequences() const;
    Q_INVOKABLE QString resolvePath(const QString& path) const;
    Q_INVOKABLE QString readText(const QString& path) const;
    Q_INVOKABLE bool writeText(const QString& path, const QString& text);

    // ---- called from the script thread (thread-safe) ----
    bool isScriptThread() const;
    bool stopRequested() const { return m_stop.load(); }
    void postOutput(const QString& text, const QString& stream);
    void postRecord(const QByteArray& json);
    void postProgress(double fraction, const QString& text);
    void postFileExported(const QString& path);
    // Ends the current data run (if any) and starts a new one; returns its id. Script thread.
    qint64 newRun(const QString& name, const QString& kind, const QVariantMap& meta);
    qint64 scriptRunId() const { return m_runId; }

signals:
    void runningChanged();
    void recordsChanged();
    void progressChanged();
    void output(const QString& text, const QString& stream);   // stream: stdout | stderr | info | input
    void recordAdded(int row);
    void finished(bool ok, const QString& message, double seconds);
    void log(const QString& level, const QString& message);
    void fileExported(const QString& path);

private:
    enum class Kind { Script, Console, Task };
    bool start(Kind kind, const QString& code, const QString& name);
    void onJobDone(Kind kind, bool ok, const QString& message);
    void finishRun(const QString& status);    // script thread

    bool m_available;
    QString m_pythonVersion;
    QString m_sequencesDir;
    bool m_running = false;
    QString m_currentName;
    QString m_currentPath;   // file of the running script ("" for editor buffers / console)
    bool m_background = false;
    QStringList m_columns;
    QVector<QVariantMap> m_records;
    double m_progress = 0;
    QString m_progressText;
    QElapsedTimer m_clock;

    QThread* m_thread = nullptr;
    ScriptWorker* m_worker = nullptr;
    std::atomic<bool> m_stop{false};
    quint64 m_job = 0;          // increments per job; a late Stop must not hit the next one

    DataStore* m_store = nullptr;
    // Script-thread state of the data run being written.
    qint64 m_runId = 0;
    QString m_jobName, m_jobPath;
    Kind m_jobKind = Kind::Script;
    qint64 m_uiRunId = 0;       // UI-thread copy
};
