#include "WindowChrome.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <windowsx.h>
#  include <dwmapi.h>
#endif

WindowChrome::WindowChrome(QObject* parent) : QObject(parent) {}

WindowChrome::~WindowChrome()
{
    if (m_active) qApp->removeNativeEventFilter(this);
}

void WindowChrome::setTitleBarHeight(int h)
{
    if (h == m_titleBarHeight) return;
    m_titleBarHeight = h;
    emit titleBarHeightChanged();
}

void WindowChrome::attach(QQuickWindow* window, bool enable)
{
    m_window = window;
    if (!window) return;
#ifdef Q_OS_WIN
    if (enable && QGuiApplication::platformName() == QLatin1String("windows")) {
        window->setFlags(window->flags() | Qt::FramelessWindowHint);
        window->show();
        HWND hwnd = reinterpret_cast<HWND>(window->winId());
        // Put the native frame styles back: they drive snap, resize, animations and the shadow.
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        const MARGINS shadow = {0, 0, 0, 1};             // a 1-px DWM sheet keeps the drop shadow
        DwmExtendFrameIntoClientArea(hwnd, &shadow);
        // The filter must be in place before SWP_FRAMECHANGED: that sends WM_NCCALCSIZE synchronously.
        m_active = true;
        qApp->installNativeEventFilter(this);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        emit activeChanged();
        return;
    }
#else
    Q_UNUSED(enable);
#endif
    window->show();
}

// Caption = inside the title bar and not over something that takes mouse input.
bool WindowChrome::isCaption(const QPointF& pos) const
{
    if (!m_window || pos.y() < 0 || pos.y() >= m_titleBarHeight) return false;
    QQuickItem* item = m_window->contentItem();
    QPointF p = pos;
    while (item) {
        QQuickItem* child = nullptr;
        const auto kids = item->childItems();
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {     // topmost first
            QQuickItem* c = *it;
            if (!c->isVisible() || !c->isEnabled()) continue;
            const QPointF local = c->mapFromItem(item, p);
            if (c->contains(local)) { child = c; p = local; break; }
        }
        if (!child) break;
        // A button, field, mouse area… (plain Text also accepts buttons, for links: treat it as passive)
        if (child->acceptedMouseButtons() != Qt::NoButton && !child->inherits("QQuickText")) return false;
        item = child;
    }
    return true;
}

bool WindowChrome::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (!m_active || !m_window || eventType != "windows_generic_MSG") return false;
    MSG* msg = static_cast<MSG*>(message);
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (msg->hwnd != hwnd) return false;

    switch (msg->message) {
    case WM_NCCALCSIZE:
        if (msg->wParam == TRUE) {
            // The whole window is client area. When maximised Windows places the window
            // with its (now invisible) resize border off-screen: inset by that border.
            if (IsZoomed(hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                const UINT dpi = GetDpiForWindow(hwnd);
                const int fx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                params->rgrc[0].left += fx;
                params->rgrc[0].right -= fx;
                params->rgrc[0].top += fy;
                params->rgrc[0].bottom -= fy;
            }
            *result = 0;
            return true;
        }
        return false;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const UINT dpi = GetDpiForWindow(hwnd);
        const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (!IsZoomed(hwnd)) {
            const bool l = pt.x < wr.left + border, r = pt.x >= wr.right - border;
            const bool t = pt.y < wr.top + border, b = pt.y >= wr.bottom - border;
            if (t && l) { *result = HTTOPLEFT; return true; }
            if (t && r) { *result = HTTOPRIGHT; return true; }
            if (b && l) { *result = HTBOTTOMLEFT; return true; }
            if (b && r) { *result = HTBOTTOMRIGHT; return true; }
            if (l) { *result = HTLEFT; return true; }
            if (r) { *result = HTRIGHT; return true; }
            if (b) { *result = HTBOTTOM; return true; }
            if (t) { *result = HTTOP; return true; }
        }
        POINT cp = pt;
        ScreenToClient(hwnd, &cp);
        const qreal dpr = m_window->devicePixelRatio();
        *result = isCaption(QPointF(cp.x / dpr, cp.y / dpr)) ? HTCAPTION : HTCLIENT;
        return true;
    }
    default:
        return false;
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}
