#include "LogModel.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cstdio>

static QPointer<LogModel> g_logModel;
static QtMessageHandler g_prevHandler = nullptr;

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (g_prevHandler) g_prevHandler(type, ctx, msg);
    if (!g_logModel) return;
    QString level;
    switch (type) {
    case QtDebugMsg:    level = "debug"; break;
    case QtInfoMsg:     level = "info"; break;
    case QtWarningMsg:  level = "warning"; break;
    default:            level = "error"; break;
    }
    const QString source = ctx.category && qstrcmp(ctx.category, "default") != 0
                               ? QString::fromUtf8(ctx.category) : QStringLiteral("host");
    QPointer<LogModel> model = g_logModel;
    QMetaObject::invokeMethod(model, [model, level, source, msg] {
        if (model) model->append(level, source, msg);
    }, Qt::QueuedConnection);
}

LogModel::LogModel(QObject* parent) : QAbstractListModel(parent) {}

void LogModel::installMessageHandler(LogModel* model)
{
    g_logModel = model;
    g_prevHandler = qInstallMessageHandler(messageHandler);
}

void LogModel::append(const QString& level, const QString& source, const QString& message)
{
    if (m_entries.size() >= kMaxEntries) {
        const int drop = kMaxEntries / 10;
        beginRemoveRows({}, 0, drop - 1);
        for (int i = 0; i < drop; ++i)
            if (m_entries[i].level == QLatin1String("error")) --m_errors;
        m_entries.remove(0, drop);
        endRemoveRows();
    }
    beginInsertRows({}, int(m_entries.size()), int(m_entries.size()));
    m_entries.push_back({QDateTime::currentDateTime(), level, source, message});
    if (level == QLatin1String("error")) ++m_errors;
    endInsertRows();
    emit countChanged();
    emit appended(level, source, message);
}

void LogModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_errors = 0;
    endResetModel();
    emit countChanged();
}

int LogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size()) return {};
    const Entry& e = m_entries.at(index.row());
    switch (role) {
    case TimeRole:    return e.time.toString("HH:mm:ss.zzz");
    case LevelRole:   return e.level;
    case SourceRole:  return e.source;
    case MessageRole: return e.message;
    }
    return {};
}

QHash<int, QByteArray> LogModel::roleNames() const
{
    return {{TimeRole, "time"}, {LevelRole, "level"}, {SourceRole, "source"}, {MessageRole, "message"}};
}
