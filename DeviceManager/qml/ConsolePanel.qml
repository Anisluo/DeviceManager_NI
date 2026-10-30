import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// Collapsible bottom panel: application log + Python script console.
Rectangle {
    id: panel
    property bool expanded: false
    property real expandedHeight: 360
    property int tab: 0

    implicitHeight: expanded ? expandedHeight : 38
    color: Theme.sidebar
    Behavior on implicitHeight { enabled: !drag.active; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    // Starting a script (from the console or a page) brings the console into view.
    Connections {
        target: App.script
        function onRunningChanged() {
            if (App.script.running && !App.script.background && App.script.currentName !== "控制台") { panel.tab = 1; panel.expanded = true }
        }
    }

    // Resize handle
    Item {
        id: dragArea
        visible: panel.expanded
        anchors.top: parent.top
        width: parent.width
        height: 6
        z: 2
        property real startH: 0
        HoverHandler { cursorShape: Qt.SizeVerCursor }
        DragHandler {
            id: drag
            target: null
            xAxis.enabled: false
            onActiveChanged: if (active) dragArea.startH = panel.expandedHeight
            onTranslationChanged: panel.expandedHeight =
                Math.max(120, Math.min(panel.Window.height * 0.7, dragArea.startH - translation.y))
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 1
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 37
            Layout.leftMargin: 16
            Layout.rightMargin: 12
            spacing: 4

            Repeater {
                model: ["日志", "脚本控制台"]
                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    implicitWidth: label.implicitWidth + 20
                    implicitHeight: 26
                    radius: 6
                    color: panel.expanded && panel.tab === index ? Theme.selected : tabMouse.containsMouse ? Theme.hover : "transparent"
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: modelData + (index === 0 && App.log.errorCount > 0 ? "  · " + App.log.errorCount : "")
                              + (index === 1 && App.script.running ? "  ●" : "")
                        color: index === 0 && App.log.errorCount > 0 ? Theme.error
                             : index === 1 && App.script.running ? Theme.accent : Theme.text
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSm
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (panel.expanded && panel.tab === index) panel.expanded = false
                            else { panel.tab = index; panel.expanded = true }
                        }
                    }
                }
            }

            // Latest message preview while collapsed
            Text {
                id: lastText
                Layout.fillWidth: true
                Layout.leftMargin: 12
                visible: !panel.expanded
                text: lastLine
                property string lastLine: ""
                color: Theme.textMuted
                font.family: Theme.mono
                font.pixelSize: Theme.fontXs
                elide: Text.ElideRight
                maximumLineCount: 1
                Connections {
                    target: App.log
                    function onAppended(level, source, message) {
                        lastText.lastLine = "[" + source + "] " + message.replace(/\s*\n\s*/g, " ⏎ ")
                    }
                }
            }
            Item { Layout.fillWidth: true; visible: panel.expanded }

            DmButton {
                visible: panel.expanded && panel.tab === 0
                kind: "ghost"; compact: true
                text: "清空"
                onClicked: App.log.clear()
            }
            DmButton {
                kind: "ghost"; compact: true
                text: panel.expanded ? "▾" : "▴"
                onClicked: panel.expanded = !panel.expanded
            }
        }

        // Log
        ListView {
            id: logView
            visible: panel.expanded && panel.tab === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 16
            Layout.rightMargin: 8
            Layout.bottomMargin: 6
            clip: true
            model: App.log
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            property bool follow: true
            onMovementEnded: follow = atYEnd
            onCountChanged: if (follow) Qt.callLater(positionViewAtEnd)
            delegate: RowLayout {
                required property string time
                required property string level
                required property string source
                required property string message
                width: logView.width - 12
                spacing: 10
                Text { text: time; color: Theme.textFaint; font.family: Theme.mono; font.pixelSize: Theme.fontXs }
                Text {
                    text: level
                    Layout.preferredWidth: 52
                    color: level === "error" ? Theme.error : level === "warning" ? Theme.warning : Theme.textFaint
                    font.family: Theme.mono; font.pixelSize: Theme.fontXs
                }
                Text {
                    text: source
                    Layout.preferredWidth: 70
                    elide: Text.ElideRight
                    color: Theme.accent
                    font.family: Theme.mono; font.pixelSize: Theme.fontXs
                }
                Text {
                    text: message
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.text
                    font.family: Theme.mono; font.pixelSize: Theme.fontXs
                    textFormat: Text.PlainText
                }
            }
        }

        // Kept alive while hidden so output keeps accumulating.
        ScriptConsole {
            visible: panel.expanded && panel.tab === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
