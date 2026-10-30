import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager

// { type: "log", height } — events emitted by the device plugin and failed calls.
Rectangle {
    id: w
    property var spec: ({})
    property var ctx: null

    implicitHeight: spec.height || 140
    radius: Theme.radiusSm
    color: Theme.bg
    border.color: Theme.border

    ListModel { id: entries }

    Connections {
        target: w.ctx
        function onDeviceEvent(evt) {
            const text = evt.type === "log" ? evt.message
                       : JSON.stringify(evt).replace(/^\{|\}$/g, "").replace(/"/g, "")
            entries.append({
                time: Qt.formatTime(new Date(), "hh:mm:ss.zzz"),
                level: evt.level || (evt.type === "log" ? "info" : evt.type || "event"),
                text: text
            })
            if (entries.count > 300) entries.remove(0, entries.count - 300)
            list.positionViewAtEnd()
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: 10
        clip: true
        model: entries
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        delegate: RowLayout {
            required property string time
            required property string level
            required property string text
            width: list.width - 10
            spacing: 10
            Text { text: time; color: Theme.textFaint; font.family: Theme.mono; font.pixelSize: Theme.fontXs }
            Text {
                text: level
                Layout.preferredWidth: 56
                color: level === "error" ? Theme.error : level === "warning" ? Theme.warning
                     : level === "state" ? Theme.accent : Theme.textMuted
                font.family: Theme.mono; font.pixelSize: Theme.fontXs
            }
            Text {
                text: parent.text
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: Theme.text
                font.family: Theme.mono; font.pixelSize: Theme.fontXs
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: entries.count === 0
        text: "暂无事件"
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
