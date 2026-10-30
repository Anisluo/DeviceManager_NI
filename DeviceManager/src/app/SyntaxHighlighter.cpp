#include "SyntaxHighlighter.h"

#include <QTextDocument>

class SyntaxHighlighter::Impl : public QSyntaxHighlighter {
public:
    Impl(QTextDocument* doc, SyntaxHighlighter* owner) : QSyntaxHighlighter(doc), o(owner) {}

protected:
    void highlightBlock(const QString& text) override
    {
        static const QRegularExpression keywords(
            R"(\b(and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield|None|True|False)\b)");
        static const QRegularExpression builtins(
            R"(\b(print|range|len|abs|min|max|sum|round|int|float|str|list|dict|tuple|set|enumerate|zip|sorted|isinstance|dm|device|devices|record|sleep|log|progress|linspace|sweep|math|time|self)\b)");
        static const QRegularExpression numbers(R"(\b[0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?\b)");
        static const QRegularExpression decorator(R"(^\s*@\w+)");
        static const QRegularExpression strings(R"('''.*?'''|""".*?"""|'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")");

        auto paint = [&](const QRegularExpression& re, const QColor& c, bool bold = false) {
            QTextCharFormat f;
            f.setForeground(c);
            if (bold) f.setFontWeight(QFont::DemiBold);
            for (auto it = re.globalMatch(text); it.hasNext();) {
                const auto m = it.next();
                setFormat(int(m.capturedStart()), int(m.capturedLength()), f);
            }
        };
        paint(numbers, o->m_number);
        paint(builtins, o->m_builtin);
        paint(keywords, o->m_keyword, true);
        paint(decorator, o->m_builtin);
        paint(strings, o->m_string);

        // Comments: '#' outside of strings.
        bool inStr = false;
        QChar quote;
        for (int i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            if (inStr) {
                if (c == '\\') ++i;
                else if (c == quote) inStr = false;
            } else if (c == '\'' || c == '"') {
                inStr = true;
                quote = c;
            } else if (c == '#') {
                QTextCharFormat f;
                f.setForeground(o->m_comment);
                f.setFontItalic(true);
                setFormat(i, int(text.size()) - i, f);
                break;
            }
        }
    }

private:
    SyntaxHighlighter* o;
};

SyntaxHighlighter::SyntaxHighlighter(QObject* parent) : QObject(parent)
{
    connect(this, &SyntaxHighlighter::changed, this, [this] { if (m_impl) m_impl->rehighlight(); });
}

void SyntaxHighlighter::setDocument(QQuickTextDocument* doc)
{
    if (doc == m_doc) return;
    delete m_impl.data();
    m_doc = doc;
    if (doc && doc->textDocument()) m_impl = new Impl(doc->textDocument(), this);
    emit changed();
}
