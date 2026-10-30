import QtQuick
import QtQuick.Layouts
import DeviceManager

// One section card of a page: { title, subtitle, span, row_span, layout: "column"|"row", widgets: [...] }
Card {
    id: section
    property var spec: ({})
    property var pageCtx: null

    title: spec.title || ""
    subtitle: spec.subtitle || ""

    GridLayout {
        width: parent.width
        columns: section.spec.layout === "row" ? Math.max(1, Math.min((section.spec.widgets || []).length,
                                                                     Math.floor(width / 170)))
                                               : 1
        columnSpacing: 12
        rowSpacing: 12

        Repeater {
            model: section.spec.widgets || []
            delegate: WidgetHost {
                required property var modelData
                spec: modelData
                pageCtx: section.pageCtx
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
        }
    }
}
