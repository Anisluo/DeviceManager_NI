import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "trend", height, window_s,
//   series: [{ source, field | expr, label, unit, axis: "left"|"right", color }] }
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property real windowS: spec.window_s || 30
    property bool paused: false
    property real pausedAt: 0
    property int hoverX: -1

    readonly property var series: spec.series || []
    readonly property real keepS: 600                    // history kept for zooming out
    property var buffers: []                             // per series: [[t, v], ...]
    property var latest: []

    spacing: 10

    function colorOf(i) { return series[i].color || Theme.series[i % Theme.series.length] }
    function clear() {
        buffers = series.map(() => [])
        latest = series.map(() => undefined)
        canvas.requestPaint()
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
                const buf = w.buffers[i]
                buf.push([t, v])
                while (buf.length > 0 && buf[0][0] < t - w.keepS) buf.shift()
                w.latest[i] = v
                touched = true
            }
            if (touched) { w.latestChanged(); if (!w.paused) repaint.dirty = true }
        }
    }

    Timer {
        id: repaint
        property bool dirty: false
        interval: 33
        repeat: true
        running: w.visible
        onTriggered: if (dirty) { dirty = false; canvas.requestPaint() }
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
                onClicked: { w.windowS = modelData; canvas.requestPaint() }
            }
        }
        DmButton {
            compact: true
            kind: w.paused ? "primary" : "ghost"
            text: w.paused ? "▶ 继续" : "❚❚ 暂停"
            onClicked: { w.paused = !w.paused; w.pausedAt = Date.now() / 1000; canvas.requestPaint() }
        }
        DmButton { compact: true; kind: "ghost"; text: "清除"; onClicked: w.clear() }
    }

    Canvas {
        id: canvas
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 260
        renderStrategy: Canvas.Cooperative

        readonly property bool hasRight: w.series.some(s => s.axis === "right")
        readonly property int mt: 10
        readonly property int mb: 26

        onPaint: {
            const c = getContext("2d")
            c.reset()
            const W = width, H = height
            const ph = H - mt - mb
            if (ph <= 10) return

            const tNow = w.paused ? w.pausedAt : Date.now() / 1000
            const tMin = tNow - w.windowS

            // Axis ranges
            const axes = { left: { lo: Infinity, hi: -Infinity, unit: null }, right: { lo: Infinity, hi: -Infinity, unit: null } }
            for (let i = 0; i < w.series.length; ++i) {
                const a = axes[w.series[i].axis === "right" ? "right" : "left"]
                if (a.unit === null) a.unit = w.series[i].unit || ""
                for (const p of w.buffers[i] || []) {
                    if (p[0] < tMin) continue
                    if (p[1] < a.lo) a.lo = p[1]
                    if (p[1] > a.hi) a.hi = p[1]
                }
            }
            for (const k in axes) {
                const a = axes[k]
                if (a.lo === Infinity) { a.lo = 0; a.hi = 1 }
                const span = a.hi - a.lo
                const pad = span > 0 ? span * 0.08 : 0
                a.ticks = Fmt.niceTicks(a.lo - pad, a.hi + pad, Math.max(2, Math.floor(ph / 50)))
                a.yOf = v => mt + ph - (v - a.ticks.min) / (a.ticks.max - a.ticks.min) * ph
            }

            c.font = "11px '" + Theme.sans + "'"
            c.lineWidth = 1

            // Tick labels first, so the plot margins fit the widest label.
            const labels = a => {
                const out = []
                for (let v = a.ticks.min; v <= a.ticks.max + a.ticks.step * 0.5; v += a.ticks.step) {
                    const text = Fmt.tick(Math.abs(v) < a.ticks.step * 1e-6 ? 0 : v, a.ticks.step, a.unit)
                    out.push({ v: v, text: text, w: c.measureText(text).width })
                }
                return out
            }
            const L = axes.left, R = axes.right
            const leftLabels = labels(L)
            const rightLabels = hasRight ? labels(R) : []
            const ml = Math.ceil(Math.max(40, ...leftLabels.map(l => l.w)) + 14)
            const mr = hasRight ? Math.ceil(Math.max(40, ...rightLabels.map(l => l.w)) + 14) : 16
            const pw = W - ml - mr
            if (pw <= 10) return
            const xOf = t => ml + (t - tMin) / w.windowS * pw

            // Horizontal grid + axis labels
            c.textBaseline = "middle"
            for (const l of leftLabels) {
                const y = Math.round(L.yOf(l.v)) + 0.5
                c.strokeStyle = Theme.grid
                c.beginPath(); c.moveTo(ml, y); c.lineTo(ml + pw, y); c.stroke()
                c.fillStyle = Theme.textMuted
                c.textAlign = "right"
                c.fillText(l.text, ml - 8, y)
            }
            c.textAlign = "left"
            for (const l of rightLabels) c.fillText(l.text, ml + pw + 8, R.yOf(l.v))

            // Time grid
            const tt = Fmt.niceTicks(0, w.windowS, Math.max(2, Math.floor(pw / 110)))
            c.textAlign = "center"
            c.textBaseline = "top"
            for (let s = 0; s <= w.windowS + 1e-9; s += tt.step) {
                const x = Math.round(xOf(tNow - s)) + 0.5
                c.strokeStyle = Theme.grid
                c.beginPath(); c.moveTo(x, mt); c.lineTo(x, mt + ph); c.stroke()
                c.fillStyle = Theme.textFaint
                c.fillText(s === 0 ? "现在" : "−" + Fmt.tick(s, tt.step) + " s", x, mt + ph + 7)
            }

            // Frame
            c.strokeStyle = Theme.axis
            c.beginPath(); c.moveTo(ml + 0.5, mt); c.lineTo(ml + 0.5, mt + ph + 0.5); c.lineTo(ml + pw, mt + ph + 0.5); c.stroke()

            // Series
            c.save()
            c.beginPath(); c.rect(ml, mt - 2, pw, ph + 4); c.clip()
            c.lineWidth = 1.6
            c.lineJoin = "round"
            for (let i = 0; i < w.series.length; ++i) {
                const a = axes[w.series[i].axis === "right" ? "right" : "left"]
                const buf = w.buffers[i] || []
                c.strokeStyle = w.colorOf(i)
                c.beginPath()
                let started = false
                for (const p of buf) {
                    if (p[0] < tMin - 1 || p[0] > tNow + 1) continue
                    const x = xOf(p[0]), y = a.yOf(p[1])
                    if (!started) { c.moveTo(x, y); started = true } else c.lineTo(x, y)
                }
                c.stroke()
            }
            c.restore()

            // Hover crosshair + values
            if (w.hoverX >= ml && w.hoverX <= ml + pw) {
                const th = tMin + (w.hoverX - ml) / pw * w.windowS
                c.strokeStyle = Theme.borderStrong
                c.beginPath(); c.moveTo(w.hoverX + 0.5, mt); c.lineTo(w.hoverX + 0.5, mt + ph); c.stroke()
                const lines = ["−" + (tNow - th).toFixed(2) + " s"]
                for (let i = 0; i < w.series.length; ++i) {
                    const buf = w.buffers[i] || []
                    if (buf.length === 0) continue
                    let lo = 0, hi = buf.length - 1
                    while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (buf[mid][0] < th) lo = mid; else hi = mid }
                    const p = Math.abs(buf[lo][0] - th) < Math.abs(buf[hi][0] - th) ? buf[lo] : buf[hi]
                    if (p[0] < tMin || Math.abs(p[0] - th) > w.windowS * 0.03) continue
                    const a = axes[w.series[i].axis === "right" ? "right" : "left"]
                    c.fillStyle = w.colorOf(i)
                    c.beginPath(); c.arc(xOf(p[0]), a.yOf(p[1]), 3.5, 0, Math.PI * 2); c.fill()
                    lines.push((w.series[i].label || w.series[i].field) + "  " + Fmt.si(p[1], w.series[i].unit || "", 5))
                }
                const bw = 150, bh = 10 + lines.length * 16
                let bx = w.hoverX + 12
                if (bx + bw > ml + pw) bx = w.hoverX - 12 - bw
                c.fillStyle = Theme.surface
                c.strokeStyle = Theme.border
                c.beginPath(); c.roundedRect(bx, mt + 6, bw, bh, 6, 6); c.fill(); c.stroke()
                c.textAlign = "left"
                c.textBaseline = "top"
                for (let k = 0; k < lines.length; ++k) {
                    c.fillStyle = k === 0 ? Theme.textMuted : Theme.text
                    c.fillText(lines[k], bx + 10, mt + 12 + k * 16)
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: mouse => { w.hoverX = mouse.x; canvas.requestPaint() }
            onExited: { w.hoverX = -1; canvas.requestPaint() }
        }

        Connections {
            target: Theme
            function onDarkChanged() { canvas.requestPaint() }
        }
    }
}
