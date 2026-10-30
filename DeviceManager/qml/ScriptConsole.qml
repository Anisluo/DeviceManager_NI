import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core

// Sequence editor (left) + output / interactive prompt / data (right).
Item {
    id: root
    readonly property var engine: App.script
    property string currentFile: ""
    property var history: []
    property int historyPos: -1
    property int rightTab: 0          // 0 output, 1 data

    function fileName(p) { return p.replace(/^.*[\\/]/, "") }
    function open(path) {
        currentFile = path
        editor.setText(engine.readText(path))
    }
    function save() {
        if (currentFile === "") { saveAs.open(); return }
        if (engine.writeText(currentFile, editor.text)) editor.modified = false
    }
    function runEditor() {
        if (engine.running) return
        if (currentFile !== "" && editor.modified) save()
        rightTab = 0
        engine.run(editor.text, currentFile || "<editor>")
    }
    function appendOutput(text, stream) {
        const parts = text.split("\n")
        // Continue the last line if it did not end with a newline.
        if (outModel.count > 0 && !outModel.get(outModel.count - 1).closed && outModel.get(outModel.count - 1).stream === stream) {
            const last = outModel.get(outModel.count - 1)
            outModel.setProperty(outModel.count - 1, "text", last.text + parts[0])
            parts.shift()
            if (parts.length > 0) outModel.setProperty(outModel.count - 1, "closed", true)
        } else if (outModel.count > 0) {
            outModel.setProperty(outModel.count - 1, "closed", true)
        }
        for (let i = 0; i < parts.length; ++i) {
            const isLast = i === parts.length - 1
            if (isLast && parts[i] === "") break
            outModel.append({ text: parts[i], stream: stream, closed: !isLast })
        }
        if (outModel.count > 3000) outModel.remove(0, outModel.count - 3000)
        outView.positionViewAtEnd()
    }

    Component.onCompleted: {
        const list = engine.sequences()
        if (list.length > 0) open(list[0])
        if (!engine.available)
            appendOutput("Python 不可用：脚本和 Python 插件已禁用，详见日志。\n", "stderr")
        else
            appendOutput("Python " + engine.pythonVersion + " · 输入表达式回车执行；dm.devices() 列出设备\n", "info")
    }

    ListModel { id: outModel }

    // Addressable as "widget:console": --action console=data|output|stop|exec:<python>
    objectName: "widget:console"
    function apply(v) {
        v = String(v)
        if (v === "stop") engine.stop()
        else if (v.startsWith("exec:")) engine.exec(v.substring(5))
        else rightTab = (v === "data" || v === "1") ? 1 : 0
    }

    Connections {
        target: root.engine
        function onRunningChanged() {
            // Show the file that is actually running (e.g. started from a page or the CLI).
            const p = root.engine.currentPath
            if (root.engine.running && p !== "" && p !== root.currentFile && !editor.modified) root.open(p)
        }
        function onOutput(text, stream) {
            root.appendOutput(stream === "input" ? ">>> " + text.replace(/\n/g, "\n... ") + "\n" : text, stream)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 12
        anchors.bottomMargin: 10
        spacing: 12

        // ---------------- Editor ----------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 55
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                DmComboBox {
                    id: fileBox
                    Layout.preferredWidth: 260
                    model: root.engine.sequences()
                    displayText: (root.currentFile ? root.fileName(root.currentFile) : "未保存的脚本") + (editor.modified ? " •" : "")
                    delegate: ItemDelegate {
                        required property string modelData
                        required property int index
                        width: fileBox.width - 8
                        x: 4
                        contentItem: Text { text: root.fileName(modelData); color: Theme.text; font.family: Theme.sans; font.pixelSize: Theme.fontMd }
                        background: Rectangle { radius: 6; color: hovered ? Theme.hover : "transparent" }
                        onClicked: { fileBox.popup.close(); root.open(modelData) }
                    }
                }
                DmButton { compact: true; kind: "ghost"; text: "保存"; enabled: editor.modified; onClicked: root.save() }
                DmButton { compact: true; kind: "ghost"; text: "另存为"; onClicked: saveAs.open() }
                Item { Layout.fillWidth: true }
                Text {
                    visible: root.engine.running
                    text: root.engine.currentName + (root.engine.progressText ? " · " + root.engine.progressText : "")
                    color: Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSm
                    elide: Text.ElideRight
                    Layout.maximumWidth: 260
                }
                DmButton {
                    compact: true
                    kind: root.engine.running ? "secondary" : "primary"
                    text: root.engine.running ? "■ 停止" : "▶ 运行"
                    enabled: root.engine.available
                    onClicked: root.engine.running ? root.engine.stop() : root.runEditor()
                }
            }

            // Progress
            Rectangle {
                Layout.fillWidth: true
                height: 3
                radius: 1.5
                color: Theme.border
                visible: root.engine.running || root.engine.progress > 0
                Rectangle {
                    width: parent.width * root.engine.progress
                    height: parent.height
                    radius: 1.5
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
            }

            CodeEditor {
                id: editor
                Layout.fillWidth: true
                Layout.fillHeight: true
                onRunRequested: root.runEditor()
                onSaveRequested: root.save()
            }
        }

        Rectangle { Layout.fillHeight: true; width: 1; color: Theme.border }

        // ---------------- Output / data ----------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 45
            spacing: 8

            RowLayout {
                spacing: 4
                Repeater {
                    model: ["输出", "数据" + (root.engine.recordCount > 0 ? " · " + root.engine.recordCount : "")]
                    delegate: DmButton {
                        required property int index
                        required property string modelData
                        compact: true
                        kind: root.rightTab === index ? "secondary" : "ghost"
                        text: modelData
                        onClicked: root.rightTab = index
                    }
                }
                Item { Layout.fillWidth: true }
                DmButton {
                    compact: true; kind: "ghost"; text: "清空"
                    onClicked: root.rightTab === 0 ? outModel.clear() : root.engine.clearRecords()
                }
            }

            // Output + prompt
            ColumnLayout {
                visible: root.rightTab === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.radiusSm
                    color: Theme.bg
                    border.color: Theme.border
                    ListView {
                        id: outView
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        model: outModel
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        delegate: TextEdit {
                            required property var model
                            readonly property string stream: model.stream
                            width: outView.width - 12
                            text: model.text
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            textFormat: TextEdit.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSm
                            color: stream === "stderr" ? Theme.error
                                 : stream === "info" ? Theme.accent
                                 : stream === "input" ? Theme.textMuted : Theme.text
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text { text: ">>>"; color: Theme.accent; font.family: Theme.mono; font.pixelSize: Theme.fontMd }
                    DmTextField {
                        id: prompt
                        Layout.fillWidth: true
                        font.family: Theme.mono
                        placeholderText: root.engine.running ? "脚本运行中…" : "dm.device(\"smu1\").measure(channel=0)"
                        enabled: root.engine.available
                        Keys.onReturnPressed: event => {
                            if (text.trim() === "" || root.engine.running) return
                            root.history.push(text)
                            root.historyPos = root.history.length
                            root.engine.exec(text)
                            text = ""
                        }
                        Keys.onUpPressed: {
                            if (root.history.length === 0) return
                            root.historyPos = Math.max(0, root.historyPos - 1)
                            text = root.history[root.historyPos]
                        }
                        Keys.onDownPressed: {
                            root.historyPos = Math.min(root.history.length, root.historyPos + 1)
                            text = root.historyPos < root.history.length ? root.history[root.historyPos] : ""
                        }
                    }
                }
            }

            // Data: plot + table
            ColumnLayout {
                id: dataView
                visible: root.rightTab === 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6
                readonly property var cols: root.engine.columns

                function refresh() {
                    if (cols.length < 2) { plot.xs = []; plot.series = []; return }
                    const x = xBox.currentText || cols[0]
                    const y = yBox.currentText || cols[1]
                    plot.xLabel = x
                    plot.xs = root.engine.column(x)
                    plot.series = [{ label: y, values: root.engine.column(y) }]
                }
                Timer { id: refreshTimer; interval: 150; onTriggered: dataView.refresh() }
                Connections {
                    target: root.engine
                    function onRecordsChanged() { refreshTimer.restart() }
                }
                onVisibleChanged: if (visible) refresh()

                RowLayout {
                    spacing: 6
                    Text { text: "X"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                    DmComboBox {
                        id: xBox
                        Layout.preferredWidth: 130
                        model: dataView.cols
                        onActivated: dataView.refresh()
                    }
                    Text { text: "Y"; color: Theme.textMuted; font.family: Theme.sans; font.pixelSize: Theme.fontSm }
                    DmComboBox {
                        id: yBox
                        Layout.preferredWidth: 130
                        model: dataView.cols
                        currentIndex: dataView.cols.length > 1 ? 1 : 0
                        onActivated: dataView.refresh()
                    }
                    Item { Layout.fillWidth: true }
                }
                XyPlot {
                    id: plot
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120
                }
                // Table
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(160, 30 + root.engine.recordCount * 22)
                    radius: Theme.radiusSm
                    color: Theme.bg
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 0
                        Row {
                                Layout.bottomMargin: 4
                            Repeater {
                                model: dataView.cols
                                delegate: Text {
                                    required property string modelData
                                    width: Math.max(90, (dataView.width - 20) / Math.max(1, dataView.cols.length))
                                    text: modelData
                                    color: Theme.textMuted
                                    font.family: Theme.sans; font.pixelSize: Theme.fontSm; font.weight: Font.DemiBold
                                }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                        ListView {
                            id: table
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: root.engine.recordCount
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            onCountChanged: positionViewAtEnd()
                            delegate: Row {
                                required property int index
                                readonly property var cells: root.engine.record(index)
                                Repeater {
                                    model: parent.cells
                                    delegate: Text {
                                        required property var modelData
                                        width: Math.max(90, (dataView.width - 20) / Math.max(1, dataView.cols.length))
                                        height: 20
                                        text: typeof modelData === "number" ? String(Number(modelData.toPrecision(7))) : String(modelData)
                                        color: Theme.text
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

    Popup {
        id: saveAs
        anchors.centerIn: parent
        padding: 16
        modal: true
        background: Rectangle { color: Theme.surface; radius: Theme.radius; border.color: Theme.border }
        onOpened: { nameField.text = root.currentFile ? root.fileName(root.currentFile) : "new_sequence.py"; nameField.forceActiveFocus(); nameField.selectAll() }
        ColumnLayout {
            spacing: 10
            Text { text: "另存为（保存在 sequences 目录）"; color: Theme.text; font.family: Theme.serif; font.pixelSize: Theme.fontLg }
            DmTextField {
                id: nameField
                Layout.preferredWidth: 320
                font.family: Theme.mono
                onAccepted: saveButton.clicked()
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                DmButton { text: "取消"; kind: "ghost"; onClicked: saveAs.close() }
                DmButton {
                    id: saveButton
                    text: "保存"; kind: "primary"
                    onClicked: {
                        let n = nameField.text.trim()
                        if (n === "") return
                        if (!n.endsWith(".py")) n += ".py"
                        const path = root.engine.resolvePath(n)
                        if (root.engine.writeText(path, editor.text)) {
                            root.currentFile = path
                            editor.modified = false
                            fileBox.model = root.engine.sequences()
                        }
                        saveAs.close()
                    }
                }
            }
        }
    }
}
