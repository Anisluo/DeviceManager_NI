import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "number", label, unit, min, max, step, decimals, format: "hex", value, init, action }
// Applies on Enter / focus-out; ↑/↓ step the value; accepts SI suffixes (10m, 2.5k) and 0x… hex.
// Without an action the value is only stored as $form.<id> for buttons to use.
RowLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property real value: spec.value !== undefined ? spec.value : 0
    property bool pending: false
    property bool failed: false
    readonly property int decimals: spec.decimals !== undefined ? spec.decimals : 3

    spacing: 10

    function display(v) { return spec.format === "hex" ? Fmt.hex(v, spec.digits || 8) : Number(v).toFixed(decimals) }
    function clamp(v) {
        if (spec.min !== undefined) v = Math.max(spec.min, v)
        if (spec.max !== undefined) v = Math.min(spec.max, v)
        return v
    }
    function apply(v) {
        if (typeof v === "string") v = Fmt.parseNumber(v)
        if (isNaN(v)) { failed = true; return }
        v = clamp(v)
        value = v
        field.text = display(v)
        failed = false
        if (!spec.action) { if (spec.id) ctx.form[spec.id] = v; return }
        pending = true
        ctx.act(spec.action, v, ok => { pending = false; failed = !ok })
    }
    function reinit() {
        ctx.read(spec.init, v => { if (typeof v === "number") { value = v; field.text = display(v) } })
    }

    Component.onCompleted: {
        field.text = display(value)
        if (!spec.action && spec.id) ctx.form[spec.id] = value
        reinit()
    }
    Connections { target: w.ctx; function onReinitRequested() { w.reinit() } }

    Text {
        text: w.spec.label || ""
        color: Theme.textMuted
        font.family: Theme.sans
        font.pixelSize: Theme.fontMd
        Layout.preferredWidth: 72
        elide: Text.ElideRight
    }
    DmTextField {
        id: field
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignRight
        invalid: w.failed
        enabled: w.ctx && w.ctx.device && w.ctx.device.online
        onAccepted: w.apply(Fmt.parseNumber(text))
        onEditingFinished: if (text !== w.display(w.value)) w.apply(Fmt.parseNumber(text))
        Keys.onUpPressed: w.apply(w.value + (w.spec.step || 1))
        Keys.onDownPressed: w.apply(w.value - (w.spec.step || 1))

        Rectangle {   // in-flight indicator
            visible: w.pending
            width: 6; height: 6; radius: 3
            color: Theme.accent
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    Text {
        text: w.spec.unit || ""
        color: Theme.textMuted
        font.family: Theme.sans
        font.pixelSize: Theme.fontMd
        Layout.preferredWidth: 18
    }
}
