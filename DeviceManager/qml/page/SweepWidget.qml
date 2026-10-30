import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "../js/Format.js" as Fmt

// Declarative scan, no Python needed (runs dm.scan.run_spec on the script thread):
// { type: "sweep", name, kind: "scan" | "shmoo",
//   axes: [{ name, label, unit, start, stop, step | points | values, device, method, args, settle_ms }],   outer → inner
//   measure: [{ name, device, method, args, field }], pass: "vout > 3.2", setup: [...], teardown: [...] }
// "$self" (the default device) is the device whose page this is. Axis ranges are editable.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    readonly property var engine: App.script
    property var axes: []           // editable copies of spec.axes
    property bool mine: false
    property string status: ""

    spacing: 10

    Component.onCompleted: axes = JSON.parse(JSON.stringify(spec.axes || []))

    function points(a) {
        if (a.values) return a.values.length
        if (a.points) return a.points
        if (!a.step) return 0
        return Math.max(0, Math.round((a.stop - a.start) / a.step)) + 1
    }
    readonly property int total: axes.reduce((n, a) => n * points(a), axes.length ? 1 : 0)

    function apply() { run() }
    function run() {
        if (engine.running) return
        const s = JSON.parse(JSON.stringify(spec))
        s.axes = axes
        s.name = s.name || "sweep"
        const self = ctx && ctx.device ? ctx.device.name : ""
        // A JSON string literal is also a valid Python string literal.
        const code = "from dm.scan import run_spec\n"
                   + "r = run_spec(" + JSON.stringify(JSON.stringify(s)) + ", self_device=" + JSON.stringify(self) + ")\n"
                   + "print(r)\n"
        mine = engine.run(code, s.name, true)
    }

    Connections {
        target: w.engine
        function onFinished(ok, message, seconds) {
            if (!w.mine) return
            w.mine = false
            w.status = (ok ? "完成" : message) + " · " + seconds.toFixed(1) + " s"
        }
    }

    // Axis editors
    Repeater {
        model: w.axes.length
        // Two lines per axis so the editor fits narrow cards: name · unit · points, then the range fields.
        delegate: ColumnLayout {
            id: axisRow
            required property int index
            readonly property var axis: w.axes[index]
            Layout.fillWidth: true
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: (axisRow.axis.label || axisRow.axis.name) + (axisRow.axis.unit ? "  [" + axisRow.axis.unit + "]" : "")
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontMd
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Text {
                    text: w.points(axisRow.axis) + " 点"
                    color: Theme.textFaint
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                }
            }
            RowLayout {
                visible: !axisRow.axis.values
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: axisRow.axis.values ? [] : ["start", "stop", axisRow.axis.points ? "points" : "step"]
                    delegate: RowLayout {
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        spacing: 4
                        Text {
                            text: ({ start: "起", stop: "止", step: "步", points: "点" })[modelData]
                            color: Theme.textFaint
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSm
                        }
                        DmTextField {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 40
                            implicitWidth: 60
                            horizontalAlignment: Text.AlignRight
                            text: Fmt.eng(axisRow.axis[modelData])
                            enabled: !w.engine.running
                            onEditingFinished: {
                                const v = Fmt.parseNumber(text)
                                if (isNaN(v)) { text = Fmt.eng(axisRow.axis[modelData]); return }
                                const copy = w.axes.slice()
                                copy[axisRow.index] = Object.assign({}, copy[axisRow.index], { [modelData]: v })
                                w.axes = copy
                            }
                        }
                    }
                }
            }
            Text {
                visible: !!axisRow.axis.values
                text: (axisRow.axis.values || []).map(v => Fmt.eng(v, 4)).join(", ")
                color: Theme.text
                font.family: Theme.mono
                font.pixelSize: Theme.fontSm
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }

    Text {
        visible: !!w.spec.pass
        text: "判据：" + (w.spec.pass || "")
        color: Theme.textMuted
        font.family: Theme.mono
        font.pixelSize: Theme.fontSm
    }

    Rectangle {
        Layout.fillWidth: true
        height: 3
        radius: 1.5
        color: Theme.border
        visible: w.mine && w.engine.running
        Rectangle { width: parent.width * w.engine.progress; height: 3; radius: 1.5; color: Theme.accent }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: w.mine && w.engine.running ? w.engine.progressText : (w.status || ("共 " + w.total + " 点"))
            color: Theme.textMuted
            font.family: Theme.sans
            font.pixelSize: Theme.fontSm
            elide: Text.ElideRight
        }
        DmButton {
            text: w.mine && w.engine.running ? "■ 停止" : "▶ 开始扫描"
            kind: w.mine && w.engine.running ? "secondary" : "primary"
            enabled: w.engine.available && (!w.engine.running || w.mine) && w.total > 0
                     && w.ctx && w.ctx.device && w.ctx.device.online
            onClicked: w.engine.running ? w.engine.stop() : w.run()
        }
    }
}
