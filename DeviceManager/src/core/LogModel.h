#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

// Application-wide log shown in the bottom console. Also captures qDebug/qWarning.
class LogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int errorCount READ errorCount NOTIFY countChanged)
public:
    enum Roles { TimeRole = Qt::UserRole + 1, LevelRole, SourceRole, MessageRole };

    explicit LogModel(QObject* parent = nullptr);
    static void installMessageHandler(LogModel* model);

    int count() const { return int(m_entries.size()); }
    int errorCount() const { return m_errors; }

    Q_INVOKABLE void append(const QString& level, const QString& source, const QString& message);
    Q_INVOKABLE void clear();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();
    void appended(const QString& level, const QString& source, const QString& message);

private:
    struct Entry { QDateTime time; QString level, source, message; };
    QVector<Entry> m_entries;
    int m_errors = 0;
    static constexpr int kMaxEntries = 5000;
};
