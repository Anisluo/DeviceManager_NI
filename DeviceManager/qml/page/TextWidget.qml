import QtQuick
import QtQuick.Layouts
import DeviceManager

// { type: "text", id, label, value, placeholder, mono, action }
// Free text (paths, pattern labels…). With an action it is sent on Enter ("$value");
// without one it is stored as $form.<id>.
RowLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    spacing: 10

    function apply(v) {
        field.text = String(v)
        commit()
    }
    function commit() {
        if (!spec.action) { if (spec.id) ctx.form[spec.id] = field.text; return }
        ctx.act(spec.action, field.text)
    }

    Component.onCompleted: {
        field.text = spec.value !== undefined ? String(spec.value) : ""
        if (!spec.action && spec.id) ctx.form[spec.id] = field.text
    }

    Text {
        text: w.spec.label || ""
        visible: text !== ""
        color: Theme.textMuted
        font.family: Theme.sans
        font.pixelSize: Theme.fontMd
        Layout.preferredWidth: 72
        elide: Text.ElideRight
    }
    DmTextField {
        id: field
        Layout.fillWidth: true
        placeholderText: w.spec.placeholder || ""
        font.family: w.spec.mono ? Theme.mono : Theme.sans
        onAccepted: w.commit()
        // kept current while typing: clicking a button does not take the focus from the field
        onTextChanged: if (!w.spec.action && w.spec.id) w.ctx.form[w.spec.id] = text
    }
}
