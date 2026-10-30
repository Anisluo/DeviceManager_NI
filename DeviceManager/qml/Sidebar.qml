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
                    detail: App.station.instrumentCount + " 台"
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

                // Instruments detected in the chassis (or the simulated one); drivers stay hidden.
                SectionLabel { text: "设备" }
                Repeater {
                    model: App.station
                    delegate: SidebarItem {
                        required property var model
                        required property string name
                        required property string kind
                        visible: kind === "instrument"
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        text: name
                        detail: model.instrumentModel || ""
                        status: model.status || ""
                        dimmed: model.status === "nodriver"
                        selected: sidebar.current.kind === "device" && sidebar.current.id === name
                        onClicked: sidebar.navigate("device", name)
                    }
                }
                Text {
                    visible: App.station.instrumentCount === 0
                    text: App.station.discoverNote || "没有检测到仪器"
                    color: Theme.textFaint
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    leftPadding: 10
                    topPadding: 4
                    bottomPadding: 4
                }

                // Plugins are the test tasks (one DLL each); a task opens its own page.
                SectionLabel { text: "插件" }
                Repeater {
                    id: taskRepeater
                    model: App.plugins
                    delegate: SidebarItem {
                        required property string pluginId
                        required property string name
                        required property string version
                        required property string category
                        required property string error
                        readonly property var dev: App.station.count >= 0 ? App.station.device(pluginId) : null
                        visible: category === "task"
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        text: name
                        detail: version
                        status: error !== "" ? "error" : dev ? dev.status : ""
                        selected: sidebar.current.kind === "device" && sidebar.current.id === pluginId
                        onClicked: sidebar.navigate(dev ? "device" : "plugin", pluginId)
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
            }
        }
    }
}
