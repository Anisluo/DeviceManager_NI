import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQml.Models
import DeviceManager
import DeviceManager.Core
import "js/Format.js" as Fmt

// Renders a plugin's page.json for one device.
//
// The page object doubles as the context ("ctx") handed to every widget:
//   ctx.device, ctx.values[sourceId], ctx.act(action, value, cb), ctx.read(init, cb)
//   signals sourceUpdated(id, data), deviceEvent(evt), reinitRequested()
Item {
    id: ctx
    property Device device: null
    property var page: ({})
    property bool polling: true
    property var values: ({})
    property var form: ({})            // values of action-less inputs, referenced as "$form.<id>"
    property int tabIndex: 0
    readonly property var tabs: page.tabs || []
    // A page is either { sections: [...] } or { tabs: [{ title, sections }] }; sources are shared.
    readonly property var sections: tabs.length > 0 ? (tabs[Math.min(tabIndex, tabs.length - 1)].sections || []) : (page.sections || [])
    readonly property int cols: page.columns || 12
    readonly property bool narrow: width < 1000

    signal sourceUpdated(string id, var data)
    signal deviceEvent(var evt)
    signal reinitRequested()

    function load() {
        page = device ? App.page(device.pluginId) : ({ error: "设备不存在" })
        values = ({})
        if (tabIndex >= (page.tabs || []).length) tabIndex = 0
    }

    // Run a widget action: substitute "$value" into args and call the device.
    function act(action, value, cb) {
        if (!action || !action.method || !device) { if (cb) cb(false, null); return }
        const args = Fmt.substitute(action.args || {}, value, form)
        device.call(action.method, args, (ok, res) => {
            if (!ok) ctx.deviceEvent({ type: "log", level: "error",
                                       message: action.method + "：" + (res && res.error ? res.error : "调用失败") })
            if (cb) cb(ok, res)
        })
    }

    // Widgets of a newly shown tab missed the earlier source updates: replay the cached values
    // once they exist (slowly polled sources would otherwise stay empty until their next poll).
    onSectionsChanged: Qt.callLater(replaySources)
    function replaySources() {
        for (const id in values) sourceUpdated(id, values[id])
    }

    // Publish a result as if it came from a polled source (button "publish").
    function publish(id, data) {
        values[id] = data
        sourceUpdated(id, data)
    }

    // Poll one source now, e.g. after a write it depends on.
    function refresh(id) {
        const src = (page.sources || []).find(s => s.id === id)
        if (!src || !device) return
        device.call(src.method, Fmt.substitute(src.args || {}, undefined, form), (ok, res) => { if (ok) publish(id, res) })
    }

    // Read an initial value: { method, args, field }
    function read(init, cb) {
        if (!init || !init.method || !device || !device.online) return
        device.call(init.method, Fmt.substitute(init.args || {}, undefined, form), (ok, res) => { if (ok) cb(Fmt.field(res, init.field)) })
    }

    Component.onCompleted: load()

    objectName: "widget:tab"
    function apply(v) { tabIndex = Math.max(0, Math.min(tabs.length - 1, parseInt(v))) }
    onDeviceChanged: load()

    Connections {
        target: App
        function onPageChanged(id) { if (ctx.device && id === ctx.device.pluginId) ctx.load() }
    }
    Connections {
        target: ctx.device
        function onEvent(evt) { ctx.deviceEvent(evt) }
        function onReopened() { ctx.reinitRequested() }
    }

    // Polled data sources
    Instantiator {
        model: ctx.page.sources || []
        delegate: Timer {
            required property var modelData
            property bool busy: false
            interval: Math.max(20, modelData.interval_ms || 500)
            repeat: true
            triggeredOnStart: true
            running: ctx.polling && ctx.visible && ctx.device !== null && ctx.device.online
            onTriggered: {
                if (busy) return            // never queue polls behind a slow instrument
                busy = true
                const id = modelData.id
                ctx.device.call(modelData.method, Fmt.substitute(modelData.args || {}, undefined, ctx.form), (ok, res) => {
                    busy = false
                    if (!ok) return
                    ctx.values[id] = res
                    ctx.sourceUpdated(id, res)
                })
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 64
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: 32; y: 28
            width: ctx.width - 64
            spacing: 22

            // Header
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    spacing: 6
                    Text {
                        text: ctx.page.title || (ctx.device ? ctx.device.name : "")
                        color: Theme.text
                        font.family: Theme.serif
                        font.pixelSize: Theme.fontXxl + 4
                    }
                    RowLayout {
                        spacing: 8
                        StatusDot { status: ctx.device ? ctx.device.status : "offline" }
                        Text {
                            text: !ctx.device ? ""
                                : ctx.device.kind === "task" ? "测试任务" + (ctx.device.instrumentsText ? "  ·  使用 " + ctx.device.instrumentsText : "")
                                : ctx.device.name + "  ·  " + ctx.device.model + (ctx.device.resource ? " @ " + ctx.device.resource : "")
                                  + (ctx.device.pluginId ? "  ·  驱动 " + ctx.device.pluginId : "")
                            color: Theme.textMuted
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontMd
                        }
                        Text {
                            visible: !!ctx.page.subtitle
                            text: "  ·  " + (ctx.page.subtitle || "")
                            color: Theme.textMuted
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontMd
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                DmButton {
                    visible: (ctx.page.sources || []).length > 0
                    text: ctx.polling ? "暂停刷新" : "继续刷新"
                    onClicked: ctx.polling = !ctx.polling
                }
                DmButton {
                    text: "重新加载页面"
                    kind: "ghost"
                    onClicked: { ctx.load(); ctx.reinitRequested() }
                }
            }

            // Errors
            Rectangle {
                visible: (!!ctx.page.error && !(ctx.device && ctx.device.status === "nodriver"))
                         || (ctx.device && ctx.device.lastError !== "")
                Layout.fillWidth: true
                radius: Theme.radiusSm
                color: Theme.errorSoft
                implicitHeight: errText.implicitHeight + 20
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    Text {
                        id: errText
                        Layout.fillWidth: true
                        text: ctx.device && ctx.device.status === "nodriver" ? ctx.device.lastError + "。可以照常在 NI MAX 或其他软件里使用它；需要在这里控制时再为该型号添加驱动插件。"
                            : ctx.page.error || (ctx.device ? ctx.device.lastError : "")
                        color: ctx.device && ctx.device.status === "nodriver" ? Theme.textMuted : Theme.error
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                    }
                    DmButton {
                        visible: ctx.device && !ctx.device.online && ctx.device.status !== "nodriver"
                        text: "重新连接"
                        compact: true
                        onClicked: App.station.reopen(ctx.device.name)
                    }
                }
            }

            // Tabs
            Row {
                visible: ctx.tabs.length > 0
                spacing: 4
                Repeater {
                    model: ctx.tabs
                    delegate: DmButton {
                        required property var modelData
                        required property int index
                        compact: true
                        kind: ctx.tabIndex === index ? "secondary" : "ghost"
                        text: modelData.title || ("页 " + (index + 1))
                        onClicked: ctx.tabIndex = index
                    }
                }
            }

            // Sections on a 12-column grid
            GridLayout {
                id: grid
                Layout.fillWidth: true
                columns: ctx.cols
                columnSpacing: Theme.gap
                rowSpacing: Theme.gap
                // Derived from the fixed page width, not grid.width, to avoid a layout feedback loop.
                readonly property real unit: (column.width - (columns - 1) * columnSpacing) / columns

                Repeater {
                    model: ctx.sections
                    delegate: PageSection {
                        required property var modelData
                        readonly property int span: ctx.narrow ? ctx.cols : Math.min(ctx.cols, modelData.span || ctx.cols)
                        readonly property real exactWidth: grid.unit * span + grid.columnSpacing * (span - 1)
                        spec: modelData
                        pageCtx: ctx
                        Layout.columnSpan: span
                        Layout.rowSpan: ctx.narrow ? 1 : (modelData.row_span || 1)
                        // Pin widths explicitly: GridLayout does not split spanned columns evenly.
                        Layout.preferredWidth: exactWidth
                        Layout.minimumWidth: exactWidth
                        Layout.maximumWidth: exactWidth
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignTop
                    }
                }
            }
        }
    }
}
