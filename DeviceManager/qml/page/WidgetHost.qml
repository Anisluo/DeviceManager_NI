import QtQuick
import DeviceManager

// Maps a widget spec's "type" to its implementation.
Loader {
    id: host
    property var spec: ({})
    property var pageCtx: null
    // Widgets with an "id" are addressable as "widget:<id>"; every widget exposes apply(value).
    objectName: spec.id ? "widget:" + spec.id : ""
    function apply(value) { if (item && item.apply) item.apply(value) }

    readonly property var registry: ({
        "number":  numberComp,
        "combo":   comboComp,
        "switch":  switchComp,
        "button":  buttonComp,
        "readout": readoutComp,
        "trend":   trendComp,
        "table":   tableComp,
        "edit_table": editTableComp,
        "log":     logComp,
        "script":  scriptComp,
        "xy":      xyComp,
        "shmoo":   shmooComp,
        "sweep":   sweepComp,
        "text":    textComp,
        "scope":   scopeComp,
        "params":  paramsComp
    })

    sourceComponent: registry[spec.type] || placeholderComp

    Component { id: numberComp;  NumberWidget  { spec: host.spec; ctx: host.pageCtx } }
    Component { id: comboComp;   ComboWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: switchComp;  SwitchWidget  { spec: host.spec; ctx: host.pageCtx } }
    Component { id: buttonComp;  ButtonWidget  { spec: host.spec; ctx: host.pageCtx } }
    Component { id: readoutComp; ReadoutWidget { spec: host.spec; ctx: host.pageCtx } }
    Component { id: trendComp;   TrendWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: tableComp;   TableWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: editTableComp; EditTableWidget { spec: host.spec; ctx: host.pageCtx } }
    Component { id: logComp;     LogWidget     { spec: host.spec; ctx: host.pageCtx } }
    Component { id: scriptComp;  ScriptWidget  { spec: host.spec; ctx: host.pageCtx } }
    Component { id: xyComp;      XyWidget      { spec: host.spec; ctx: host.pageCtx } }
    Component { id: shmooComp;   ShmooWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: sweepComp;   SweepWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: textComp;    TextWidget    { spec: host.spec; ctx: host.pageCtx } }
    Component { id: scopeComp;   ScopeWidget   { spec: host.spec; ctx: host.pageCtx } }
    Component { id: paramsComp;  ParamsWidget  { spec: host.spec; ctx: host.pageCtx } }
    Component { id: placeholderComp; PlaceholderWidget { spec: host.spec } }
}
