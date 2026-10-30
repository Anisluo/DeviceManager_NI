import QtQuick
import QtQuick.Controls.Basic
import DeviceManager

TextField {
    id: control
    property bool invalid: false

    font.family: Theme.sans
    font.pixelSize: Theme.fontMd
    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectionColor: Theme.accentSoft
    selectedTextColor: Theme.text
    leftPadding: 10
    rightPadding: 10
    hoverEnabled: true

    background: Rectangle {
        implicitHeight: 34
        implicitWidth: 120
        radius: Theme.radiusSm
        color: control.enabled ? Theme.bg : Theme.surfaceAlt
        border.width: control.activeFocus ? 1.5 : 1
        border.color: control.invalid ? Theme.error
                    : control.activeFocus ? Theme.accent
                    : control.hovered ? Theme.borderStrong : Theme.border
    }
}
