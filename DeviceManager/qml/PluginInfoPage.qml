import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// Plugin details: manifest, README, devices using it and a raw method tester.
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

        GridLayout {
            Layout.fillWidth: true
            columns: width > 900 ? 2 : 1
            columnSpacing: Theme.gap
            rowSpacing: Theme.gap

            Card {
                title: "清单"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
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

            Card {
                title: "使用此插件的设备"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                ColumnLayout {
                    width: parent.width
                    spacing: 6
                    Text {
                        visible: page.devices.length === 0
                        text: "当前工位没有使用此插件的设备"
                        color: Theme.textFaint; font.family: Theme.sans; font.pixelSize: Theme.fontMd
                    }
                    Repeater {
                        model: page.devices
                        delegate: SidebarItem {
                            required property string modelData
                            Layout.fillWidth: true
                            text: modelData
                            status: App.station.device(modelData) ? App.station.device(modelData).status : ""
                            detail: "打开页面 →"
                            onClicked: page.navigate("device", modelData)
                        }
                    }
                }
            }
        }

        Card {
            visible: page.testDevice !== null
            title: "方法调试"
            subtitle: page.testDevice ? "在设备 " + page.testDevice.name + " 上直接调用插件方法" : ""
            Layout.fillWidth: true
            ColumnLayout {
                width: parent.width
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
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
                        kind: "primary"
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
                Text {
                    text: page.methodList[methodBox.currentIndex] ? page.methodList[methodBox.currentIndex].doc : ""
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                }
                Rectangle {
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
            title: "README"
            Layout.fillWidth: true
            visible: readme.text !== ""
            Text {
                id: readme
                width: parent.width
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
