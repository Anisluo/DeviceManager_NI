import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// Spreadsheet: { type: "edit_table", source, rows_field, key: ["timeset", "pin"], write: "set_edge", height, empty,
//   rev_field,        rows only change when this value does (for long tables)
//   write_many,       method taking { cells: [...] } for several cells at once
//   undo,             method for Ctrl+Z
//   also: ["tl"],     sources to refresh after a write,
//   columns: [{ field, label, width, name, mono, editor: "text"|"combo", options, options_field, readonly_if, row_header }],
//   pin_columns: { field: "pin_list", states: "states", after: 2 },
//   row_actions: [{ label, method, range, at: "top"|"bottom" }] }
// Each edit calls write with the key fields plus { field: <column name or field>, value: <text> };
// a pin column writes field "pin:<pin>". A row action runs on every selected row, or once with
// { count } when `range` is set.
//
// Works like Excel: click selects, drag or Shift extends, typing replaces the cell, F2 / Enter /
// double click edit it, Delete clears, Ctrl+C / Ctrl+V exchange tab separated text with other
// programs, a single pasted value (or a typed pin state) fills the selection, the right button
// boxes a selection and opens the menu: pin states, a value for the whole box, row actions. Cells are plain text and there is one editor, so a long table stays light.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var sourceData: ({})
    property var rows: []
    property var rev: undefined
    property string message: ""
    property bool messageIsError: false
    property int curRow: -1
    property int curCol: -1
    property int ancRow: -1
    property int ancCol: -1
    property int editRow: -1                // the cell being edited
    property int editCol: -1
    property string editSeed: ""            // first character typed, replaces the cell
    readonly property int rowH: 30
    readonly property int headerH: 34
    readonly property var pins: spec.pin_columns ? (Fmt.field(sourceData, spec.pin_columns.field) || []) : []
    readonly property var actions: spec.row_actions || []
    readonly property var columns: {
        const base = (spec.columns || []).slice()
        if (!spec.pin_columns) return base
        const cols = pins.map((pin, k) => ({ pin: pin, pinIndex: k, label: pin, width: 38, editor: "state" }))
        const at = spec.pin_columns.after || 0
        return base.slice(0, at).concat(cols, base.slice(at))
    }
    readonly property real fixedWidth: columns.reduce((sum, c) => sum + (c.width || 0), 0)
    readonly property int flexCount: columns.filter(c => !c.width).length
    readonly property real flexWidth: flexCount ? Math.max(120, (list.width - fixedWidth) / flexCount) : 0
    readonly property real totalWidth: Math.max(list.width, fixedWidth + flexCount * flexWidth)
    readonly property int selTop: Math.min(curRow, ancRow)
    readonly property int selBottom: Math.max(curRow, ancRow)
    readonly property int selLeft: Math.min(curCol, ancCol)
    readonly property int selRight: Math.max(curCol, ancCol)
    readonly property bool multi: selTop !== selBottom || selLeft !== selRight
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
    function xOf(c) { let x = 0; for (let i = 0; i < c; ++i) x += widthOf(columns[i]); return x }
    function colAt(x) {
        let edge = 0
        for (let i = 0; i < columns.length; ++i) { edge += widthOf(columns[i]); if (x < edge) return i }
        return columns.length - 1
    }
    function valueOf(row, col) {
        if (col.pin !== undefined) return String(Fmt.field(row, spec.pin_columns.states) || "").charAt(col.pinIndex)
        const v = Fmt.field(row, col.field)
        return v === undefined || v === null ? "" : String(v)
    }
    function optionsOf(col) { return col.options_field ? (Fmt.field(sourceData, col.options_field) || []) : (col.options || []) }
    function editable(r, c) {
        const col = columns[c], row = rows[r]
        return !!col && !!row && !!col.editor && !(col.readonly_if && Fmt.field(row, col.readonly_if))
    }
    function selected(r, c) { return curRow >= 0 && r >= selTop && r <= selBottom && c >= selLeft && c <= selRight }

    function select(r, c, extend) {
        if (r < 0 || r >= rows.length || c < 0 || c >= columns.length) return
        curRow = r
        curCol = c
        if (!extend) { ancRow = r; ancCol = c }
        list.positionViewAtIndex(r, ListView.Contain)
        focusScope.forceActiveFocus()
    }
    function selectAll() {
        if (!rows.length) return
        ancRow = 0; ancCol = 0
        curRow = rows.length - 1; curCol = columns.length - 1
    }

    // ---- writing
    function say(text, isError) { message = text; messageIsError = !!isError }
    function refresh() {
        ctx.refresh(spec.source)
        for (const id of spec.also || []) ctx.refresh(id)
    }
    function failed(method, res) {
        const text = res && res.error ? res.error : "写入失败"
        say(text, true)
        ctx.deviceEvent({ type: "log", level: "error", message: method + "：" + text })
    }
    function keyArgs(row) {
        const args = {}
        for (const k of spec.key || []) args[k] = Fmt.field(row, k)
        return args
    }
    function cellArgs(r, c, value) {
        const args = keyArgs(rows[r]), col = columns[c]
        args.field = col.pin !== undefined ? "pin:" + col.pin : (col.name || col.field)
        args.value = value
        return args
    }
    // cells: [{ r, c, value }]
    function write(cells) {
        const todo = cells.filter(x => editable(x.r, x.c) && valueOf(rows[x.r], columns[x.c]) !== x.value)
        if (!todo.length) return
        const args = todo.map(x => cellArgs(x.r, x.c, x.value))
        say("")
        if (spec.write_many && args.length > 1) {
            ctx.device.call(spec.write_many, { cells: args }, (ok, res) => { if (!ok) failed(spec.write_many, res); refresh() })
            return
        }
        const next = i => {
            if (i >= args.length) { refresh(); return }
            ctx.device.call(spec.write, args[i], (ok, res) => {
                if (!ok) { failed(spec.write, res); refresh(); return }
                next(i + 1)
            })
        }
        next(0)
    }
    function fill(value, onlyPins) {
        const cells = []
        for (let r = selTop; r <= selBottom; ++r)
            for (let c = selLeft; c <= selRight; ++c)
                if (!onlyPins || columns[c].pin !== undefined) cells.push({ r: r, c: c, value: value })
        write(cells)
    }
    function clearSelection() {
        const cells = []
        for (let r = selTop; r <= selBottom; ++r)
            for (let c = selLeft; c <= selRight; ++c)
                cells.push({ r: r, c: c, value: columns[c].pin !== undefined ? "X" : "" })
        write(cells)
    }
    function runAction(a) {
        if (curRow < 0) return
        if (a.range) {
            const args = keyArgs(rows[a.at === "bottom" ? selBottom : selTop])
            args.count = selBottom - selTop + 1
            say("")
            ctx.device.call(a.method, args, (ok, res) => { if (!ok) failed(a.method, res); refresh() })
            return
        }
        const targets = []
        for (let r = selBottom; r >= selTop; --r) targets.push(keyArgs(rows[r]))
        const next = i => {
            if (i >= targets.length) { refresh(); return }
            ctx.device.call(a.method, targets[i], (ok, res) => {
                if (!ok) { failed(a.method, res); refresh(); return }
                next(i + 1)
            })
        }
        say("")
        next(0)
    }
    function undo() {
        if (!spec.undo) { say("这张表回车即写入文件，可用“还原工程文件”恢复"); return }
        ctx.device.call(spec.undo, {}, (ok, res) => { if (ok) say(""); else failed(spec.undo, res); refresh() })
    }

    // ---- clipboard (a hidden TextEdit reaches the system clipboard)
    TextEdit { id: clipboard; visible: false; textFormat: TextEdit.PlainText }
    function copySelection() {
        if (curRow < 0) return
        const lines = []
        for (let r = selTop; r <= selBottom; ++r) {
            const cells = []
            for (let c = selLeft; c <= selRight; ++c) cells.push(valueOf(rows[r], columns[c]))
            lines.push(cells.join("\t"))
        }
        clipboard.text = lines.join("\n")
        clipboard.selectAll()
        clipboard.copy()
        say("已复制 " + lines.length + " 行")
    }
    function paste() {
        if (curRow < 0) return
        clipboard.text = ""
        clipboard.paste()
        if (!clipboard.text) return
        const table = clipboard.text.replace(/\r/g, "").replace(/\n$/, "").split("\n").map(l => l.split("\t"))
        if (table.length === 1 && table[0].length === 1) { fill(table[0][0]); return }
        const cells = []
        let clipped = false
        for (let i = 0; i < table.length; ++i)
            for (let j = 0; j < table[i].length; ++j) {
                if (selTop + i >= rows.length || selLeft + j >= columns.length) { clipped = true; continue }
                cells.push({ r: selTop + i, c: selLeft + j, value: table[i][j] })
            }
        write(cells)
        if (clipped) say("超出表格范围的内容没有粘贴", true)
    }

    // ---- cell editor
    function startEdit(seed) {
        if (curRow < 0 || !editable(curRow, curCol)) return
        if (columns[curCol].editor === "state") return
        editSeed = seed || ""
        editRow = curRow
        editCol = curCol
    }
    function finishEdit(value, move) {
        const r = editRow, c = editCol
        editRow = -1
        editCol = -1
        if (value !== undefined) write([{ r: r, c: c, value: value }])
        focusScope.forceActiveFocus()
        if (move === "down") select(Math.min(r + 1, rows.length - 1), c, false)
        else if (move === "up") select(Math.max(r - 1, 0), c, false)
        else if (move === "right") select(r, Math.min(c + 1, columns.length - 1), false)
    }

    Row {
        visible: w.actions.length > 0 || !!w.spec.undo
        spacing: 4
        Repeater {
            model: w.actions
            delegate: DmButton {
                required property var modelData
                compact: true
                kind: "ghost"
                enabled: w.curRow >= 0
                text: modelData.label
                onClicked: w.runAction(modelData)
            }
        }
        DmButton {
            visible: !!w.spec.undo
            compact: true
            kind: "ghost"
            text: "撤销"
            onClicked: w.undo()
        }
    }

    FocusScope {
        id: focusScope
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(w.spec.height || 480, w.rowH * w.rows.length + w.headerH + 14)
        activeFocusOnTab: true

        Keys.onPressed: e => {
            if (w.curRow < 0) { if (w.rows.length) w.select(0, 0, false); e.accepted = true; return }
            const ctrl = e.modifiers & Qt.ControlModifier, shift = e.modifiers & Qt.ShiftModifier
            const col = w.columns[w.curCol]
            e.accepted = true
            if (ctrl && e.key === Qt.Key_C) w.copySelection()
            else if (ctrl && e.key === Qt.Key_V) w.paste()
            else if (ctrl && e.key === Qt.Key_Z) w.undo()
            else if (ctrl && e.key === Qt.Key_A) w.selectAll()
            else if (e.key === Qt.Key_Up) w.select(w.curRow - 1, w.curCol, shift)
            else if (e.key === Qt.Key_Down) w.select(w.curRow + 1, w.curCol, shift)
            else if (e.key === Qt.Key_Left) w.select(w.curRow, w.curCol - 1, shift)
            else if (e.key === Qt.Key_Right) w.select(w.curRow, w.curCol + 1, shift)
            else if (e.key === Qt.Key_Tab) w.select(w.curRow, Math.min(w.curCol + 1, w.columns.length - 1), false)
            else if (e.key === Qt.Key_Backtab) w.select(w.curRow, Math.max(w.curCol - 1, 0), false)
            else if (e.key === Qt.Key_PageDown) w.select(Math.min(w.curRow + 20, w.rows.length - 1), w.curCol, shift)
            else if (e.key === Qt.Key_PageUp) w.select(Math.max(w.curRow - 20, 0), w.curCol, shift)
            else if (e.key === Qt.Key_Delete || e.key === Qt.Key_Backspace) w.clearSelection()
            else if (e.key === Qt.Key_F2 || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                if (col.editor === "state") w.select(w.curRow + 1, w.curCol, false)
                else w.startEdit("")
            } else if (e.text.length === 1 && e.text >= " " && !ctrl) {
                const c = e.text.toUpperCase()
                if (col.editor === "state") {
                    if ("01LHXMVDE".indexOf(c) < 0) { w.say("引脚状态可用 0 1 L H X M V D E", true); return }
                    const single = !w.multi
                    w.fill(c, true)
                    if (single) w.select(w.curRow + 1, w.curCol, false)
                } else {
                    w.startEdit(e.text)
                }
            } else {
                e.accepted = false
            }
        }

        ListView {
            id: list
            anchors.fill: parent
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
                height: w.headerH
                color: Theme.surfaceAlt
                Row {
                    anchors.fill: parent
                    Repeater {
                        model: w.columns
                        delegate: Rectangle {
                            id: head
                            required property var modelData
                            required property int index
                            width: w.widthOf(modelData)
                            height: w.headerH
                            color: w.curRow >= 0 && index >= w.selLeft && index <= w.selRight ? Theme.accentSoft : "transparent"
                            Text {
                                anchors.fill: parent
                                leftPadding: head.modelData.editor === "state" ? 0 : 6
                                horizontalAlignment: head.modelData.editor === "state" ? Text.AlignHCenter : Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                text: head.modelData.label || head.modelData.field
                                color: Theme.textMuted
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSm
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderStrong }
            }

            delegate: Item {
                id: rowItem
                required property int index
                readonly property var row: w.rows[index] || ({})
                width: w.totalWidth
                height: w.rowH

                Row {
                    anchors.fill: parent
                    Repeater {
                        model: w.columns
                        delegate: Rectangle {
                            id: cell
                            required property var modelData
                            required property int index
                            readonly property var col: modelData
                            readonly property string value: w.valueOf(rowItem.row, col)
                            readonly property bool isPin: col.editor === "state"
                            width: w.widthOf(col)
                            height: w.rowH
                            color: w.selected(rowItem.index, index) ? Theme.accentSoft
                                 : col.row_header ? Theme.surfaceAlt : "transparent"
                            Text {
                                anchors.fill: parent
                                leftPadding: cell.isPin ? 0 : 6
                                rightPadding: cell.isPin ? 0 : 4
                                horizontalAlignment: cell.isPin ? Text.AlignHCenter : Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                text: cell.value
                                color: cell.isPin && cell.value === "X" ? Theme.textFaint
                                     : cell.isPin && (cell.value === "H" || cell.value === "L") ? Theme.toneColor("warning")
                                     : cell.isPin && cell.value === "1" ? Theme.accent
                                     : w.editable(rowItem.index, cell.index) ? Theme.text : Theme.textMuted
                                font.family: cell.isPin || cell.col.mono ? Theme.mono : Theme.sans
                                font.pixelSize: Theme.fontSm
                                font.weight: cell.isPin ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border; opacity: 0.6 }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border; opacity: 0.6 }
                            Rectangle {
                                visible: rowItem.index === w.curRow && cell.index === w.curCol
                                anchors.fill: parent
                                color: "transparent"
                                border.width: 2
                                border.color: Theme.accent
                            }
                        }
                    }
                }

                Loader {
                    active: rowItem.index === w.editRow && w.editCol >= 0
                    x: active ? w.xOf(w.editCol) : 0
                    z: 3
                    width: active ? w.widthOf(w.columns[w.editCol]) : 0
                    height: w.rowH
                    sourceComponent: active && w.columns[w.editCol].editor === "combo" ? comboEditor : textEditor
                }
                Component {
                    id: textEditor
                    Rectangle {
                        color: Theme.bg
                        border.width: 2
                        border.color: Theme.accent
                        TextInput {
                            id: input
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 4
                            verticalAlignment: TextInput.AlignVCenter
                            clip: true
                            selectByMouse: true
                            color: Theme.text
                            selectionColor: Theme.accentSoft
                            selectedTextColor: Theme.text
                            font.family: w.columns[w.editCol].mono ? Theme.mono : Theme.sans
                            font.pixelSize: Theme.fontSm
                            property bool closing: false
                            function close(value, move) { closing = true; w.finishEdit(value, move) }
                            Component.onCompleted: {
                                text = w.editSeed !== "" ? w.editSeed : w.valueOf(rowItem.row, w.columns[w.editCol])
                                forceActiveFocus()
                                if (w.editSeed === "") selectAll()
                            }
                            Keys.onPressed: e => {
                                if (e.key === Qt.Key_Escape) close(undefined, "")
                                else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) close(text, "down")
                                else if (e.key === Qt.Key_Tab) close(text, "right")
                                else if (e.key === Qt.Key_Up) close(text, "up")
                                else if (e.key === Qt.Key_Down) close(text, "down")
                                else return
                                e.accepted = true
                            }
                            onActiveFocusChanged: if (!activeFocus && !closing) close(text, "")
                        }
                    }
                }
                Component {
                    id: comboEditor
                    DmComboBox {
                        id: combo
                        font.pixelSize: Theme.fontSm
                        readonly property var options: w.optionsOf(w.columns[w.editCol])
                        model: options
                        currentIndex: options.indexOf(w.valueOf(rowItem.row, w.columns[w.editCol]))
                        Component.onCompleted: { forceActiveFocus(); popup.open() }
                        onActivated: index => w.finishEdit(options[index], "")
                        Connections {
                            target: combo.popup
                            function onClosed() { Qt.callLater(() => { if (w.editRow >= 0) w.finishEdit(undefined, "") }) }
                        }
                    }
                }
            }
        }

        // Above the list, leaving the scroll bars free; the wheel still reaches the list.
        MouseArea {
            id: mouse
            anchors.fill: parent
            anchors.rightMargin: 12
            anchors.bottomMargin: 12
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true
            property bool picking: false
            property int pressRow: -1               // right button: where the box started
            property int pressCol: -1
            property bool boxing: false
            function rowAt(y) { return list.indexAt(10, list.contentY + y) }
            function clampRow(y) {
                const r = rowAt(y)
                return r >= 0 ? r : y < w.headerH ? Math.max(0, w.curRow - 1) : w.rows.length - 1
            }
            onPressed: e => {
                focusScope.forceActiveFocus()
                const c = w.colAt(list.contentX + e.x)
                if (e.y < w.headerH) {                     // header: the whole column
                    if (!w.rows.length) return
                    w.ancRow = 0; w.ancCol = c
                    w.curRow = w.rows.length - 1; w.curCol = c
                    return
                }
                const r = rowAt(e.y)
                if (r < 0) return
                if (e.button === Qt.RightButton) {
                    if (!w.selected(r, c)) w.select(r, c, false)
                    pressRow = r
                    pressCol = c
                    boxing = false
                    return
                }
                if (w.columns[c].row_header) {              // row header: the whole row
                    w.ancRow = (e.modifiers & Qt.ShiftModifier) && w.curRow >= 0 ? w.ancRow : r
                    w.ancCol = 0
                    w.curRow = r
                    w.curCol = w.columns.length - 1
                    picking = true
                    return
                }
                w.select(r, c, e.modifiers & Qt.ShiftModifier)
                picking = true
            }
            onPositionChanged: e => {
                if (pressedButtons & Qt.RightButton) {
                    if (pressRow < 0) return
                    const r = clampRow(e.y), c = w.colAt(list.contentX + e.x)
                    if (!boxing && r === pressRow && c === pressCol) return
                    if (!boxing) { boxing = true; w.ancRow = pressRow; w.ancCol = pressCol }
                    w.curRow = r
                    w.curCol = c
                    list.positionViewAtIndex(r, ListView.Contain)
                    return
                }
                if (!picking || !pressed) return
                w.curRow = clampRow(e.y)
                if (!w.columns[w.ancCol].row_header) w.curCol = w.colAt(list.contentX + e.x)
                list.positionViewAtIndex(w.curRow, ListView.Contain)
            }
            onReleased: e => {
                picking = false
                if (e.button === Qt.RightButton && w.curRow >= 0) menu.popup()
                pressRow = -1
            }
            onDoubleClicked: e => {
                if (e.button !== Qt.LeftButton || e.y < w.headerH) return
                w.startEdit("")
            }
        }

        Popup {
            id: menu
            padding: 4
            property var choices: []
            property bool hasPins: false
            function popup() {
                const cols = w.columns.slice(w.selLeft, w.selRight + 1)
                hasPins = cols.some(c => c.pin !== undefined)
                choices = cols.length === 1 && cols[0].editor === "combo" ? w.optionsOf(cols[0]) : []
                fillField.text = ""
                x = Math.min(mouse.mouseX, focusScope.width - 260)
                y = Math.min(mouse.mouseY, Math.max(0, focusScope.height - 300))
                open()
                fillField.forceActiveFocus()
            }
            function fillWith(value) {
                close()
                focusScope.forceActiveFocus()
                w.fill(value)
            }
            background: Rectangle { color: Theme.surface; radius: Theme.radiusSm; border.color: Theme.border }
            contentItem: Column {
                spacing: 2
                Row {
                    visible: menu.hasPins
                    spacing: 2
                    Repeater {
                        model: ["0", "1", "L", "H", "X", "M", "V", "D", "E"]
                        delegate: DmButton {
                            required property string modelData
                            width: 28
                            compact: true
                            kind: "ghost"
                            text: modelData
                            onClicked: menu.fillWith(modelData)
                        }
                    }
                }
                Repeater {
                    model: menu.choices
                    delegate: DmButton {
                        required property string modelData
                        width: 240
                        compact: true
                        kind: "ghost"
                        text: modelData
                        onClicked: menu.fillWith(modelData)
                    }
                }
                Row {
                    spacing: 4
                    DmTextField {
                        id: fillField
                        width: 176
                        height: 30
                        font.pixelSize: Theme.fontSm
                        placeholderText: "批量改为…"
                        onAccepted: menu.fillWith(text)
                    }
                    DmButton {
                        width: 60
                        compact: true
                        text: "应用"
                        onClicked: menu.fillWith(fillField.text)
                    }
                }
                Repeater {
                    model: [{ label: "复制", run: () => w.copySelection() }, { label: "粘贴", run: () => w.paste() }]
                           .concat(w.spec.undo ? [{ label: "撤销", run: () => w.undo() }] : [])
                           .concat(w.actions.map(a => ({ label: a.label, run: () => w.runAction(a) })))
                    delegate: DmButton {
                        required property var modelData
                        width: 240
                        compact: true
                        kind: "ghost"
                        text: modelData.label
                        onClicked: { menu.close(); modelData.run() }
                    }
                }
            }
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
        visible: w.message !== ""
        Layout.fillWidth: true
        text: w.message
        wrapMode: Text.Wrap
        color: w.messageIsError ? Theme.error : Theme.textMuted
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
}
