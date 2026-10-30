#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>

class QQuickItem;
class QQuickWindow;

// Custom, theme-coloured title bar with native window behaviour on Windows.
//
// The window keeps its native styles (WS_CAPTION / WS_THICKFRAME), so dragging,
// double-click maximise, edge resizing, Aero Snap, the drop shadow, minimise
// animations and the Alt+Space menu all still work; only the non-client area is
// removed (WM_NCCALCSIZE) and the hit test (WM_NCHITTEST) decides what is
// caption, border or client. Everything in the top `titleBarHeight` pixels is
// caption except items that take mouse input (buttons, fields...).
//
// On other platforms, or with --native-frame / DM_NATIVE_FRAME=1, the system
// frame is kept and `active` stays false (QML then hides its window buttons).
class WindowChrome : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int titleBarHeight READ titleBarHeight WRITE setTitleBarHeight NOTIFY titleBarHeightChanged)
public:
    explicit WindowChrome(QObject* parent = nullptr);
    ~WindowChrome() override;

    // Call before the window is shown; shows it.
    void attach(QQuickWindow* window, bool enable);

    bool active() const { return m_active; }
    int titleBarHeight() const { return m_titleBarHeight; }
    void setTitleBarHeight(int h);

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void activeChanged();
    void titleBarHeightChanged();

private:
    bool isCaption(const QPointF& logicalPos) const;

    QPointer<QQuickWindow> m_window;
    bool m_active = false;
    int m_titleBarHeight = 52;
};
