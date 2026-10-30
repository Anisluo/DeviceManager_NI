import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "readout", label, source, field | expr, unit, digits,
//   states: { "<value>": { text, tone: ok|warning|error|accent } } }
Rectangle {
    id: w
    property var spec: ({})
    property var ctx: null
    property var value: undefined
    property real updatedAt: 0

    implicitHeight: 92
    radius: Theme.radiusSm + 2
    color: Theme.surfaceAlt
    border.color: Theme.border

    readonly property var stateSpec: spec.states && value !== undefined ? spec.states[String(value)] : undefined
    readonly property var parts: spec.format === "hex" ? { number: Fmt.hex(value, spec.hex_digits || 8), unit: "" }
                                 : typeof value === "string" ? { number: value, unit: "" }
                                 : Fmt.siSplit(value, spec.unit || "", spec.digits || 5)
    readonly property bool stale: !ctx || !ctx.polling || !ctx.device || !ctx.device.online

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id !== w.spec.source) return
            w.value = w.spec.expr ? Fmt.evalExpr(w.spec.expr, data) : Fmt.field(data, w.spec.field)
            w.updatedAt = Date.now()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        Text {
            text: w.spec.label || w.spec.field || ""
            color: Theme.textMuted
            font.family: Theme.sans
            font.pixelSize: Theme.fontSm
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        // Numeric value
        RowLayout {
            visible: !w.stateSpec
            spacing: 6
            Layout.fillWidth: true
            Text {
                text: w.parts.number
                color: w.stale ? Theme.textFaint : Theme.text
                font.family: Theme.sans
                // long strings (paths, target names) get a smaller font and are elided in the middle
                font.pixelSize: String(w.parts.number).length > 14 ? 16 : 28
                elide: Text.ElideMiddle
                Layout.fillWidth: String(w.parts.number).length > 14
                Layout.maximumWidth: w.width - 28
                font.weight: Font.Light
                font.features: { "tnum": 1 }
            }
            Text {
                text: w.parts.unit
                color: Theme.textMuted
                font.family: Theme.sans
                font.pixelSize: Theme.fontLg
                Layout.alignment: Qt.AlignBaseline
            }
        }

        // State pill
        Rectangle {
            visible: !!w.stateSpec
            Layout.topMargin: 6
            radius: 12
            implicitHeight: 26
            implicitWidth: pill.implicitWidth + 24
            color: w.stateSpec ? Theme.toneSoft(w.stateSpec.tone) : "transparent"
            Text {
                id: pill
                anchors.centerIn: parent
                text: w.stateSpec ? w.stateSpec.text : ""
                color: w.stateSpec ? Theme.toneColor(w.stateSpec.tone) : Theme.text
                font.family: Theme.sans
                font.pixelSize: Theme.fontMd
                font.weight: Font.DemiBold
            }
        }
        Item { Layout.fillHeight: true }
    }
}
