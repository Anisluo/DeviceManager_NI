import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// Plugin details: the devices using it (open their pages), its methods with a call tester,
// the manifest, and the README folded away.
Flickable {
    id: page
    property string pluginId: ""
    signal navigate(string kind, string id)

    property var info: App.plugins.get(pluginId)
    property var devices: App.station.devicesForPlugin(pluginId)
    property var testDevice: devices.length > 0 ? App.station.device(devices[0]) : null
    property var methodList: testDevice && testDevice.online ? testDevice.methods() : []

    Connections {
        target: App
        function onPluginReloaded(id) { if (id === page.pluginId) page.refresh() }
    }
    function refresh() {
        info = App.plugins.get(pluginId)
        methodList = testDevice && testDevice.online ? testDevice.methods() : []
    }

    contentHeight: column.implicitHeight + 64
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    ColumnLayout {
        id: column
        x: 32; y: 32
        width: page.width - 64
        spacing: 20

        ColumnLayout {
            spacing: 6
            RowLayout {
                spacing: 12
                Text {
                    text: page.info.name || page.pluginId
                    color: Theme.text
                    font.family: Theme.serif
                    font.pixelSize: Theme.fontXxl + 4
                }
                Rectangle {
                    radius: 10
                    color: Theme.accentSoft
                    implicitWidth: ver.implicitWidth + 16
                    implicitHeight: 22
                    Text { id: ver; anchors.centerIn: parent; text: "v" + (page.info.version || "?") + " · " + (page.info.type || "cpp"); color: Theme.accent; font.family: Theme.sans; font.pixelSize: Theme.fontXs }
                }
            }
            Text {
                text: page.info.description || ""
                color: Theme.textMuted
                font.family: Theme.sans
                font.pixelSize: Theme.fontLg
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Rectangle {
                visible: (page.info.error || "") !== ""
                Layout.fillWidth: true
                Layout.topMargin: 6
                radius: Theme.radiusSm
                color: Theme.errorSoft
                implicitHeight: errText.implicitHeight + 16
                Text { id: errText; anchors.fill: parent; anchors.margins: 8; text: page.info.error || ""; color: Theme.error; wrapMode: Text.Wrap; font.family: Theme.sans; font.pixelSize: Theme.fontMd }
            }
        }

        // Devices first: editing, configuring and running happen on the device page.
        Card {
            title: "设备"
            subtitle: page.devices.length ? "编辑、配置和运行都在设备页面里" : ""
            Layout.fillWidth: true
            ColumnLayout {
                width: parent.width
                spacing: 8
                Text {
                    visible: page.devices.length === 0
                    text: "当前工位没有使用此插件的设备。在工位配置（station.json）的 devices 里加上它即可。"
                    color: Theme.textFaint; font.family: Theme.sans; font.pixelSize: Theme.fontMd
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Repeater {
                    model: page.devices
                    delegate: Rectangle {
                        id: devRow
                        required property string modelData
                        readonly property var dev: App.station.device(modelData)
                        Layout.fillWidth: true
                        implicitHeight: 56
                        radius: Theme.radiusSm
                        color: Theme.bg
                        border.color: Theme.border
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 10
                            spacing: 12
                            StatusDot { status: devRow.dev ? devRow.dev.status : "offline" }
                            ColumnLayout {
                                spacing: 1
                                Layout.fillWidth: true
                                Text { text: devRow.modelData; color: Theme.text; font.family: Theme.sans; font.pixelSize: Theme.fontLg }
                                Text {
                                    text: devRow.dev ? devRow.dev.resource + (devRow.dev.online ? " · 在线" : " · " + (devRow.dev.lastError || "离线")) : ""
                                    color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontXs
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            DmButton {
                                text: "打开设备页面 →"
                                kind: "primary"
                                onClicked: page.navigate("device", devRow.modelData)
                            }
                        }
                    }
                }
            }
        }

        // Methods the plugin exposes (read from a device using it), with a direct call tester.
        Card {
            title: "方法"
            subtitle: page.methodList.length ? page.methodList.length + " 个 · 点一行填入下方调用" : ""
            Layout.fillWidth: true
            ColumnLayout {
                width: parent.width
                spacing: 10
                Text {
                    visible: page.methodList.length === 0
                    text: page.testDevice ? "设备 " + page.testDevice.name + " 在线后列出插件方法" : "工位里有使用此插件的设备时列出插件方法"
                    color: Theme.textFaint; font.family: Theme.sans; font.pixelSize: Theme.fontMd
                }
                Repeater {
                    model: page.methodList
                    delegate: Rectangle {
                        id: mRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        implicitHeight: mLine.implicitHeight + 10
                        radius: 6
                        color: methodBox.currentIndex === index ? Theme.accentSoft : mHover.hovered ? Theme.surfaceAlt : "transparent"
                        HoverHandler { id: mHover }
                        RowLayout {
                            id: mLine
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 12
                            Text {
                                text: mRow.modelData.name + "(" + (mRow.modelData.args || []).join(", ") + ")"
                                color: Theme.text
                                font.family: Theme.mono
                                font.pixelSize: Theme.fontSm
                                Layout.preferredWidth: Math.min(implicitWidth, 360)
                                elide: Text.ElideRight
                            }
                            Text {
                                text: mRow.modelData.doc || ""
                                color: Theme.textMuted
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSm
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: methodBox.currentIndex = mRow.index
                        }
                    }
                }
                RowLayout {
                    visible: page.methodList.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8
                    DmComboBox {
                        id: methodBox
                        Layout.preferredWidth: 220
                        model: page.methodList.map(m => m.name)
                        onCurrentIndexChanged: {
                            const m = page.methodList[currentIndex]
                            if (!m) return
                            const args = {}
                            for (const a of m.args) args[a] = a === "channel" ? 0 : null
                            argsField.text = JSON.stringify(args)
                        }
                    }
                    DmTextField {
                        id: argsField
                        Layout.fillWidth: true
                        font.family: Theme.mono
                        placeholderText: "{\"channel\": 0}"
                        onAccepted: runButton.clicked()
                    }
                    DmButton {
                        id: runButton
                        text: "调用"
                        kind: "secondary"
                        onClicked: {
                            let args = {}
                            try { args = argsField.text ? JSON.parse(argsField.text) : {} }
                            catch (e) { resultText.text = "参数不是合法 JSON：" + e; return }
                            const t0 = Date.now()
                            page.testDevice.call(methodBox.currentText, args, (ok, res) => {
                                resultText.color = ok ? Theme.text : Theme.error
                                resultText.text = JSON.stringify(res, null, 2) + "\n\n(" + (Date.now() - t0) + " ms)"
                            })
                        }
                    }
                }
                Rectangle {
                    visible: resultText.text !== ""
                    Layout.fillWidth: true
                    implicitHeight: Math.max(60, resultText.implicitHeight + 20)
                    radius: Theme.radiusSm
                    color: Theme.bg
                    border.color: Theme.border
                    TextEdit {
                        id: resultText
                        anchors.fill: parent
                        anchors.margins: 10
                        readOnly: true
                        selectByMouse: true
                        color: Theme.text
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSm
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }
            }
        }

        Card {
            title: "清单"
            Layout.fillWidth: true
            GridLayout {
                width: parent.width
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                Repeater {
                    model: [
                        ["ID", page.info.id], ["类别", page.info.category], ["入口", page.info.entry],
                        ["目录", page.info.dir], ["库文件", page.info.library || "—"], ["页面", page.info.page || "—"]
                    ]
                    delegate: Item {
                        required property var modelData
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        implicitHeight: row.implicitHeight
                        RowLayout {
                            id: row
                            width: parent.width
                            Text { text: modelData[0]; Layout.preferredWidth: 64; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                            Text { text: modelData[1] || "—"; Layout.fillWidth: true; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fontSm; wrapMode: Text.WrapAnywhere }
                        }
                    }
                }
                Text { text: "能力"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm; Layout.alignment: Qt.AlignTop; Layout.preferredWidth: 64 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: page.info.capabilities || []
                        delegate: Rectangle {
                            required property string modelData
                            radius: 6
                            color: Theme.surfaceAlt
                            border.color: Theme.border
                            implicitWidth: cap.implicitWidth + 12
                            implicitHeight: 22
                            Text { id: cap; anchors.centerIn: parent; text: modelData; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fontXs }
                        }
                    }
                }
            }
        }

        // README stays available, folded away.
        Card {
            id: readmeCard
            property bool expanded: false
            title: "说明文档"
            subtitle: expanded ? "" : "README，开发和部署时参考"
            Layout.fillWidth: true
            visible: readme.text !== ""
            headerExtra: DmButton {
                compact: true
                kind: "ghost"
                text: readmeCard.expanded ? "收起" : "展开"
                onClicked: readmeCard.expanded = !readmeCard.expanded
            }
            Text {
                id: readme
                visible: readmeCard.expanded
                width: parent.width
                height: visible ? implicitHeight : 0
                text: App.plugins.readme(page.pluginId)
                textFormat: Text.MarkdownText
                wrapMode: Text.Wrap
                color: Theme.text
                linkColor: Theme.accent
                font.family: Theme.sans
                font.pixelSize: Theme.fontMd
            }
        }
    }
}
