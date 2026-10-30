import QtQuick
import QtQuick.Controls.Basic
import DeviceManager

ComboBox {
    id: control
    font.family: Theme.sans
    font.pixelSize: Theme.fontMd
    hoverEnabled: true

    contentItem: Text {
        leftPadding: 10
        rightPadding: control.indicator.width + 6
        text: control.displayText
        font: control.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 10
        y: (control.height - height) / 2
        text: "▾"
        color: Theme.textMuted
        font.pixelSize: Theme.fontMd
    }

    background: Rectangle {
        implicitHeight: 34
        implicitWidth: 140
        radius: Theme.radiusSm
        color: Theme.bg
        border.width: control.visualFocus || control.popup.visible ? 1.5 : 1
        border.color: control.popup.visible ? Theme.accent : control.hovered ? Theme.borderStrong : Theme.border
    }

    delegate: ItemDelegate {
        id: item
        required property int index
        required property var modelData
        width: control.width - 8
        x: 4
        hoverEnabled: true
        contentItem: Text {
            text: control.textRole ? item.modelData[control.textRole] : item.modelData
            font: control.font
            color: Theme.text
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6
            color: control.currentIndex === item.index ? Theme.accentSoft
                 : item.hovered ? Theme.hover : "transparent"
        }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            spacing: 2
        }
        background: Rectangle {
            color: Theme.surface
            radius: Theme.radiusSm
            border.color: Theme.border
        }
    }
}
