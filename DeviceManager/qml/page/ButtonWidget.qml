import QtQuick
import QtQuick.Layouts
import DeviceManager

// { type: "button", label, style: "primary"|"secondary"|"ghost", action, reload_page,
//   publish: "<source id>" — the result feeds readouts / tables bound to that source,
//   refresh: ["<source id>"] — poll these sources right after the call }
RowLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property bool busy: false

    function apply() {
        if (busy) return
        busy = true
        ctx.act(spec.action, undefined, (ok, res) => {
            busy = false
            if (ok && spec.publish) ctx.publish(spec.publish, res)
            if (ok && spec.reload_page) ctx.reinitRequested()
            if (ok) for (const id of spec.refresh || []) ctx.refresh(id)
        })
    }

    DmButton {
        Layout.fillWidth: true
        text: (w.busy ? "… " : "") + (w.spec.label || "执行")
        kind: w.spec.style || "primary"
        enabled: !w.busy && w.ctx && w.ctx.device && w.ctx.device.online
        onClicked: w.apply()
    }
}
