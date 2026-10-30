import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "scope", source, height, channels: ["iq", ...] (optional filter), unit_index: 0 }
// Plots a capture { t: [ms...], channels: [{ name, unit, data }] }. unit_index picks the channels
// of the n-th distinct unit, so one capture can be split across plots with sensible Y axes.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property string note: "等待采集…"
    property bool empty: false
    // a secondary plot (unit_index > 0) with nothing to show folds away instead of an empty frame
    visible: !(empty && (spec.unit_index || 0) > 0)
    spacing: 4

    function show(cap) {
        if (!cap || !cap.channels || !cap.t || cap.t.length === 0) { plot.series = []; plot.xs = []; empty = true; return }
        let chs = cap.channels
        if (spec.channels) chs = chs.filter(c => spec.channels.indexOf(c.name) >= 0)
        if (spec.unit_index !== undefined) {
            const units = []
            for (const c of chs) if (units.indexOf(c.unit) < 0) units.push(c.unit)
            const u = units[spec.unit_index]
            chs = u === undefined ? [] : chs.filter(c => c.unit === u)
        }
        plot.xs = cap.t.map(v => v / 1000)            // ms -> s, so the axis gets µs / ms prefixes
        plot.series = chs.map(c => ({ label: c.name + (c.unit ? " [" + c.unit + "]" : ""), values: c.data, unit: c.unit }))
        empty = chs.length === 0
        note = chs.length ? cap.t.length + " 点 · Δt " + Fmt.si((cap.dt_ms || 0) / 1000, "s", 3) : "（此图没有通道）"
    }

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) { if (id === w.spec.source) w.show(data) }
    }

    Text {
        text: w.note
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontXs
    }
    XyPlot {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 260
        xLabel: "t"
        xUnit: "s"
        markers: false
    }
}
