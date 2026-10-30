import QtQuick
import QtQuick.Controls.Basic
import DeviceManager

Switch {
    id: control
    hoverEnabled: true
    font.family: Theme.sans
    font.pixelSize: Theme.fontMd

    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 22
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: height / 2
        color: control.checked ? Theme.accent : Theme.selected
        Behavior on color { ColorAnimation { duration: 120 } }

        Rectangle {
            width: 16; height: 16; radius: 8
            y: 3
            x: control.checked ? parent.width - width - 3 : 3
            color: "#FFFFFF"
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + 10
        text: control.text
        font: control.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
    }
}
