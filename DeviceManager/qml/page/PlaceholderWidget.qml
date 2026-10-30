import QtQuick
import DeviceManager

// Shown for widget types this host version does not render yet.
Rectangle {
    property var spec: ({})
    readonly property var planned: ({})

    implicitHeight: 64
    radius: Theme.radiusSm
    color: "transparent"
    border.color: Theme.borderStrong
    border.width: 1

    Text {
        anchors.centerIn: parent
        text: planned[spec.type]
              ? "控件「" + spec.type + "」将在 " + planned[spec.type] + " 支持"
              : "未知控件类型：" + (spec.type || "(缺少 type)")
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
