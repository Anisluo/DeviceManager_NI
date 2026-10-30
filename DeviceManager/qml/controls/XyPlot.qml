import QtQuick
import DeviceManager
import DeviceManager.Core

// Static XY plot (lines + markers) with SI-formatted axes, hover readout and mouse zoom.
//   xs: [..]; series: [{ label, values: [..], xs?: [..] (own x values), unit, color }]
// Drawing and zoom live in C++ (PlotView); this file only binds the theme and the data.
PlotView {
    id: plot
    property var xs: []
    property var series: []

    function push() { setData(xs, series) }
    onXsChanged: Qt.callLater(push)          // callers usually set both; push once
    onSeriesChanged: Qt.callLater(push)
    Component.onCompleted: push()

    textColor: Theme.text
    mutedColor: Theme.textMuted
    faintColor: Theme.textFaint
    gridColor: Theme.grid
    axisColor: Theme.axis
    surfaceColor: Theme.surface
    borderColor: Theme.border
    cursorColor: Theme.borderStrong
    accentColor: Theme.accent
    seriesColors: Theme.series
    fontFamily: Theme.sans
    fontSize: Theme.fontXs

    HoverHandler { id: hover }

    // Small tools in the plot's bottom-right corner: measurement cursors and reset.
    Row {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 20
        anchors.bottomMargin: (plot.xLabel ? 40 : 26) + 6
        spacing: 4
        visible: plot.pointCount > 0 && (hover.hovered || plot.cursors || !plot.autoRange)
        Repeater {
            model: [
                { key: "cursors", text: "光标", on: plot.cursors, show: true },
                { key: "reset", text: "复位", on: false, show: !plot.autoRange }
            ]
            delegate: Rectangle {
                required property var modelData
                visible: modelData.show
                width: label.implicitWidth + 14
                height: 20
                radius: 10
                color: modelData.on ? Theme.accentSoft : Theme.surface
                border.color: modelData.on ? Theme.accent : Theme.border
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: parent.modelData.text
                    color: parent.modelData.on ? Theme.accent : Theme.textMuted
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontXs
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: parent.modelData.key === "cursors" ? plot.cursors = !plot.cursors : plot.resetView()
                }
            }
        }
    }
}
