import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window
import DeviceManager
import DeviceManager.Core

ApplicationWindow {
    id: window
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 640
    visible: false            // shown from C++ once the window frame is set up (WindowChrome)
    title: "DeviceManager — " + App.station.name
    color: Theme.bg

    // What the main area shows: { kind: "station" | "device" | "plugin", id: "" }
    property var view: ({ kind: "station", id: "" })

    // Optional "--view" from the command line: station | device:<name> | plugin:<id>
    property string startView: ""
    property string startConsole: ""     // "--console log|script"

    function show(kind, id) { view = { kind: kind, id: id || "" } }

    Component.onCompleted: {
        if (startView !== "") {
            const i = startView.indexOf(":")
            show(i < 0 ? startView : startView.substring(0, i), i < 0 ? "" : startView.substring(i + 1))
        } else if (App.station.defaultPage && App.station.device(App.station.defaultPage)) {
            show("device", App.station.defaultPage)
        }
    }

    Connections {
        target: App.station
        function onLoaded() {
            const d = App.station.defaultPage
            window.show(d && App.station.device(d) ? "device" : "station", d)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.preferredWidth: 256
            Layout.fillHeight: true
            current: window.view
            onNavigate: (kind, id) => window.show(kind, id)
        }

        Rectangle { width: 1; Layout.fillHeight: true; color: Theme.border }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Top bar: breadcrumb + global actions. With WindowChrome active it is also the
            // window's title bar: empty areas drag the window, the buttons stay clickable.
            Item {
                id: topBar
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                Component.onCompleted: WindowChrome.titleBarHeight = height
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 28
                    anchors.rightMargin: WindowChrome.active ? 0 : 16
                    spacing: 8
                    Text {
                        text: App.station.name
                        color: Theme.textMuted
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                    }
                    Text {
                        visible: window.view.kind !== "station"
                        text: "/"
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontMd
                    }
                    Text {
                        visible: window.view.kind !== "station"
                        text: window.view.kind === "plugin" ? "插件 · " + window.view.id
                            : window.view.kind === "data" ? "测试数据" : window.view.id
                        color: Theme.text
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                    }
                    Item { Layout.fillWidth: true }
                    DmButton {
                        kind: "ghost"
                        compact: true
                        text: "重新扫描插件"
                        onClicked: App.rescanPlugins()
                    }
                    DmButton {
                        kind: "ghost"
                        compact: true
                        text: App.darkTheme ? "☀  浅色" : "☾  深色"
                        onClicked: App.darkTheme = !App.darkTheme
                    }

                    // Window buttons (custom frame only)
                    Row {
                        visible: WindowChrome.active
                        Layout.fillHeight: true
                        Layout.leftMargin: 8
                        Repeater {
                            model: [
                                { glyph: "\uE921", role: "min" },
                                { glyph: window.visibility === Window.Maximized ? "\uE923" : "\uE922", role: "max" },
                                { glyph: "\uE8BB", role: "close" }
                            ]
                            delegate: Rectangle {
                                id: wbtn
                                required property var modelData
                                width: 46
                                height: topBar.height
                                color: !wbMouse.containsMouse ? "transparent"
                                     : modelData.role === "close" ? (wbMouse.pressed ? "#A93A2C" : "#C4503F")
                                     : (wbMouse.pressed ? Theme.selected : Theme.hover)
                                Text {
                                    anchors.centerIn: parent
                                    text: wbtn.modelData.glyph
                                    font.family: "Segoe MDL2 Assets"
                                    font.pixelSize: 10
                                    color: wbtn.modelData.role === "close" && wbMouse.containsMouse ? "#FFFFFF"
                                         : window.active ? Theme.text : Theme.textFaint
                                }
                                MouseArea {
                                    id: wbMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        if (wbtn.modelData.role === "min") window.showMinimized()
                                        else if (wbtn.modelData.role === "max")
                                            window.visibility === Window.Maximized ? window.showNormal() : window.showMaximized()
                                        else window.close()
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            // Main content
            Loader {
                id: content
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: window.view.kind === "device" ? deviceView
                               : window.view.kind === "plugin" ? pluginView
                               : window.view.kind === "data" ? dataView : stationView
            }

            ConsolePanel {
                Layout.fillWidth: true
                expanded: window.startConsole !== ""
                tab: window.startConsole === "script" ? 1 : 0
            }
        }
    }

    Component {
        id: stationView
        StationPage { onNavigate: (kind, id) => window.show(kind, id) }
    }
    Component {
        id: deviceView
        DevicePage { device: App.station.device(window.view.id) }
    }
    Component {
        id: dataView
        DataPage { Component.onCompleted: if (window.view.id) select(parseInt(window.view.id)) }
    }
    Component {
        id: pluginView
        PluginInfoPage {
            pluginId: window.view.id
            onNavigate: (kind, id) => window.show(kind, id)
        }
    }
}
