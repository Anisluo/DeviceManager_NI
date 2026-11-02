import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import DeviceManager
import DeviceManager.Core
import "../js/Format.js" as Fmt

ColumnLayout {
    id: w
    property var spec: ({})
    property var ctx: null
    property var rev: undefined
    property var curves: []
    readonly property int visibleCurves: curves.filter(c => c.checked).length
    property string note: spec.hint || "等待分析…"
    spacing: 6
    function key(index, label) { return (ctx.device ? ctx.device.name : "") + "|" + spec.source + "|" + (spec.field || "") + "|" + index + "|" + label }
    function toggle(index, checked) {
        const copy = curves.slice()
        copy[index] = Object.assign({}, copy[index], { checked: checked })
        const saved = Object.assign({}, ctx.chartSelections || {})
        saved[key(index, copy[index].label)] = checked
        ctx.chartSelections = saved
        curves = copy
        plot.setSeriesVisible(index, checked)
    }
    function show(data) {
        const cap = spec.field ? Fmt.field(data, spec.field) : data
        if (cap && cap.rev !== undefined && cap.rev === rev) return
        rev = cap ? cap.rev : undefined
        const saved = ctx.chartSelections || {}
        curves = ((cap && cap.series) || []).map((s, i) => ({
            label: s.label || ("波形 " + (i + 1)),
            checked: saved[key(i, s.label || ("波形 " + (i + 1)))] !== false
        }))
        const shown = cap ? Object.assign({}, cap, { series: (cap.series || []).map((s, i) => Object.assign({}, s, { visible: curves[i].checked })) }) : {}
        plot.setCapture(shown)
        note = cap && cap.xs && cap.xs.length ? "QCustomPlot · 完整数据 " + cap.full_points + " 点 · 绘图 " + cap.shown_points + " 点（保留极值）" : (spec.hint || "等待分析…")
    }
    Flow {
        Layout.fillWidth: true
        spacing: 14
        Repeater {
            model: w.curves
            delegate: CheckBox {
                id: choice
                required property var modelData
                required property int index
                text: modelData.label
                checked: modelData.checked
                implicitHeight: 28
                leftPadding: 6
                rightPadding: 6
                topPadding: 4
                bottomPadding: 4
                spacing: 7
                hoverEnabled: true
                font.family: Theme.sans
                font.pixelSize: Theme.fontSm
                indicator: Rectangle {
                    implicitWidth: 15
                    implicitHeight: 15
                    x: choice.leftPadding
                    y: (choice.height - height) / 2
                    radius: 4
                    color: choice.checked ? Theme.accentSoft : Theme.surface
                    border.color: choice.checked ? Theme.accent : choice.hovered ? Theme.borderStrong : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: choice.checked
                        color: Theme.accent
                        font.family: Theme.sans
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                }
                contentItem: Text {
                    text: choice.text
                    leftPadding: choice.indicator.width + choice.spacing
                    font: choice.font
                    color: choice.checked ? Theme.text : Theme.textMuted
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 6
                    color: choice.hovered ? Theme.hover : "transparent"
                    border.color: choice.visualFocus ? Theme.borderStrong : "transparent"
                }
                onToggled: w.toggle(index, checked)
            }
        }
    }
    Text {
        visible: w.curves.length > 0 && w.visibleCurves === 0
        text: "未勾选任何波形"
        color: Theme.textMuted
        font.pixelSize: Theme.fontSm
    }
    Connections { target: w.ctx; function onSourceUpdated(id, data) { if (id === w.spec.source) w.show(data) } }
    RowLayout {
        Layout.fillWidth: true
        Text { text: w.note; color: Theme.textMuted; font.pixelSize: Theme.fontXs; Layout.fillWidth: true; elide: Text.ElideRight }
        DmButton { compact: true; text: "自适应"; onClicked: plot.fit() }
    }
    AnalysisPlot {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: w.spec.height || 340
        background: Theme.surface
        foreground: Theme.text
        gridColor: Theme.grid
        seriesColors: Theme.series
        ToolTip.visible: hover.hovered
        ToolTip.text: hoverText
        ToolTip.delay: 200
        HoverHandler { id: hover }
    }
    Text {
        text: "点击 X / Y 轴选中，再滚轮缩放该轴（当前：" + plot.selectedAxis + "）；再次点击取消 · 左键框选放大 · 右键拖动平移 · 双击自适应"
        color: Theme.textFaint; font.pixelSize: Theme.fontXs; wrapMode: Text.Wrap; Layout.fillWidth: true
    }
}
