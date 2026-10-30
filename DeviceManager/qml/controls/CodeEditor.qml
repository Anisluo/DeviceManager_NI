import QtQuick
import QtQuick.Controls.Basic
import DeviceManager
import DeviceManager.Core

// Python code editor: line numbers, syntax colours, 4-space indent, auto-indent.
// Emits runRequested() on Ctrl+Enter / F5 and saveRequested() on Ctrl+S.
Rectangle {
    id: root
    property alias text: edit.text
    property alias readOnly: edit.readOnly
    property bool modified: false
    signal runRequested()
    signal saveRequested()

    function setText(t) { edit.text = t; modified = false; flick.contentY = 0 }

    color: Theme.bg
    radius: Theme.radiusSm
    border.color: edit.activeFocus ? Theme.accent : Theme.border
    clip: true

    readonly property real lineHeight: edit.contentHeight / Math.max(1, edit.lineCount)

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 1
        contentWidth: gutter.width + edit.contentWidth + 24
        contentHeight: edit.contentHeight + 16
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        function ensureVisible(r) {
            if (contentX >= r.x + gutter.width) contentX = r.x
            else if (contentX + width <= r.x + gutter.width + r.width + 20) contentX = r.x + gutter.width + r.width + 20 - width
            if (contentY >= r.y) contentY = r.y
            else if (contentY + height <= r.y + r.height + 16) contentY = r.y + r.height + 16 - height
        }

        // Line numbers
        Rectangle {
            id: gutter
            width: Math.max(36, numbers.implicitWidth + 18)
            height: Math.max(flick.height, flick.contentHeight)
            color: Theme.surfaceAlt
            Text {
                id: numbers
                x: 8
                y: 8
                width: parent.width - 14
                horizontalAlignment: Text.AlignRight
                text: Array.from({ length: edit.lineCount }, (_, i) => i + 1).join("\n")
                font: edit.font
                color: Theme.textFaint
                lineHeightMode: Text.FixedHeight
                lineHeight: root.lineHeight
            }
        }

        TextEdit {
            id: edit
            x: gutter.width + 10
            y: 8
            width: Math.max(flick.width - gutter.width - 20, contentWidth)
            font.family: Theme.mono
            font.pixelSize: Theme.fontMd
            color: Theme.text
            selectionColor: Theme.accentSoft
            selectedTextColor: Theme.text
            selectByMouse: true
            persistentSelection: true
            wrapMode: TextEdit.NoWrap
            tabStopDistance: 4 * fontMetrics.advanceWidth(" ")
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
            onTextChanged: root.modified = true

            Keys.onPressed: event => {
                const ctrl = event.modifiers & Qt.ControlModifier
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && ctrl) {
                    root.runRequested(); event.accepted = true
                } else if (event.key === Qt.Key_F5) {
                    root.runRequested(); event.accepted = true
                } else if (event.key === Qt.Key_S && ctrl) {
                    root.saveRequested(); event.accepted = true
                } else if (event.key === Qt.Key_Tab) {
                    edit.insert(edit.cursorPosition, "    "); event.accepted = true
                } else if (event.key === Qt.Key_Backtab) {
                    const start = edit.text.lastIndexOf("\n", edit.cursorPosition - 1) + 1
                    const lead = edit.text.substr(start, 4).match(/^ {1,4}/)
                    if (lead) edit.remove(start, start + lead[0].length)
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    // Auto-indent: keep the current indent, add one level after ':'
                    const pos = edit.cursorPosition
                    const start = edit.text.lastIndexOf("\n", pos - 1) + 1
                    const line = edit.text.substring(start, pos)
                    let indent = line.match(/^\s*/)[0]
                    if (/:\s*(#.*)?$/.test(line)) indent += "    "
                    edit.insert(pos, "\n" + indent)
                    event.accepted = true
                }
            }

            FontMetrics { id: fontMetrics; font: edit.font }
        }

        MouseArea {   // clicking empty space below the text focuses the editor
            x: edit.x
            y: edit.y + edit.contentHeight
            width: flick.contentWidth
            height: Math.max(0, flick.height - edit.contentHeight)
            cursorShape: Qt.IBeamCursor
            onClicked: { edit.forceActiveFocus(); edit.cursorPosition = edit.length }
        }
    }

    SyntaxHighlighter {
        document: edit.textDocument
        keyword: Theme.dark ? "#E48A6C" : "#B5532F"
        string: Theme.dark ? "#A8C08A" : "#5E7F3E"
        comment: Theme.textFaint
        number: Theme.dark ? "#D9B865" : "#9A6B12"
        builtin: Theme.dark ? "#8FB0D6" : "#3F6593"
    }
}
