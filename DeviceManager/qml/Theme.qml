pragma Singleton
import QtQuick
import DeviceManager.Core

// Warm, quiet palette: paper-like background, terracotta accent used sparingly,
// serif headings, thin rules, large radii, no heavy shadows.
QtObject {
    readonly property bool dark: App.darkTheme

    // Surfaces
    readonly property color bg:         dark ? "#262624" : "#F5F4EE"
    readonly property color sidebar:    dark ? "#1F1E1D" : "#EEECE3"
    readonly property color surface:    dark ? "#30302E" : "#FBFAF7"
    readonly property color surfaceAlt: dark ? "#383836" : "#F0EEE6"
    readonly property color hover:      dark ? "#3A3936" : "#E8E5DA"
    readonly property color selected:   dark ? "#45433F" : "#E2DED1"
    readonly property color border:     dark ? "#403F3B" : "#E1DDD1"
    readonly property color borderStrong: dark ? "#55534D" : "#CFCABB"

    // Text
    readonly property color text:       dark ? "#ECEAE3" : "#28261F"
    readonly property color textMuted:  dark ? "#A6A39A" : "#6E6A5E"
    readonly property color textFaint:  dark ? "#7C796F" : "#9A9587"

    // Accent & states
    readonly property color accent:      "#D97757"
    readonly property color accentHover: dark ? "#E48A6C" : "#C4643F"
    readonly property color accentSoft:  dark ? "#4A3329" : "#F5E2D8"
    readonly property color textOnAccent:    "#FFFFFF"
    readonly property color ok:          dark ? "#8DB48E" : "#4F7D53"
    readonly property color okSoft:      dark ? "#2F3D30" : "#E3ECDF"
    readonly property color warning:     dark ? "#E0B062" : "#A87418"
    readonly property color warningSoft: dark ? "#45391F" : "#F5EAD2"
    readonly property color error:       dark ? "#E5806E" : "#B4453A"
    readonly property color errorSoft:   dark ? "#4A2B26" : "#F6DFDA"

    // Chart series: accent first, then muted companions that stay readable in both modes.
    readonly property var series: dark
        ? ["#E08A6C", "#8FB0D6", "#A8C08A", "#D9B865", "#C4A0D0", "#7FC4BC"]
        : ["#D97757", "#4F74A3", "#6E8F4E", "#B8892B", "#8D6A9F", "#3E8F86"]
    readonly property color grid:  dark ? "#3A3936" : "#E9E6DC"
    readonly property color axis:  dark ? "#5C5A54" : "#C9C4B6"

    // Type
    readonly property string serif: Qt.platform.os === "windows" ? "Georgia" : "Noto Serif"
    readonly property string sans:  Qt.platform.os === "windows" ? "Segoe UI" : "Noto Sans"
    readonly property string mono:  Qt.platform.os === "windows" ? "Cascadia Mono" : "Noto Sans Mono"
    readonly property int fontXs: 11
    readonly property int fontSm: 12
    readonly property int fontMd: 13
    readonly property int fontLg: 15
    readonly property int fontXl: 20
    readonly property int fontXxl: 26

    // Shape & spacing
    readonly property int radius:   14
    readonly property int radiusSm: 8
    readonly property int gap:      16
    readonly property int pad:      20

    function toneColor(tone) {
        switch (tone) {
        case "ok": return ok
        case "warning": return warning
        case "error": return error
        case "accent": return accent
        default: return textMuted
        }
    }
    function toneSoft(tone) {
        switch (tone) {
        case "ok": return okSoft
        case "warning": return warningSoft
        case "error": return errorSoft
        case "accent": return accentSoft
        default: return surfaceAlt
        }
    }
}
