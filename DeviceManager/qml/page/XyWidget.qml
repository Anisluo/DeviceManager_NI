import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "../js/Format.js" as Fmt

// { type: "xy", x: "vin", y: "vout" | ["vout", "iin"], group: "rload", height, x_unit, y_unit }
// Plots the rows of the scan / script that is running (or ran last), live.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    readonly property var engine: App.script
    readonly property var yCols: Array.isArray(spec.y) ? spec.y : (spec.y ? [spec.y] : [])

    spacing: 6

    function refresh() {
        const cols = engine.columns
        if (!spec.x || cols.indexOf(spec.x) < 0) { plot.xs = []; plot.series = []; return }
        const xs = engine.column(spec.x)
        const series = []
        if (spec.group && cols.indexOf(spec.group) >= 0) {
            const g = engine.column(spec.group)
            const keys = []
            for (const v of g) if (keys.indexOf(v) < 0) keys.push(v)
            for (const y of yCols) {
                if (cols.indexOf(y) < 0) continue
                const ys = engine.column(y)
                for (const key of keys) {
                    const sx = [], sy = []
                    for (let k = 0; k < xs.length; ++k) if (g[k] === key) { sx.push(xs[k]); sy.push(ys[k]) }
                    const label = spec.group + "=" + (typeof key === "number" ? Fmt.si(key, "", 3) : key)
                    series.push({ label: yCols.length > 1 ? y + " " + label : label, xs: sx, values: sy, unit: spec.y_unit || "" })
                }
            }
        } else {
            for (const y of yCols)
                if (cols.indexOf(y) >= 0) series.push({ label: y, values: engine.column(y), unit: spec.y_unit || "" })
        }
        plot.xLabel = spec.x
        plot.xUnit = spec.x_unit || ""
        plot.xs = xs
        plot.series = series
    }

    Timer { id: throttle; interval: 120; onTriggered: w.refresh() }
    Connections {
        target: w.engine
        function onRecordsChanged() { throttle.restart() }
    }
    Component.onCompleted: refresh()

    Text {
        visible: w.engine.recordCount === 0
        text: "运行扫描后在这里显示 " + (w.yCols.join(", ") || "y") + " – " + (w.spec.x || "x") + " 曲线"
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
    XyPlot {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 280
    }
}
