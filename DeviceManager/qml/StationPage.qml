import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// Station overview: description + one card per device.
Flickable {
    id: page
    signal navigate(string kind, string id)
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
            Text {
                text: App.station.name
                color: Theme.text
                font.family: Theme.serif
                font.pixelSize: Theme.fontXxl + 4
            }
            Text {
                text: App.station.description
                visible: text !== ""
                color: Theme.textMuted
                font.family: Theme.sans
                font.pixelSize: Theme.fontLg
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Text {
                text: App.station.path
                color: Theme.textFaint
                font.family: Theme.mono
                font.pixelSize: Theme.fontXs
            }
        }

        RowLayout {
            spacing: 8
            Text {
                text: "切换工位"
                color: Theme.textMuted
                font.family: Theme.sans
                font.pixelSize: Theme.fontMd
            }
            DmComboBox {
                id: stationBox
                Layout.preferredWidth: 360
                model: App.availableStations()
                displayText: currentText.replace(/^.*[\\/]/, "")
                Component.onCompleted: currentIndex = Math.max(0, model.indexOf(App.station.path))
            }
            DmButton {
                text: "加载"
                kind: "primary"
                onClicked: App.loadStation(stationBox.currentText)
            }
        }

        Text {
            text: "设备"
            color: Theme.text
            font.family: Theme.serif
            font.pixelSize: Theme.fontXl
            Layout.topMargin: 8
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.gap
            Repeater {
                model: App.station
                delegate: Card {
                    id: devCard
                    required property var model
                    required property var device
                    required property string name
                    required property string pluginId
                    required property string resource
                    required property string kind
                    visible: kind === "instrument"
                    width: visible ? 300 : 0
                    height: visible ? implicitHeight : 0
                    title: name
                    subtitle: (devCard.model.instrumentModel || "") + (resource ? " · " + resource : "") + (pluginId ? "  ·  驱动 " + pluginId : "")
                    ColumnLayout {
                        width: parent.width
                        spacing: 10
                        RowLayout {
                            spacing: 8
                            StatusDot { status: devCard.model.status || "" }
                            Text {
                                text: devCard.model.status === "online" ? "在线" : devCard.model.status === "error" ? "错误"
                                    : devCard.model.status === "nodriver" ? "无驱动" : "离线"
                                color: Theme.textMuted
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSm
                            }
                        }
                        Text {
                            visible: devCard.device && devCard.device.lastError !== ""
                            text: devCard.device ? devCard.device.lastError : ""
                            color: devCard.model.status === "nodriver" ? Theme.textFaint : Theme.error
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSm
                        }
                        RowLayout {
                            spacing: 8
                            DmButton { text: "打开页面"; kind: "primary"; compact: true; visible: devCard.pluginId !== ""; onClicked: page.navigate("device", devCard.name) }
                            DmButton { text: "重新连接"; compact: true; visible: devCard.pluginId !== ""; onClicked: App.station.reopen(devCard.name) }
                        }
                    }
                }
            }
        }
    }
}
