import QtQuick
import QtQuick.Layouts
import DeviceManager

// { type: "switch", label, value, init, action }  — "$value" is true/false
RowLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    spacing: 10

    function reinit() { ctx.read(spec.init, v => sw.checked = !!v) }
    function apply(v) {
        const want = v === true || v === "true" || v === 1 || v === "1"
        sw.checked = want
        ctx.act(spec.action, want, ok => { if (!ok) sw.checked = !want })
    }
    Component.onCompleted: { sw.checked = !!spec.value; reinit() }
    Connections { target: w.ctx; function onReinitRequested() { w.reinit() } }

    Text {
        text: w.spec.label || ""
        color: Theme.textMuted
        font.family: Theme.sans
        font.pixelSize: Theme.fontMd
        Layout.preferredWidth: 72
        elide: Text.ElideRight
    }
    DmSwitch {
        id: sw
        enabled: w.ctx && w.ctx.device && w.ctx.device.online
        text: checked ? "开" : "关"
        onToggled: w.apply(checked)
    }
    Item { Layout.fillWidth: true }
}
