import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "js/Format.js" as Fmt

// Stored runs: list on the left; plot, table and exports of the selected run on the right.
Item {
    id: page
    property var runId: 0             // selected run (0 = newest)
    property var run: ({})
    property var cols: []
    property var rows: []
    property string exported: ""      // last exported file
    property bool confirmDelete: false
    readonly property var store: App.data
    readonly property bool isShmoo: run.kind === "shmoo" && run.meta && run.meta.x && run.meta.y
    readonly property var axisNames: (run.meta && run.meta.axes ? run.meta.axes.map(a => a.name) : [])

    function unitOf(name) {
        const a = (run.meta && run.meta.axes || []).find(a => a.name === name)
        return a ? (a.unit || "") : ""
    }

    function select(id) {
        runId = id
        confirmDelete = false
        load(true)
    }
    function load(resetSelectors) {
        if (!runId && store.count > 0) runId = store.data(store.index(0, 0), 257)   // IdRole = Qt::UserRole + 1
        run = runId ? store.run(runId) : ({})
        cols = run.columns || []
        rows = runId ? store.rows(runId, 0, 500) : []
        if (resetSelectors) {
            const measured = cols.filter(c => axisNames.indexOf(c) < 0 && c !== "pass")
            const x = axisNames.length ? axisNames[axisNames.length - 1] : cols[0]
            xBox.currentIndex = Math.max(0, cols.indexOf(x))
            yBox.currentIndex = Math.max(0, cols.indexOf(measured[0] !== undefined ? measured[0] : cols[1]))
            const g = axisNames.length > 1 ? axisNames[axisNames.length - 2] : ""
            groupBox.currentIndex = g ? cols.indexOf(g) + 1 : 0
        }
        refreshPlot()
    }
    function refreshPlot() {
        if (!runId || cols.length === 0) { plot.xs = []; plot.series = []; return }
        if (isShmoo) {
            shmoo.xs = store.column(runId, run.meta.x)
            shmoo.ys = store.column(runId, run.meta.y)
            shmoo.values = store.column(runId, run.meta.value || "pass")
            return
        }
        const x = xBox.currentText, y = yBox.currentText
        const xs = store.column(runId, x), ys = store.column(runId, y)
        const g = groupBox.currentIndex > 0 ? groupBox.currentText : ""
        const series = []
        if (g) {
            const gv = store.column(runId, g)
            const keys = []
            for (const v of gv) if (keys.indexOf(v) < 0) keys.push(v)
            for (const key of keys.slice(0, 16)) {
                const sx = [], sy = []
                for (let k = 0; k < xs.length; ++k) if (gv[k] === key) { sx.push(xs[k]); sy.push(ys[k]) }
                series.push({ label: g + "=" + (typeof key === "number" ? Fmt.si(key, unitOf(g), 3) : key), xs: sx, values: sy, unit: unitOf(y) })
            }
        } else {
            series.push({ label: y, values: ys, unit: unitOf(y) })
        }
        plot.xLabel = x
        plot.xUnit = unitOf(x)
        plot.xs = xs
        plot.series = series
    }

    Component.onCompleted: load(true)
    Connections {
        target: page.store
        function onRunChanged(id) { if (id === page.runId) page.load(false) }
        function onCountChanged() { if (!page.runId) page.load(true) }
    }
    Connections {
        target: App.script
        function onFileExported(path) { page.exported = path }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: Theme.gap

        // ---------------- Run list ----------------
        Card {
            Layout.preferredWidth: 330
            Layout.fillHeight: true
            title: "测试数据"
            subtitle: page.store.count + " 次运行 · " + page.store.path.replace(/^.*[\\/]/, "")
            ListView {
                id: runList
                width: parent.width
                height: parent.height
                clip: true
                model: page.store
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: runItem
                    required property var runId
                    required property string name
                    required property string kind
                    required property string started
                    required property string status
                    required property int points
                    width: runList.width - 8
                    height: 52
                    radius: Theme.radiusSm
                    color: page.runId === runId ? Theme.selected : itemMouse.containsMouse ? Theme.hover : "transparent"
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.topMargin: 7
                        anchors.bottomMargin: 7
                        spacing: 2
                        RowLayout {
                            spacing: 6
                            StatusDot {
                                status: runItem.status === "ok" ? "online" : runItem.status === "running" ? "busy"
                                      : runItem.status === "stopped" ? "offline" : "error"
                            }
                            Text {
                                text: runItem.name
                                color: Theme.text
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontMd
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                radius: 6
                                color: runItem.kind === "shmoo" ? Theme.accentSoft : Theme.surfaceAlt
                                implicitWidth: kindText.implicitWidth + 10
                                implicitHeight: 18
                                Text {
                                    id: kindText
                                    anchors.centerIn: parent
                                    text: runItem.kind
                                    color: runItem.kind === "shmoo" ? Theme.accent : Theme.textMuted
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.fontXs
                                }
                            }
                        }
                        Text {
                            text: "#" + runItem.runId + " · " + runItem.started.replace("T", " ").substring(0, 19) + " · " + runItem.points + " 点"
                            color: Theme.textFaint
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontXs
                        }
                    }
                    MouseArea {
                        id: itemMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.select(runItem.runId)
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: page.store.count === 0
                    text: "还没有数据。运行一个扫描或在脚本里调用 dm.record()。"
                    width: parent.width - 20
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textFaint
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                }
            }
        }

        // ---------------- Detail ----------------
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: detail.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            visible: !!page.run.id

            ColumnLayout {
                id: detail
                width: parent.width
                spacing: Theme.gap

                ColumnLayout {
                    spacing: 4
                    Text {
                        text: page.run.name || ""
                        color: Theme.text
                        font.family: Theme.serif
                        font.pixelSize: Theme.fontXxl
                    }
                    Text {
                        text: "#" + page.run.id + " · " + page.run.kind + " · " + String(page.run.started || "").replace("T", " ").substring(0, 19)
                              + (page.run.duration !== undefined && page.run.duration !== null ? " · " + Number(page.run.duration).toFixed(1) + " s" : "")
                              + " · " + page.run.points + " 点 · " + page.run.status
                              + (page.run.station ? " · " + page.run.station : "")
                        color: Theme.textMuted
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontMd
                    }
                    Text {
                        visible: !!page.run.script
                        text: page.run.script || ""
                        color: Theme.textFaint
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontXs
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    DmButton { compact: true; text: "导出 CSV"; onClicked: page.exported = page.store.exportCsv(page.runId) }
                    DmButton { compact: true; text: "报告 HTML"; enabled: App.script.available && !App.script.running; onClicked: App.exportRun(page.runId, "html") }
                    DmButton { compact: true; text: "报告 PDF"; enabled: App.script.available && !App.script.running; onClicked: App.exportRun(page.runId, "pdf") }
                    DmButton { compact: true; text: "Parquet"; enabled: App.script.available && !App.script.running; onClicked: App.exportRun(page.runId, "parquet") }
                    DmButton { compact: true; kind: "ghost"; text: "打开导出目录"; onClicked: Qt.openUrlExternally(App.fileUrl(page.store.exportDir)) }
                    DmButton {
                        compact: true
                        kind: "ghost"
                        text: page.confirmDelete ? "确认删除？" : "删除"
                        enabled: page.run.status !== "running"
                        onClicked: {
                            if (!page.confirmDelete) { page.confirmDelete = true; return }
                            page.store.deleteRun(page.runId)
                            page.runId = 0
                            page.load(true)
                        }
                    }
                }

                Rectangle {
                    visible: page.exported !== ""
                    Layout.fillWidth: true
                    radius: Theme.radiusSm
                    color: Theme.okSoft
                    implicitHeight: 38
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 6
                        Text {
                            Layout.fillWidth: true
                            text: "已导出 " + page.exported
                            color: Theme.ok
                            elide: Text.ElideMiddle
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSm
                        }
                        DmButton { compact: true; kind: "ghost"; text: "打开"; onClicked: Qt.openUrlExternally(App.fileUrl(page.exported)) }
                    }
                }

                Card {
                    Layout.fillWidth: true
                    title: page.isShmoo ? "Shmoo" : "曲线"
                    ColumnLayout {
                        width: parent.width
                        spacing: 8
                        RowLayout {
                            visible: !page.isShmoo
                            spacing: 6
                            Text { text: "X"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                            DmComboBox { id: xBox; Layout.preferredWidth: 140; model: page.cols; onActivated: page.refreshPlot() }
                            Text { text: "Y"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                            DmComboBox { id: yBox; Layout.preferredWidth: 140; model: page.cols; onActivated: page.refreshPlot() }
                            Text { text: "分组"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                            DmComboBox { id: groupBox; Layout.preferredWidth: 140; model: ["无"].concat(page.cols); onActivated: page.refreshPlot() }
                        }
                        XyPlot {
                            id: plot
                            visible: !page.isShmoo
                            Layout.fillWidth: true
                            Layout.preferredHeight: 320
                        }
                        ShmooMap {
                            id: shmoo
                            visible: page.isShmoo
                            Layout.fillWidth: true
                            Layout.preferredHeight: 340
                            xLabel: page.isShmoo ? page.run.meta.x : ""
                            yLabel: page.isShmoo ? page.run.meta.y : ""
                            xUnit: page.isShmoo ? page.unitOf(page.run.meta.x) : ""
                            yUnit: page.isShmoo ? page.unitOf(page.run.meta.y) : ""
                        }
                    }
                }

                Card {
                    Layout.fillWidth: true
                    title: "数据"
                    subtitle: page.rows.length < (page.run.points || 0) ? "显示前 " + page.rows.length + " 行，完整数据请导出" : ""
                    ColumnLayout {
                        width: parent.width
                        spacing: 0
                        readonly property real colWidth: Math.max(90, width / Math.max(1, page.cols.length))
                        Row {
                            Repeater {
                                model: page.cols
                                delegate: Text {
                                    required property string modelData
                                    width: parent.parent.colWidth
                                    text: modelData + (page.unitOf(modelData) ? " [" + page.unitOf(modelData) + "]" : "")
                                    color: Theme.textMuted
                                    font.family: Theme.sans; font.pixelSize: Theme.fontSm; font.weight: Font.DemiBold
                                    bottomPadding: 6
                                }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                        ListView {
                            id: table
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(360, page.rows.length * 22 + 4)
                            clip: true
                            model: page.rows
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Row {
                                id: tableRow
                                required property var modelData
                                Repeater {
                                    model: tableRow.modelData
                                    delegate: Text {
                                        required property var modelData
                                        width: table.parent.colWidth
                                        height: 22
                                        verticalAlignment: Text.AlignVCenter
                                        text: modelData === true ? "✓" : modelData === false ? "✕"
                                            : typeof modelData === "number" ? String(Number(modelData.toPrecision(7))) : String(modelData)
                                        color: modelData === false ? Theme.error : modelData === true ? Theme.ok : Theme.text
                                        font.family: Theme.mono; font.pixelSize: Theme.fontSm
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // CLI: --action data=<runId>|csv|html|pdf
    objectName: "widget:data"
    function apply(v) {
        v = String(v)
        if (v === "csv") exported = store.exportCsv(runId)
        else if (v === "html" || v === "pdf" || v === "parquet") App.exportRun(runId, v)
        else select(parseInt(v))
    }
}
