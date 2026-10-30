import QtQuick
import QtQuick.Layouts
import DeviceManager

// { type: "combo", label, options: [{label, value}] | ["a","b"], options_from: { method, args, field }, init, action }
// Without an action the selection is stored as $form.<id>.
RowLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var rawOptions: spec.options || []
    readonly property var options: rawOptions.map(o => typeof o === "object" ? o : ({ label: String(o), value: o }))
    function store(i) { if (!spec.action && spec.id && options[i]) ctx.form[spec.id] = options[i].value }

    spacing: 10

    function indexOfValue(v) {
        for (let i = 0; i < options.length; ++i) if (options[i].value === v) return i
        return -1
    }
    function apply(v) {
        const i = indexOfValue(v)
        if (i < 0) return
        box.currentIndex = i
        store(i)
        if (spec.action) ctx.act(spec.action, options[i].value)
    }
    function reinit() {
        ctx.read(spec.init, v => { const i = indexOfValue(v); if (i >= 0) box.currentIndex = i })
    }
    Component.onCompleted: {
        if (spec.options_from)
            ctx.read(spec.options_from, list => {
                if (!Array.isArray(list)) return
                rawOptions = list
                box.currentIndex = Math.max(0, indexOfValue(spec.value))
                store(box.currentIndex)
            })
        if (spec.value !== undefined) box.currentIndex = Math.max(0, indexOfValue(spec.value))
        store(box.currentIndex)
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
    DmComboBox {
        id: box
        Layout.fillWidth: true
        model: w.options
        textRole: "label"
        enabled: w.ctx && w.ctx.device && w.ctx.device.online
        onActivated: index => { w.store(index); if (w.spec.action) w.ctx.act(w.spec.action, w.options[index].value) }
    }
    Item { Layout.preferredWidth: 18 }
}
