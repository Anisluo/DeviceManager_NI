import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// { type: "shmoo", x: "vin", y: "rload", value: "pass", height, x_unit, y_unit, value_unit }
// Live pass/fail (or numeric) map of the running / last scan.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    readonly property var engine: App.script
    readonly property string valueCol: spec.value || "pass"
    property int passed: 0
    property int total: 0

    spacing: 6

    function refresh() {
        const cols = engine.columns
        if (cols.indexOf(spec.x) < 0 || cols.indexOf(spec.y) < 0 || cols.indexOf(valueCol) < 0) {
            map.xs = []; map.ys = []; map.values = []; passed = 0; total = 0
            return
        }
        const vals = engine.column(valueCol)
        map.xs = engine.column(spec.x)
        map.ys = engine.column(spec.y)
        map.values = vals
        total = vals.length
        passed = vals.filter(v => v === true).length
    }

    Timer { id: throttle; interval: 120; onTriggered: w.refresh() }
    Connections {
        target: w.engine
        function onRecordsChanged() { throttle.restart() }
    }
    Component.onCompleted: refresh()

    RowLayout {
        spacing: 14
        Repeater {
            model: [{ c: Theme.dark ? "#5E8F60" : "#7FAE7F", t: "通过" }, { c: Theme.dark ? "#B4574A" : "#D7826F", t: "失败" }]
            delegate: RowLayout {
                required property var modelData
                spacing: 6
                Rectangle { width: 12; height: 12; radius: 3; color: modelData.c }
                Text { text: modelData.t; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
            }
        }
        Item { Layout.fillWidth: true }
        Text {
            visible: w.total > 0
            text: "通过 " + w.passed + " / " + w.total + "  (" + (100 * w.passed / Math.max(1, w.total)).toFixed(1) + " %)"
            color: Theme.text
            font.family: Theme.sans
            font.pixelSize: Theme.fontSm
        }
    }
    ShmooMap {
        id: map
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 300
        xLabel: w.spec.x || ""
        yLabel: w.spec.y || ""
        xUnit: w.spec.x_unit || ""
        yUnit: w.spec.y_unit || ""
        valueUnit: w.spec.value_unit || ""
    }
}
