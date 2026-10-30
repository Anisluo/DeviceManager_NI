import QtQuick
import DeviceManager
import "../js/Format.js" as Fmt

// Shmoo / heat map over two axes.
//   xs, ys, values: one entry per measured point (same length; order irrelevant)
//   values: true/false (pass/fail) or numbers (sequential colour scale)
Canvas {
    id: map
    property var xs: []
    property var ys: []
    property var values: []
    property string xLabel: ""
    property string yLabel: ""
    property string xUnit: ""
    property string yUnit: ""
    property string valueUnit: ""
    property var hover: null          // { i, j } cell under the mouse

    renderStrategy: Canvas.Cooperative
    onXsChanged: requestPaint()
    onValuesChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Connections { target: Theme; function onDarkChanged() { map.requestPaint() } }

    // Derived grid (recomputed in paint)
    property var ux: []
    property var uy: []
    property var cells: ({})
    property real gx: 0
    property real gy: 0
    property real cw: 1
    property real ch: 1

    function uniqueSorted(a) {
        return Array.from(new Set(a.filter(v => typeof v === "number"))).sort((p, q) => p - q)
    }
    function isBool(v) { return v === true || v === false }

    function mix(a, b, t) {   // hex colours
        const pa = [1, 3, 5].map(k => parseInt(a.substr(k, 2), 16))
        const pb = [1, 3, 5].map(k => parseInt(b.substr(k, 2), 16))
        return "rgb(" + pa.map((v, k) => Math.round(v + (pb[k] - v) * t)).join(",") + ")"
    }

    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.font = "11px '" + Theme.sans + "'"
        ux = uniqueSorted(xs)
        uy = uniqueSorted(ys)
        if (ux.length === 0 || uy.length === 0) {
            c.fillStyle = Theme.textFaint
            c.textAlign = "center"
            c.fillText("暂无数据", width / 2, height / 2)
            return
        }
        const grid = {}
        let lo = Infinity, hi = -Infinity, numeric = false
        // pass/fail read back from SQLite arrives as 1/0: treat a 0/1-only map as boolean
        const binary = values.every(v => v === true || v === false || v === 0 || v === 1 || v === null || v === undefined)
        for (let k = 0; k < values.length; ++k) {
            const i = ux.indexOf(xs[k]), j = uy.indexOf(ys[k])
            if (i < 0 || j < 0) continue
            grid[i + "," + j] = binary && typeof values[k] === "number" ? values[k] === 1 : values[k]
            if (!binary && typeof values[k] === "number") { numeric = true; lo = Math.min(lo, values[k]); hi = Math.max(hi, values[k]) }
        }
        cells = grid

        const ylabels = uy.map(v => Fmt.si(v, yUnit, 3))
        const ml = Math.ceil(Math.max(30, ...ylabels.map(t => c.measureText(t).width)) + 14) + (yLabel ? 18 : 0)
        const mb = 40, mt = 8, mr = numeric ? 70 : 12
        gx = ml; gy = mt
        cw = (width - ml - mr) / ux.length
        ch = (height - mt - mb) / uy.length
        const gap = Math.min(cw, ch) > 8 ? 1.5 : 0

        const passC = Theme.dark ? "#5E8F60" : "#7FAE7F"
        const failC = Theme.dark ? "#B4574A" : "#D7826F"
        const low = Theme.dark ? "#2F3D46" : "#EEF2F5", high = Theme.dark ? "#E08A6C" : "#C4643F"
        for (let i = 0; i < ux.length; ++i) for (let j = 0; j < uy.length; ++j) {
            const v = grid[i + "," + j]
            let col = Theme.surfaceAlt
            if (v === true) col = passC
            else if (v === false) col = failC
            else if (typeof v === "number") col = mix(low, high, hi > lo ? (v - lo) / (hi - lo) : 0.5)
            c.fillStyle = col
            // y grows upwards
            c.fillRect(gx + i * cw + gap / 2, gy + (uy.length - 1 - j) * ch + gap / 2, cw - gap, ch - gap)
        }

        // Axes labels (thinned to avoid overlap)
        c.fillStyle = Theme.textMuted
        c.textAlign = "right"
        c.textBaseline = "middle"
        const ystep = Math.max(1, Math.ceil(14 / ch))
        for (let j = 0; j < uy.length; j += ystep)
            c.fillText(ylabels[j], gx - 8, gy + (uy.length - 1 - j + 0.5) * ch)
        c.textAlign = "center"
        c.textBaseline = "top"
        const xl = ux.map(v => Fmt.si(v, xUnit, 3))
        const xw = Math.max(...xl.map(t => c.measureText(t).width)) + 10
        const xstep = Math.max(1, Math.ceil(xw / cw))
        for (let i = 0; i < ux.length; i += xstep)
            c.fillText(xl[i], gx + (i + 0.5) * cw, gy + uy.length * ch + 6)
        c.fillStyle = Theme.textFaint
        c.fillText(xLabel, gx + ux.length * cw / 2, gy + uy.length * ch + 22)
        c.save()
        c.translate(4, gy + uy.length * ch / 2)
        c.rotate(-Math.PI / 2)
        c.textBaseline = "top"
        c.fillText(yLabel, 0, 0)
        c.restore()

        // Colour bar for numeric maps
        if (numeric) {
            const bx = width - mr + 16, bh = uy.length * ch
            for (let k = 0; k < bh; ++k) {
                c.fillStyle = mix(low, high, 1 - k / bh)
                c.fillRect(bx, gy + k, 12, 1)
            }
            c.fillStyle = Theme.textMuted
            c.textAlign = "left"
            c.textBaseline = "top"
            c.fillText(Fmt.si(hi, valueUnit, 3), bx + 16, gy)
            c.textBaseline = "bottom"
            c.fillText(Fmt.si(lo, valueUnit, 3), bx + 16, gy + bh)
        }

        // Hover outline + tooltip
        if (hover) {
            const x0 = gx + hover.i * cw, y0 = gy + (uy.length - 1 - hover.j) * ch
            c.strokeStyle = Theme.text
            c.lineWidth = 1.5
            c.strokeRect(x0 + 0.5, y0 + 0.5, cw - 1, ch - 1)
            const v = grid[hover.i + "," + hover.j]
            const lines = [xLabel + "  " + Fmt.si(ux[hover.i], xUnit, 4), yLabel + "  " + Fmt.si(uy[hover.j], yUnit, 4),
                           v === undefined ? "未测" : isBool(v) ? (v ? "通过" : "失败") : Fmt.si(v, valueUnit, 5)]
            const bw = 150, bh = 10 + lines.length * 16
            let tx = x0 + cw + 8, ty = y0
            if (tx + bw > width) tx = x0 - bw - 8
            if (ty + bh > height) ty = height - bh
            c.fillStyle = Theme.surface
            c.strokeStyle = Theme.border
            c.lineWidth = 1
            c.beginPath(); c.roundedRect(tx, ty, bw, bh, 6, 6); c.fill(); c.stroke()
            c.textAlign = "left"
            c.textBaseline = "top"
            for (let k = 0; k < lines.length; ++k) {
                c.fillStyle = k === 2 ? Theme.text : Theme.textMuted
                c.fillText(lines[k], tx + 10, ty + 6 + k * 16)
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onExited: { map.hover = null; map.requestPaint() }
        onPositionChanged: mouse => {
            const i = Math.floor((mouse.x - map.gx) / map.cw)
            const j = map.uy.length - 1 - Math.floor((mouse.y - map.gy) / map.ch)
            const h = (i >= 0 && i < map.ux.length && j >= 0 && j < map.uy.length) ? { i: i, j: j } : null
            if (JSON.stringify(h) !== JSON.stringify(map.hover)) { map.hover = h; map.requestPaint() }
        }
    }
}
