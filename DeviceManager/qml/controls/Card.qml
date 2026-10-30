import QtQuick
import QtQuick.Layouts
import DeviceManager

// Rounded surface with an optional serif title and a thin rule beneath it.
Rectangle {
    id: card
    property string title: ""
    property string subtitle: ""
    property alias headerExtra: extraSlot.data
    default property alias content: body.data
    property int padding: Theme.pad

    color: Theme.surface
    radius: Theme.radius
    border.color: Theme.border
    border.width: 1
    implicitHeight: column.implicitHeight + padding * 2
    implicitWidth: column.implicitWidth + padding * 2

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: card.padding
        spacing: 14

        RowLayout {
            visible: card.title !== ""
            Layout.fillWidth: true
            spacing: 8
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Text {
                    text: card.title
                    font.family: Theme.serif
                    font.pixelSize: Theme.fontLg + 1
                    color: Theme.text
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Text {
                    visible: card.subtitle !== ""
                    text: card.subtitle
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    color: Theme.textMuted
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
            Row { id: extraSlot; spacing: 6 }
        }

        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: childrenRect.height
            implicitWidth: childrenRect.width
        }
    }
}
