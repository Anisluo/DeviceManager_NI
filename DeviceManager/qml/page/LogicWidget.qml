import QtQuick
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "../js/Format.js" as Fmt

// { type: "logic", source, hint, stale_source, stale_field }
// Digital waveform of a capture { lanes: [{ name, note, lo, hi, t: [s], v: [V] }], t_end, rev }:
// one strip per pin, time zoom only, two cursors. hint: text before the first capture.
// stale_source, stale_field: when that value no longer equals rev, says the waveform is out of date.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var rev: undefined
    property var liveRev: undefined
    property bool shown: false
    readonly property bool stale: rev !== undefined && liveRev !== undefined && rev !== liveRev
    readonly property double delta: view.cursor2 - view.cursor1
    spacing: 6

    function show(cap) {
        if (!cap || !cap.lanes || (cap.rev !== undefined && cap.rev === rev)) return
        rev = cap.rev
        view.setData(cap.lanes, cap.t_end)
        shown = true
    }

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id === w.spec.source) w.show(data)
            if (id === w.spec.stale_source) w.liveRev = Fmt.field(data, w.spec.stale_field)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 16
        Text {
            visible: !w.shown || w.stale
            text: w.stale ? "配置已改动，这是上一次的波形，请重新仿真" : (w.spec.hint || "等待数据…")
            color: w.stale ? Theme.toneColor("warning") : Theme.textFaint
            font.family: Theme.sans
            font.pixelSize: Theme.fontXs
        }
        Item { Layout.fillWidth: true }
        Text {
            visible: w.shown
            text: "C1 " + Fmt.si(view.cursor1, "s", 6) + "    C2 " + Fmt.si(view.cursor2, "s", 6)
            color: Theme.textMuted
            font.family: Theme.mono
            font.pixelSize: Theme.fontSm
        }
        Text {
            visible: w.shown
            text: "Δt " + Fmt.si(w.delta, "s", 6) + (w.delta !== 0 ? "    1/Δt " + Fmt.si(1 / Math.abs(w.delta), "Hz", 5) : "")
            color: Theme.text
            font.family: Theme.mono
            font.pixelSize: Theme.fontSm
            font.weight: Font.DemiBold
        }
        DmButton {
            visible: w.shown
            compact: true
            text: "全部时间"
            onClicked: view.fit()
        }
    }

    LogicView {
        id: view
        visible: w.shown
        Layout.fillWidth: true
        Layout.preferredHeight: 50 + 72 * laneCount      // ruler, axis, one strip per pin (LogicView.h)
        textColor: Theme.text
        mutedColor: Theme.textMuted
        faintColor: Theme.textFaint
        gridColor: Theme.grid
        axisColor: Theme.axis
        accentColor: Theme.accent
        seriesColors: Theme.series
        fontFamily: Theme.sans
        fontSize: Theme.fontXs
    }
}
