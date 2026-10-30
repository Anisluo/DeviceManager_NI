import QtQuick
import DeviceManager

// online -> green, error -> red, busy -> terracotta, anything else -> grey
Rectangle {
    property string status: "offline"
    width: 8; height: 8; radius: 4
    color: status === "online" ? Theme.ok
         : status === "error" ? Theme.error
         : status === "busy" ? Theme.accent : Theme.textFaint
}
