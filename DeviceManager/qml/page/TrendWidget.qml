import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "../js/Format.js" as Fmt

// { type: "trend", height, window_s,
//   series: [{ source, field | expr, label, unit, axis: "left"|"right", color }] }
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property real windowS: spec.window_s || 30
    property bool paused: false

    readonly property var series: spec.series || []
    readonly property real keepS: 600                    // history kept for zooming out
    property var latest: []

    spacing: 10

    function colorOf(i) { return series[i].color || Theme.series[i % Theme.series.length] }
    function clear() {
        latest = series.map(() => undefined)
        plot.setSeriesSpecs(series.map((s, i) => ({
            label: s.label || s.field, unit: s.unit || "", color: s.color || "", axis: s.axis || "left" })))
        plot.xNow = Date.now() / 1000
    }
    Component.onCompleted: clear()
    onSeriesChanged: clear()

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            const t = Date.now() / 1000
            let touched = false
            for (let i = 0; i < w.series.length; ++i) {
                const s = w.series[i]
                if (s.source !== id) continue
                const v = s.expr ? Fmt.evalExpr(s.expr, data) : Fmt.field(data, s.field)
                if (typeof v !== "number" || !isFinite(v)) continue
                plot.append(i, t, v)
                w.latest[i] = v
                touched = true
            }
            if (touched) { w.latestChanged(); repaint.dirty = true }
        }
    }

    Timer {
        id: repaint
        property bool dirty: false
        interval: 33
        repeat: true
        running: w.visible
        // the plot follows "now" only when new data arrived; paused keeps the view still
        onTriggered: if (dirty && !w.paused) { dirty = false; plot.xNow = Date.now() / 1000 }
    }

    // Legend + controls
    RowLayout {
        Layout.fillWidth: true
        spacing: 18
        Repeater {
            model: w.series
            delegate: RowLayout {
                required property var modelData
                required property int index
                spacing: 6
                Rectangle { width: 14; height: 3; radius: 1.5; color: w.colorOf(index) }
                Text {
                    text: (modelData.label || modelData.field) + (modelData.axis === "right" ? " (右轴)" : "")
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                }
                Text {
                    text: Fmt.si(w.latest[index], modelData.unit || "", 5)
                    color: Theme.text
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    font.features: { "tnum": 1 }
                }
            }
        }
        Item { Layout.fillWidth: true }
        Repeater {
            model: [10, 30, 60, 300]
            delegate: DmButton {
                required property int modelData
                compact: true
                kind: w.windowS === modelData ? "secondary" : "ghost"
                text: modelData >= 60 ? (modelData / 60) + " min" : modelData + " s"
                onClicked: { w.windowS = modelData; plot.resetView() }
            }
        }
        DmButton {
            compact: true
            kind: w.paused ? "primary" : "ghost"
            text: w.paused ? "▶ 继续" : "❚❚ 暂停"
            onClicked: { w.paused = !w.paused; if (!w.paused) plot.xNow = Date.now() / 1000 }
        }
        DmButton { compact: true; kind: "ghost"; text: "清除"; onClicked: w.clear() }
    }

    PlotView {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 260
        xMode: "ago"
        followSpan: w.windowS
        keepSpan: w.keepS
        markers: false
        legend: false                                    // the row above is the legend
        textColor: Theme.text
        mutedColor: Theme.textMuted
        faintColor: Theme.textFaint
        gridColor: Theme.grid
        axisColor: Theme.axis
        surfaceColor: Theme.surface
        borderColor: Theme.border
        cursorColor: Theme.borderStrong
        accentColor: Theme.accent
        seriesColors: Theme.series
        fontFamily: Theme.sans
        fontSize: Theme.fontXs
    }
}
