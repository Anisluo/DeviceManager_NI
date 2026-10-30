import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "params", source, write: "param_write", group: "电流环" (optional) }
// Editable table of the parameters a device reports: rows { name, group, value, unit, min, max,
// type: float|int|bool|enum, readonly, enums, note (optional text after the range) }. Enter writes; the device's answer (possibly
// clamped) is shown. Rows are grouped by `group`.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var rows: []
    property var status: ({})       // name -> "ok" | "clamped" | "error"
    spacing: 2

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id !== w.spec.source || !data || !data.rows) return
            w.rows = w.spec.group ? data.rows.filter(r => r.group === w.spec.group) : data.rows
        }
    }

    function write(row, value) {
        const st = Object.assign({}, status)
        w.ctx.device.call(w.spec.write || "param_write", { name: row.name, value: value }, (ok, res) => {
            st[row.name] = !ok ? "error" : res.clamped ? "clamped" : "ok"
            w.status = st
            if (ok) {
                const copy = w.rows.slice()
                const i = copy.findIndex(r => r.name === row.name)
                if (i >= 0) { copy[i] = Object.assign({}, copy[i], { value: res.value }); w.rows = copy }
            }
        })
    }

    Repeater {
        model: w.rows
        delegate: ColumnLayout {
            id: rowItem
            required property var modelData
            required property int index
            readonly property var row: modelData
            readonly property bool newGroup: index === 0 || w.rows[index - 1].group !== row.group
            Layout.fillWidth: true
            spacing: 0

            Text {
                visible: rowItem.newGroup && !w.spec.group
                text: rowItem.row.group
                color: Theme.text
                font.family: Theme.serif
                font.pixelSize: Theme.fontLg
                topPadding: rowItem.index === 0 ? 0 : 14
                bottomPadding: 6
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                spacing: 10
                Text {
                    text: rowItem.row.name
                    Layout.preferredWidth: 120
                    color: rowItem.row.readonly ? Theme.textMuted : Theme.text
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSm
                    elide: Text.ElideRight
                }
                Item {
                    // with a note the editor keeps a fixed width and the note takes the rest
                    Layout.fillWidth: !rowItem.row.note
                    Layout.preferredWidth: rowItem.row.note ? 150 : -1
                    Layout.preferredHeight: 34
                    // read-only
                    Text {
                        visible: rowItem.row.readonly
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.row.type === "enum" ? (rowItem.row.enums[rowItem.row.value] || rowItem.row.value)
                            : rowItem.row.type === "bool" ? (rowItem.row.value ? "是" : "否")
                            : typeof rowItem.row.value === "number" ? String(Number(rowItem.row.value.toPrecision(6))) : String(rowItem.row.value)
                        color: Theme.textMuted
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                    }
                    // number
                    DmTextField {
                        id: numField
                        visible: !rowItem.row.readonly && (rowItem.row.type === "float" || rowItem.row.type === "int")
                        anchors.fill: parent
                        horizontalAlignment: Text.AlignRight
                        invalid: w.status[rowItem.row.name] === "error"
                        readonly property string shown: typeof rowItem.row.value === "number" ? String(Number(rowItem.row.value.toPrecision(6))) : ""
                        onShownChanged: if (!activeFocus) text = shown
                        Component.onCompleted: text = shown
                        onAccepted: { const v = Fmt.parseNumber(text); if (!isNaN(v)) w.write(rowItem.row, v) }
                        onActiveFocusChanged: if (!activeFocus) text = shown
                    }
                    // enum
                    DmComboBox {
                        visible: !rowItem.row.readonly && rowItem.row.type === "enum"
                        anchors.fill: parent
                        model: rowItem.row.enums || []
                        currentIndex: rowItem.row.value
                        onActivated: index => w.write(rowItem.row, index)
                    }
                    // bool
                    DmSwitch {
                        visible: !rowItem.row.readonly && rowItem.row.type === "bool"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: !!rowItem.row.value
                        text: checked ? "开" : "关"
                        onToggled: w.write(rowItem.row, checked)
                    }
                }
                Text {
                    text: rowItem.row.unit || ""
                    Layout.preferredWidth: 70
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                }
                Text {
                    visible: text.length > 0 || !rowItem.row.note     // an empty range gives its room to the note
                    text: rowItem.row.readonly ? "只读"
                        : (rowItem.row.min < rowItem.row.max && rowItem.row.type !== "bool" && rowItem.row.type !== "enum")
                          ? Fmt.eng(rowItem.row.min, 3) + " … " + Fmt.eng(rowItem.row.max, 3) : ""
                    Layout.preferredWidth: 110
                    color: w.status[rowItem.row.name] === "clamped" ? Theme.warning : Theme.textFaint
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontXs
                }
                // optional free-text note (e.g. what a count means)
                Text {
                    visible: !!rowItem.row.note
                    text: rowItem.row.note || ""
                    Layout.fillWidth: true
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    elide: Text.ElideRight
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; opacity: 0.5 }
        }
    }

    Text {
        visible: w.rows.length === 0
        text: w.spec.empty || "等待参数表…（先连接控制板）"
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
