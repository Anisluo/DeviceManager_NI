import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "edit_table", source, rows_field, key: ["timeset", "pin"], write: "set_edge", height, empty,
//   rev_field,      rows only change when this value does (for long tables)
//   also: ["tl"],   sources to refresh after a write,
//   columns: [{ field, label, width, name, mono, editor: "text"|"combo", options, options_field, readonly_if }],
//   pin_columns: { field: "pin_list", states: "states", after: 2 },
//   row_actions: [{ label, method }] }
// Editable table. Each edit calls write with the key fields plus { field: <column name or field>,
// value: <text> }; a pin column writes field "pin:<pin>". Rows are virtualised, so a long table
// stays light, and the delegates survive a refresh, so the cell being edited keeps its focus.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var sourceData: ({})
    property var rows: []
    property var rev: undefined
    property string error: ""
    property int tick: 0                    // a write finished: cells drop text that was refused
    readonly property int rowH: 36
    readonly property var pins: spec.pin_columns ? (Fmt.field(sourceData, spec.pin_columns.field) || []) : []
    readonly property var actions: spec.row_actions || []
    readonly property real actionsWidth: actions.reduce((sum, a) => sum + (a.width || 58), 0)
    readonly property var columns: {
        const base = (spec.columns || []).slice()
        if (!spec.pin_columns) return base
        const cols = pins.map((pin, k) => ({ pin: pin, pinIndex: k, label: pin, width: 42, editor: "state" }))
        const at = spec.pin_columns.after || 0
        return base.slice(0, at).concat(cols, base.slice(at))
    }
    readonly property real fixedWidth: columns.reduce((sum, c) => sum + (c.width || 0), 0) + actionsWidth
    readonly property int flexCount: columns.filter(c => !c.width).length
    readonly property real flexWidth: flexCount ? Math.max(100, (list.width - fixedWidth) / flexCount) : 0
    readonly property real totalWidth: Math.max(list.width, fixedWidth + flexCount * flexWidth)
    spacing: 6

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id !== w.spec.source) return
            const r = w.spec.rows_field ? Fmt.field(data, w.spec.rows_field) : [data]
            if (!Array.isArray(r)) return
            const rev = w.spec.rev_field ? Fmt.field(data, w.spec.rev_field) : JSON.stringify(data)
            if (rev === w.rev) return
            w.rev = rev
            const y = list.contentY, resized = r.length !== w.rows.length
            w.sourceData = data
            w.rows = r
            if (resized) Qt.callLater(() => { list.contentY = y })   // a new row count resets the view
        }
    }

    function widthOf(col) { return col.width || flexWidth }
    function valueOf(row, col) {
        if (col.pin !== undefined) return String(Fmt.field(row, spec.pin_columns.states) || "").charAt(col.pinIndex)
        const v = Fmt.field(row, col.field)
        return v === undefined || v === null ? "" : String(v)
    }
    function optionsOf(col) { return col.options_field ? (Fmt.field(sourceData, col.options_field) || []) : (col.options || []) }

    function call(method, args) {
        ctx.device.call(method, args, (ok, res) => {
            error = ok ? "" : (res && res.error ? res.error : "写入失败")
            if (!ok) ctx.deviceEvent({ type: "log", level: "error", message: method + "：" + error })
            tick++
            ctx.refresh(spec.source)
            for (const id of spec.also || []) ctx.refresh(id)
        })
    }
    function keyArgs(row) {
        const args = {}
        for (const k of spec.key || []) args[k] = Fmt.field(row, k)
        return args
    }
    function commit(row, col, value) {
        const args = keyArgs(row)
        args.field = col.pin !== undefined ? "pin:" + col.pin : (col.name || col.field)
        args.value = value
        call(spec.write, args)
    }
    function moveTo(r, c) {
        if (r < 0 || r >= rows.length || c < 0 || c >= columns.length) return
        list.positionViewAtIndex(r, ListView.Contain)
        const item = list.itemAtIndex(r)
        if (item) item.focusColumn(c)
    }

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(w.spec.height || 480, w.rowH * (w.rows.length + 1) + 12)
        clip: true
        model: w.rows.length
        contentWidth: w.totalWidth
        flickableDirection: Flickable.AutoFlickIfNeeded
        boundsBehavior: Flickable.StopAtBounds
        headerPositioning: ListView.OverlayHeader
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        header: Rectangle {
            z: 2
            width: w.totalWidth
            height: w.rowH
            color: Theme.surface
            Row {
                anchors.fill: parent
                Repeater {
                    model: w.columns
                    delegate: Text {
                        required property var modelData
                        width: w.widthOf(modelData)
                        height: w.rowH
                        leftPadding: modelData.editor === "state" ? 0 : 6
                        horizontalAlignment: modelData.editor === "state" ? Text.AlignHCenter : Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.label || modelData.field
                        color: Theme.textMuted
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSm
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        delegate: Item {
            id: rowItem
            required property int index
            readonly property var row: w.rows[index] || ({})
            readonly property int rowIndex: index
            width: w.totalWidth
            height: w.rowH

            function focusColumn(c) {
                const cell = cells.itemAt(c)
                if (cell && cell.item && cell.item.takeFocus) cell.item.takeFocus()
            }

            Row {
                anchors.fill: parent
                Repeater {
                    id: cells
                    model: w.columns
                    delegate: Loader {
                        id: cell
                        required property var modelData
                        required property int index
                        readonly property var col: modelData
                        readonly property int colIndex: index
                        readonly property string value: w.valueOf(rowItem.row, col)
                        readonly property bool locked: !!(col.readonly_if && Fmt.field(rowItem.row, col.readonly_if))
                        width: w.widthOf(col)
                        height: w.rowH
                        sourceComponent: locked || !col.editor ? viewCell
                                       : col.editor === "state" ? stateCell
                                       : col.editor === "combo" ? comboCell : textCell

                        Component {
                            id: viewCell
                            Text {
                                leftPadding: 6
                                rightPadding: 6
                                verticalAlignment: Text.AlignVCenter
                                text: cell.locked ? "—" : cell.value
                                color: cell.locked ? Theme.textFaint : Theme.text
                                font.family: cell.col.mono ? Theme.mono : Theme.sans
                                font.pixelSize: Theme.fontSm
                                elide: Text.ElideRight
                            }
                        }
                        Component {
                            id: textCell
                            Rectangle {
                                function takeFocus() { input.forceActiveFocus(); input.selectAll() }
                                color: input.activeFocus ? Theme.bg : "transparent"
                                radius: 4
                                border.width: input.activeFocus ? 1.5 : area.containsMouse ? 1 : 0
                                border.color: input.activeFocus ? Theme.accent : Theme.border
                                MouseArea { id: area; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                                TextInput {
                                    id: input
                                    anchors.fill: parent
                                    anchors.leftMargin: 6
                                    anchors.rightMargin: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    clip: true
                                    selectByMouse: true
                                    color: Theme.text
                                    selectionColor: Theme.accentSoft
                                    selectedTextColor: Theme.text
                                    font.family: cell.col.mono ? Theme.mono : Theme.sans
                                    font.pixelSize: Theme.fontSm
                                    readonly property string shown: cell.value
                                    onShownChanged: if (!activeFocus) text = shown
                                    Component.onCompleted: text = shown
                                    onEditingFinished: if (text !== shown) w.commit(rowItem.row, cell.col, text)
                                    onActiveFocusChanged: if (activeFocus) selectAll()
                                    Keys.onEscapePressed: { text = shown; focus = false }
                                    Keys.onUpPressed: w.moveTo(rowItem.rowIndex - 1, cell.colIndex)
                                    Keys.onDownPressed: w.moveTo(rowItem.rowIndex + 1, cell.colIndex)
                                    Connections {
                                        target: w
                                        function onTickChanged() { if (!input.activeFocus) input.text = input.shown }
                                    }
                                }
                            }
                        }
                        Component {
                            id: comboCell
                            DmComboBox {
                                function takeFocus() { forceActiveFocus() }
                                anchors.fill: parent
                                anchors.margins: 1
                                font.pixelSize: Theme.fontSm
                                model: w.optionsOf(cell.col)
                                currentIndex: w.optionsOf(cell.col).indexOf(cell.value)
                                onActivated: index => {
                                    const v = w.optionsOf(cell.col)[index]
                                    if (v !== cell.value) w.commit(rowItem.row, cell.col, v)
                                }
                            }
                        }
                        Component {
                            id: stateCell
                            Rectangle {
                                id: pinCell
                                function takeFocus() { forceActiveFocus() }
                                readonly property string s: cell.value
                                anchors.fill: parent
                                anchors.margins: 2
                                radius: 3
                                activeFocusOnTab: true
                                color: s === "1" || s === "H" ? Theme.accentSoft : s === "X" ? "transparent" : Theme.surfaceAlt
                                border.width: activeFocus ? 1.5 : 0
                                border.color: Theme.accent
                                Text {
                                    anchors.centerIn: parent
                                    text: pinCell.s
                                    color: pinCell.s === "X" ? Theme.textFaint : pinCell.s === "H" || pinCell.s === "L" ? Theme.toneColor("warning") : Theme.text
                                    font.family: Theme.mono
                                    font.pixelSize: Theme.fontSm
                                    font.weight: Font.DemiBold
                                }
                                MouseArea { anchors.fill: parent; onClicked: pinCell.forceActiveFocus() }
                                Keys.onPressed: e => {
                                    const c = e.text.toUpperCase()
                                    if (c.length === 1 && "01LHXMVDE".indexOf(c) >= 0) {
                                        if (c !== pinCell.s) w.commit(rowItem.row, cell.col, c)
                                        w.moveTo(rowItem.rowIndex + 1, cell.colIndex)
                                    } else if (e.key === Qt.Key_Up) w.moveTo(rowItem.rowIndex - 1, cell.colIndex)
                                    else if (e.key === Qt.Key_Down) w.moveTo(rowItem.rowIndex + 1, cell.colIndex)
                                    else if (e.key === Qt.Key_Left) w.moveTo(rowItem.rowIndex, cell.colIndex - 1)
                                    else if (e.key === Qt.Key_Right) w.moveTo(rowItem.rowIndex, cell.colIndex + 1)
                                    else return
                                    e.accepted = true
                                }
                            }
                        }
                    }
                }
                Repeater {
                    model: w.actions
                    delegate: DmButton {
                        required property var modelData
                        width: modelData.width || 58
                        y: (w.rowH - height) / 2
                        compact: true
                        kind: "ghost"
                        text: modelData.label
                        onClicked: w.call(modelData.method, w.keyArgs(rowItem.row))
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border; opacity: 0.5 }
        }
    }

    Text {
        visible: w.rows.length === 0
        text: w.spec.empty || "等待数据…"
        color: Theme.textFaint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
    Text {
        visible: w.error !== ""
        Layout.fillWidth: true
        text: w.error
        wrapMode: Text.Wrap
        color: Theme.error
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
