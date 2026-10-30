#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSyntaxHighlighter>

// Python highlighting for a QML TextEdit/TextArea:
//   SyntaxHighlighter { document: editor.textDocument; keyword: Theme.accent; ... }
class SyntaxHighlighter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument* document READ document WRITE setDocument NOTIFY changed)
    Q_PROPERTY(QColor keyword MEMBER m_keyword NOTIFY changed)
    Q_PROPERTY(QColor string MEMBER m_string NOTIFY changed)
    Q_PROPERTY(QColor comment MEMBER m_comment NOTIFY changed)
    Q_PROPERTY(QColor number MEMBER m_number NOTIFY changed)
    Q_PROPERTY(QColor builtin MEMBER m_builtin NOTIFY changed)
public:
    explicit SyntaxHighlighter(QObject* parent = nullptr);

    QQuickTextDocument* document() const { return m_doc; }
    void setDocument(QQuickTextDocument* doc);

signals:
    void changed();

private:
    class Impl;
    QPointer<QQuickTextDocument> m_doc;
    QPointer<QSyntaxHighlighter> m_impl;   // owned by the QTextDocument
    QColor m_keyword, m_string, m_comment, m_number, m_builtin;
};
