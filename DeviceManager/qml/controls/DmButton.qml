import QtQuick
import QtQuick.Controls.Basic
import DeviceManager

// kind: "primary" (terracotta), "secondary" (outlined), "ghost" (text only)
Button {
    id: control
    property string kind: "secondary"
    property bool compact: false

    font.family: Theme.sans
    font.pixelSize: compact ? Theme.fontSm : Theme.fontMd
    padding: compact ? 6 : 9
    leftPadding: compact ? 10 : 16
    rightPadding: compact ? 10 : 16
    hoverEnabled: true

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.kind === "primary" ? Theme.textOnAccent
             : control.enabled ? Theme.text : Theme.textFaint
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: control.compact ? 28 : 36
        radius: Theme.radiusSm
        color: control.kind === "primary"
               ? (control.down ? Qt.darker(Theme.accentHover, 1.1) : control.hovered ? Theme.accentHover : Theme.accent)
               : control.down ? Theme.selected
               : control.hovered ? Theme.hover
               : control.kind === "ghost" ? "transparent" : Theme.surface
        border.width: control.kind === "secondary" ? 1 : 0
        border.color: Theme.borderStrong
        opacity: control.enabled ? 1 : 0.55
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
