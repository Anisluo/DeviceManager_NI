import QtQuick
import QtQuick.Layouts
import DeviceManager

Rectangle {
    id: item
    property string text: ""
    property string detail: ""
    property string status: ""          // shows a StatusDot when set
    property bool selected: false
    property bool dimmed: false          // e.g. an instrument without a driver
    signal clicked()

    implicitHeight: 34
    radius: Theme.radiusSm
    color: selected ? Theme.selected : mouse.containsMouse ? Theme.hover : "transparent"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8
        StatusDot { visible: item.status !== ""; status: item.status }
        Text {
            text: item.text
            color: item.dimmed ? Theme.textFaint : Theme.text
            font.family: Theme.sans
            font.pixelSize: Theme.fontMd
            font.weight: item.selected ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Text {
            visible: item.detail !== ""
            text: item.detail
            color: Theme.textFaint
            font.family: Theme.sans
            font.pixelSize: Theme.fontXs
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: item.clicked()
    }
}
