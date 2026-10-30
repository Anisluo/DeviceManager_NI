import QtQuick
import QtQuick.Layouts
import DeviceManager
import "../js/Format.js" as Fmt

// { type: "tree", source, select, add, remove, noun, also: ["vec"] }
// A project and its items, from a source { root, current, items: [{ name, modified }] }. Clicking
// an item calls select { name }; "添加" asks for a name in place and calls add { name }; Delete or
// "删除" asks first, then calls remove { name }. also: sources to refresh after any of these.
ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property string root: ""
    property string current: ""
    property var items: []
    property bool open: true
    property bool adding: false
    property string removing: ""            // the item waiting for a confirmation
    property string error: ""
    readonly property string noun: spec.noun || "项"
    spacing: 4

    Connections {
        target: w.ctx
        function onSourceUpdated(id, data) {
            if (id !== w.spec.source || !data) return
            w.root = data.root || ""
            const moved = (data.current || "") !== w.current
            w.current = data.current || ""
            w.items = data.items || []
            if (moved) Qt.callLater(() => list.positionViewAtIndex(w.items.findIndex(x => x.name === w.current), ListView.Contain))
        }
    }

    function refresh() {
        ctx.refresh(spec.source)
        for (const id of spec.also || []) ctx.refresh(id)
    }
    function call(method, args, done) {
        error = ""
        ctx.device.call(method, args, (ok, res) => {
            if (!ok) error = res && res.error ? res.error : "操作失败"
            refresh()
            if (done) done(ok)
        })
    }
    function select(name) {
        removing = ""
        if (name !== current) call(spec.select, { name: name })
        keys.forceActiveFocus()
    }
    function step(delta) {
        const i = items.findIndex(x => x.name === current) + delta
        if (i >= 0 && i < items.length) select(items[i].name)
    }

    Item {
        id: keys
        Layout.preferredHeight: 0
        focus: true
        Keys.onUpPressed: w.step(-1)
        Keys.onDownPressed: w.step(1)
        Keys.onDeletePressed: w.removing = w.current
        Keys.onEscapePressed: w.removing = ""
    }

    Rectangle {
        Layout.fillWidth: true
        height: 30
        radius: 4
        color: rootArea.containsMouse ? Theme.hover : "transparent"
        Text {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            verticalAlignment: Text.AlignVCenter
            text: (w.open ? "▾  " : "▸  ") + w.root
            color: Theme.text
            font.family: Theme.sans
            font.pixelSize: Theme.fontSm
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        MouseArea {
            id: rootArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: w.open = !w.open
        }
    }

    ListView {
        id: list
        visible: w.open
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(w.spec.height || 560, contentHeight)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: w.items
        spacing: 1
        delegate: Rectangle {
            id: node
            required property var modelData
            readonly property bool active: modelData.name === w.current
            width: list.width
            height: 30
            radius: 4
            color: active ? Theme.accentSoft : nodeArea.containsMouse ? Theme.hover : "transparent"
            Text {
                anchors.fill: parent
                anchors.leftMargin: 26
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: node.modelData.name + (node.modelData.modified ? " *" : "")
                color: Theme.text
                font.family: Theme.mono
                font.pixelSize: Theme.fontSm
                elide: Text.ElideRight
            }
            MouseArea {
                id: nodeArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: w.select(node.modelData.name)
            }
        }
    }

    DmTextField {
        id: input
        visible: w.adding
        Layout.fillWidth: true
        font.pixelSize: Theme.fontSm
        placeholderText: "新" + w.noun + "名称，回车创建"
        onAccepted: w.call(w.spec.add, { name: text }, ok => {
            if (ok) { w.adding = false; text = ""; keys.forceActiveFocus() }
        })
        Keys.onEscapePressed: { w.adding = false; text = ""; keys.forceActiveFocus() }
    }

    Text {
        visible: w.removing !== ""
        Layout.fillWidth: true
        text: "删除" + w.noun + " " + w.removing + "？"
        wrapMode: Text.Wrap
        color: Theme.text
        font.family: Theme.sans
        font.pixelSize: Theme.fontSm
    }
    RowLayout {
        Layout.topMargin: 6
        spacing: 4
        DmButton {
            visible: w.removing === ""
            compact: true
            text: "添加"
            onClicked: { w.adding = true; input.forceActiveFocus() }
        }
        DmButton {
            visible: w.removing === ""
            compact: true
            kind: "ghost"
            text: "删除"
            enabled: w.current !== ""
            onClicked: w.removing = w.current
        }
        DmButton {
            visible: w.removing !== ""
            compact: true
            kind: "primary"
            text: "确定删除"
            onClicked: {
                const name = w.removing
                w.removing = ""
                w.call(w.spec.remove, { name: name })
                keys.forceActiveFocus()
            }
        }
        DmButton {
            visible: w.removing !== ""
            compact: true
            kind: "ghost"
            text: "取消"
            onClicked: w.removing = ""
        }
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
