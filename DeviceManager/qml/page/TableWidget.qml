import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "table", source, rows_field?, height,
//   columns: [{ field, label, unit, digits, format: "hex", tones: { "<value>": "ok|warning|error|accent" } }] }
// With rows_field the source value is an array of row objects; without it the
// table shows one row built from the source object itself.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var rows: []
    readonly property var columns: spec.columns || []
    spacing: 0

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id !== w.spec.source) return
            const r = w.spec.rows_field ? Fmt.field(data, w.spec.rows_field) : [data]
            w.rows = Array.isArray(r) ? r : []
        }
    }

    function tone(row, col) {
        if (!col.tones) return ""
        return col.tones[String(Fmt.field(row, col.field))] || ""
    }
    function cell(row, col) {
        const v = Fmt.field(row, col.field)
        if (col.format === "hex") return Fmt.hex(v, col.digits || 8)
        if (v === true) return "✓"
        if (v === false) return "✕"
        if (typeof v === "number" && col.unit !== undefined) return Fmt.si(v, col.unit, col.digits || 5)
        if (typeof v === "number" && col.digits) return String(Number(v.toPrecision(col.digits)))
        if (typeof v === "number") return String(Math.round(v * 1e6) / 1e6)
        return v === undefined ? "—" : String(v)
    }

    // Header
    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        Repeater {
            model: w.columns
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: modelData.label || modelData.field
                color: Theme.textMuted
                font.family: Theme.sans
                font.pixelSize: Theme.fontSm
                font.weight: Font.DemiBold
            }
        }
    }
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

    Repeater {
        model: w.rows
        delegate: ColumnLayout {
            id: rowItem
            required property var modelData
            Layout.fillWidth: true
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 7
                Layout.bottomMargin: 7
                Repeater {
                    model: w.columns
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: w.cell(rowItem.modelData, modelData)
                        color: w.tone(rowItem.modelData, modelData) ? Theme.toneColor(w.tone(rowItem.modelData, modelData)) : Theme.text
                        font.weight: w.tone(rowItem.modelData, modelData) ? Font.DemiBold : Font.Normal
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                        font.features: { "tnum": 1 }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; opacity: 0.6 }
        }
    }

    Text {
        visible: w.rows.length === 0
        Layout.topMargin: 10
        text: "等待数据…"
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
