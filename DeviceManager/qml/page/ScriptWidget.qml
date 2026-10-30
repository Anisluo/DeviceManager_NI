import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// { type: "script", file: "ldo_line_regulation.py" (relative to sequences/), height, editable }
// Embeds a sequence in a page: edit, save, run/stop. Output goes to the bottom console.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    readonly property var engine: App.script
    readonly property string path: engine.resolvePath(spec.file || "")
    property string status: ""
    property bool mine: false        // did this widget start the running job?

    spacing: 8

    function apply() { run() }
    function run() {
        if (engine.running) return
        if (editor.modified && path !== "") save()
        mine = engine.run(editor.text, path || "<page script>")
    }
    function save() { if (path !== "" && engine.writeText(path, editor.text)) editor.modified = false }

    Component.onCompleted: editor.setText(spec.code !== undefined ? spec.code : engine.readText(path))

    Connections {
        target: w.engine
        function onFinished(ok, message, seconds) {
            if (!w.mine) return
            w.mine = false
            w.status = (ok ? "完成" : message) + " · " + seconds.toFixed(2) + " s · " + w.engine.recordCount + " 行数据"
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Text {
            text: w.spec.file || "脚本"
            color: Theme.textMuted
            font.family: Theme.mono
            font.pixelSize: Theme.fontSm
        }
        Item { Layout.fillWidth: true }
        Text {
            text: w.engine.running && w.mine ? (w.engine.progressText || "运行中…") : w.status
            color: Theme.textMuted
            font.family: Theme.sans
            font.pixelSize: Theme.fontSm
            elide: Text.ElideRight
            Layout.maximumWidth: 320
        }
        DmButton {
            visible: w.spec.editable !== false && w.path !== ""
            compact: true; kind: "ghost"; text: "保存"
            enabled: editor.modified
            onClicked: w.save()
        }
        DmButton {
            compact: true
            kind: w.engine.running && w.mine ? "secondary" : "primary"
            text: w.engine.running && w.mine ? "■ 停止" : "▶ 运行"
            enabled: w.engine.available && (!w.engine.running || w.mine)
            onClicked: w.engine.running ? w.engine.stop() : w.run()
        }
    }
    Rectangle {
        Layout.fillWidth: true
        height: 3
        radius: 1.5
        color: Theme.border
        visible: w.mine && w.engine.running
        Rectangle { width: parent.width * w.engine.progress; height: 3; radius: 1.5; color: Theme.accent }
    }
    CodeEditor {
        id: editor
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 240
        readOnly: w.spec.editable === false
        onRunRequested: w.run()
        onSaveRequested: w.save()
    }
}
