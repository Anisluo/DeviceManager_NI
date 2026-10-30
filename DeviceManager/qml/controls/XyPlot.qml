import QtQuick
import DeviceManager
import "../js/Format.js" as Fmt

// Static XY plot (lines + markers) with SI-formatted axes and hover readout.
//   xs: [..]; series: [{ label, values: [..], xs?: [..] (own x values), unit, color }]
Canvas {
    id: plot
    property var xs: []
    property var series: []
    property string xLabel: ""
    property string xUnit: ""
    property bool markers: true
    property var hoverX: null            // x value under the mouse

    renderStrategy: Canvas.Cooperative
    onXsChanged: requestPaint()
    onSeriesChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Connections { target: Theme; function onDarkChanged() { plot.requestPaint() } }

    function colorOf(i) { return series[i].color || Theme.series[i % Theme.series.length] }

    function finite(v) { return typeof v === "number" && isFinite(v) }

    onPaint: {
        const c = getContext("2d")
        c.reset()
        const W = width, H = height
        c.font = "11px '" + Theme.sans + "'"

        let xlo = Infinity, xhi = -Infinity, ylo = Infinity, yhi = -Infinity
        for (const s of series) {
            for (const x of s.xs || xs) if (finite(x)) { xlo = Math.min(xlo, x); xhi = Math.max(xhi, x) }
            for (const y of s.values || []) if (finite(y)) { ylo = Math.min(ylo, y); yhi = Math.max(yhi, y) }
        }
        if (xlo === Infinity || ylo === Infinity) {
            c.fillStyle = Theme.textFaint
            c.textAlign = "center"
            c.fillText("暂无数据", W / 2, H / 2)
            return
        }
        const yUnit = series.length > 0 ? (series[0].unit || "") : ""
        const ypad = (yhi - ylo) * 0.06
        const xt = Fmt.niceTicks(xlo, xhi, Math.max(2, Math.floor(W / 110)))
        const yt = Fmt.niceTicks(ylo - ypad, yhi + ypad, Math.max(2, Math.floor(H / 48)))

        const ylabels = []
        for (let v = yt.min; v <= yt.max + yt.step * 0.5; v += yt.step) {
            const t = Fmt.tick(Math.abs(v) < yt.step * 1e-6 ? 0 : v, yt.step, yUnit)
            ylabels.push({ v: v, t: t, w: c.measureText(t).width })
        }
        const ml = Math.ceil(Math.max(40, ...ylabels.map(l => l.w)) + 14), mr = 14, mt = 10, mb = 40
        const pw = W - ml - mr, ph = H - mt - mb
        if (pw < 20 || ph < 20) return
        const X = x => ml + (x - xt.min) / (xt.max - xt.min) * pw
        const Y = y => mt + ph - (y - yt.min) / (yt.max - yt.min) * ph

        c.lineWidth = 1
        c.textBaseline = "middle"
        c.textAlign = "right"
        for (const l of ylabels) {
            const y = Math.round(Y(l.v)) + 0.5
            c.strokeStyle = Theme.grid
            c.beginPath(); c.moveTo(ml, y); c.lineTo(ml + pw, y); c.stroke()
            c.fillStyle = Theme.textMuted
            c.fillText(l.t, ml - 8, y)
        }
        c.textAlign = "center"
        c.textBaseline = "top"
        for (let v = xt.min; v <= xt.max + xt.step * 0.5; v += xt.step) {
            const x = Math.round(X(v)) + 0.5
            c.strokeStyle = Theme.grid
            c.beginPath(); c.moveTo(x, mt); c.lineTo(x, mt + ph); c.stroke()
            c.fillStyle = Theme.textMuted
            c.fillText(Fmt.tick(Math.abs(v) < xt.step * 1e-6 ? 0 : v, xt.step, xUnit), x, mt + ph + 6)
        }
        c.fillStyle = Theme.textFaint
        c.fillText(xLabel, ml + pw / 2, mt + ph + 22)

        c.strokeStyle = Theme.axis
        c.beginPath(); c.moveTo(ml + 0.5, mt); c.lineTo(ml + 0.5, mt + ph + 0.5); c.lineTo(ml + pw, mt + ph + 0.5); c.stroke()

        for (let i = 0; i < series.length; ++i) {
            const vals = series[i].values || []
            const sx = series[i].xs || xs
            c.strokeStyle = colorOf(i)
            c.fillStyle = colorOf(i)
            c.lineWidth = 1.6
            c.lineJoin = "round"
            c.beginPath()
            let started = false
            for (let k = 0; k < sx.length; ++k) {
                if (!finite(sx[k]) || !finite(vals[k])) { started = false; continue }
                if (!started) { c.moveTo(X(sx[k]), Y(vals[k])); started = true } else c.lineTo(X(sx[k]), Y(vals[k]))
            }
            c.stroke()
            if (markers && sx.length <= 400)
                for (let k = 0; k < sx.length; ++k)
                    if (finite(sx[k]) && finite(vals[k])) { c.beginPath(); c.arc(X(sx[k]), Y(vals[k]), 2.4, 0, Math.PI * 2); c.fill() }
        }

        // Legend (only when there is more than one curve)
        if (series.length > 1) {
            c.textAlign = "left"
            c.textBaseline = "middle"
            let lx = ml + pw - 8
            const entries = series.map((s, i) => ({ t: s.label || "", w: c.measureText(s.label || "").width, i: i }))
            lx -= entries.reduce((a, e) => a + e.w + 30, 0)
            for (const e of entries) {
                c.fillStyle = colorOf(e.i)
                c.fillRect(lx, mt + 8, 14, 3)
                c.fillStyle = Theme.textMuted
                c.fillText(e.t, lx + 18, mt + 9.5)
                lx += e.w + 30
            }
        }

        // Hover readout
        if (hoverX !== null) {
            const hx = X(hoverX)
            c.strokeStyle = Theme.borderStrong
            c.lineWidth = 1
            c.beginPath(); c.moveTo(hx + 0.5, mt); c.lineTo(hx + 0.5, mt + ph); c.stroke()
            const lines = [(xLabel || "x") + "  " + Fmt.si(hoverX, xUnit, 5)]
            for (let i = 0; i < series.length; ++i) {
                const sx = series[i].xs || xs
                let k = -1, best = Infinity
                for (let j = 0; j < sx.length; ++j) { const d = Math.abs(sx[j] - hoverX); if (d < best) { best = d; k = j } }
                const v = k >= 0 && best <= (xt.max - xt.min) * 0.02 ? (series[i].values || [])[k] : NaN
                if (!finite(v)) continue
                c.fillStyle = colorOf(i)
                c.beginPath(); c.arc(hx, Y(v), 4, 0, Math.PI * 2); c.fill()
                lines.push(series[i].label + "  " + Fmt.si(v, series[i].unit || "", 6))
            }
            const bw = 170, bh = 10 + lines.length * 16
            let bx = hx + 12
            if (bx + bw > ml + pw) bx = hx - 12 - bw
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
        plot.plotLeft = ml; plot.plotWidth = pw; plot.xMin = xt.min; plot.xMax = xt.max
    }

    // Geometry of the last paint, for hover hit-testing
    property real plotLeft: 0
    property real plotWidth: 1
    property real xMin: 0
    property real xMax: 1

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onExited: { plot.hoverX = null; plot.requestPaint() }
        onPositionChanged: mouse => {
            const x = plot.xMin + (mouse.x - plot.plotLeft) / plot.plotWidth * (plot.xMax - plot.xMin)
            // snap to the nearest x of any series
            let best = null, bestD = Infinity
            for (const s of plot.series) for (const v of (s.xs || plot.xs)) {
                const d = Math.abs(v - x)
                if (d < bestD) { bestD = d; best = v }
            }
            if (best !== plot.hoverX) { plot.hoverX = best; plot.requestPaint() }
        }
    }
}
