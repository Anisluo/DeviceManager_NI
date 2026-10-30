import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

Rectangle {
    id: sidebar
    property var current: ({ kind: "station", id: "" })
    signal navigate(string kind, string id)

    color: Theme.sidebar

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 0

        // Brand
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 6
            Layout.leftMargin: 10
            spacing: 10
            Image {                                       // the application icon (resources/make_icon.py)
                source: "qrc:/icons/app_64.png"
                sourceSize: Qt.size(28, 28)
                width: 28; height: 28
                smooth: true
                mipmap: true
            }
            ColumnLayout {
                spacing: 0
                Text {
                    text: "DeviceManager"
                    color: Theme.text
                    font.family: Theme.serif
                    font.pixelSize: Theme.fontLg + 1
                }
                Text {
                    text: "芯片测控平台 · v" + App.version
                    color: Theme.textFaint
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontXs
                }
            }
        }

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: list.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: list
                width: parent.width
                spacing: 2

                SectionLabel { text: "工位" }
                SidebarItem {
                    Layout.fillWidth: true
                    text: App.station.name
                    detail: App.station.count + " 台"
                    selected: sidebar.current.kind === "station"
                    onClicked: sidebar.navigate("station", "")
                }

                SidebarItem {
                    Layout.fillWidth: true
                    text: "测试数据"
                    detail: App.data.count > 0 ? App.data.count + " 次" : ""
                    selected: sidebar.current.kind === "data"
                    onClicked: sidebar.navigate("data", "")
                }

                SectionLabel { text: "设备" }
                Repeater {
                    model: App.station
                    delegate: SidebarItem {
                        required property var model
                        required property string name
                        required property string pluginId
                        Layout.fillWidth: true
                        text: name
                        detail: pluginId
                        status: model.status
                        selected: sidebar.current.kind === "device" && sidebar.current.id === name
                        onClicked: sidebar.navigate("device", name)
                    }
                }

                SectionLabel { text: "测试任务" }
                Repeater {
                    id: taskRepeater
                    model: App.plugins
                    delegate: SidebarItem {
                        required property string pluginId
                        required property string name
                        required property string category
                        visible: category === "task"
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        text: name
                        selected: sidebar.current.kind === "plugin" && sidebar.current.id === pluginId
                        onClicked: sidebar.navigate("plugin", pluginId)
                    }
                }
                Text {
                    visible: !hasTasks()
                    function hasTasks() {
                        for (let i = 0; i < taskRepeater.count; ++i)
                            if (taskRepeater.itemAt(i) && taskRepeater.itemAt(i).visible) return true
                        return false
                    }
                    text: "暂无测试任务插件"
                    color: Theme.textFaint
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    leftPadding: 10
                    topPadding: 4
                    bottomPadding: 4
                }

                SectionLabel { text: "插件" }
                Repeater {
                    model: App.plugins
                    delegate: SidebarItem {
                        required property string pluginId
                        required property string name
                        required property string version
                        required property string error
                        Layout.fillWidth: true
                        text: name
                        detail: version
                        status: error !== "" ? "error" : ""
                        selected: sidebar.current.kind === "plugin" && sidebar.current.id === pluginId
                        onClicked: sidebar.navigate("plugin", pluginId)
                    }
                }
            }
        }
    }
}
