.pragma library

// SI-prefixed engineering formatting: 0.0012 A -> "1.2000 mA".
var PREFIXES = [
    { e: -15, p: "f" }, { e: -12, p: "p" }, { e: -9, p: "n" }, { e: -6, p: "µ" },
    { e: -3, p: "m" },  { e: 0, p: "" },    { e: 3, p: "k" },  { e: 6, p: "M" }, { e: 9, p: "G" }
]

function siParts(value) {
    if (value === 0 || !isFinite(value)) return { scaled: value, prefix: "" }
    var exp = Math.floor(Math.log10(Math.abs(value)) / 3) * 3
    exp = Math.max(-15, Math.min(9, exp))
    for (var i = 0; i < PREFIXES.length; ++i)
        if (PREFIXES[i].e === exp) return { scaled: value / Math.pow(10, exp), prefix: PREFIXES[i].p }
    return { scaled: value, prefix: "" }
}

// digits = significant digits
function si(value, unit, digits) {
    if (value === undefined || value === null || (typeof value === "number" && isNaN(value))) return "—"
    if (typeof value !== "number") return String(value)
    if (!isFinite(value)) return value > 0 ? "∞" : "−∞"
    if (!unit && Number.isInteger(value)) return String(value)    // counts, indices
    digits = digits || 5
    var parts = siParts(value)
    var intDigits = Math.abs(parts.scaled) >= 100 ? 3 : Math.abs(parts.scaled) >= 10 ? 2 : 1
    var decimals = Math.max(0, digits - intDigits)
    return parts.scaled.toFixed(decimals) + " " + parts.prefix + (unit || "")
}

// Compact engineering notation for editable fields: 2000000 -> "2 M", 0.05 -> "50 m" (round-trips through parseNumber).
function eng(value, digits) {
    if (typeof value !== "number" || !isFinite(value)) return String(value)
    var parts = siParts(value)
    var n = Number(parts.scaled.toPrecision(digits || 6))
    return String(n) + (parts.prefix ? " " + parts.prefix : "")
}

// Split form for big readouts: { number: "1.2000", unit: "mA" }
function siSplit(value, unit, digits) {
    var s = si(value, unit, digits)
    var i = s.indexOf(" ")
    return i < 0 ? { number: s, unit: "" } : { number: s.substring(0, i), unit: s.substring(i + 1) }
}

// Axis tick label: compact, shares one prefix across the axis.
function tick(value, step, unit) {
    var ref = Math.max(Math.abs(value), Math.abs(step))
    var parts = siParts(ref)
    var scale = parts.scaled !== 0 ? ref / parts.scaled : 1
    var v = value / scale
    var stepScaled = Math.abs(step / scale)
    var decimals = stepScaled >= 1 ? 0 : Math.min(9, Math.ceil(-Math.log10(stepScaled) - 1e-9))
    return v.toFixed(decimals) + (unit !== undefined ? " " + parts.prefix + unit : "")
}

// "Nice" axis ticks covering [min, max] with about `count` intervals.
function niceTicks(min, max, count) {
    if (!isFinite(min) || !isFinite(max)) return { min: 0, max: 1, step: 0.25 }
    if (min === max) {
        var pad = Math.abs(min) > 0 ? Math.abs(min) * 1e-3 : 1e-9
        min -= pad; max += pad
    }
    var range = max - min
    var rough = range / (count || 5)
    var mag = Math.pow(10, Math.floor(Math.log10(rough)))
    var norm = rough / mag
    var step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag
    return { min: Math.floor(min / step) * step, max: Math.ceil(max / step) * step, step: step }
}

// Parse user input with optional SI suffix: "10m" -> 0.01, "2.5k" -> 2500, "1u"/"1µ" -> 1e-6.
var SUFFIX = { f: 1e-15, p: 1e-12, n: 1e-9, u: 1e-6, "µ": 1e-6, m: 1e-3, k: 1e3, K: 1e3, M: 1e6, G: 1e9 }
function parseNumber(text) {
    var s = String(text).trim().replace(/\s+/g, "")
    if (/^0x[0-9a-f_]+$/i.test(s)) return parseInt(s.replace(/_/g, ""), 16)
    var m = /^([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)([fpnuµmkKMG]?)[A-Za-zΩ]*$/.exec(s)
    if (!m) return NaN
    return parseFloat(m[1]) * (m[2] ? SUFFIX[m[2]] : 1)
}

// Deep copy of a JSON-like value with "$value" and "$form.<id>" placeholders substituted.
function substitute(obj, value, form) {
    if (obj === "$value") return value
    if (typeof obj === "string" && obj.indexOf("$form.") === 0) {
        var key = obj.substring(6)
        return form && key in form ? form[key] : null
    }
    if (Array.isArray(obj)) return obj.map(function (x) { return substitute(x, value, form) })
    if (obj !== null && typeof obj === "object") {
        var out = {}
        for (var k in obj) out[k] = substitute(obj[k], value, form)
        return out
    }
    return obj
}

function hex(v, digits) {
    if (typeof v !== "number" || !isFinite(v)) return v === undefined || v === null ? "—" : String(v)
    var s = (v >>> 0).toString(16).toUpperCase()
    while (s.length < (digits || 8)) s = "0" + s
    return "0x" + s
}

// Read "a.b.0.c" from a result object.
function field(obj, path) {
    if (path === undefined || path === null || path === "") return obj
    var parts = String(path).split(".")
    var cur = obj
    for (var i = 0; i < parts.length; ++i) {
        if (cur === undefined || cur === null) return undefined
        cur = cur[parts[i]]
    }
    return cur
}

// Evaluate a simple arithmetic expression over the fields of `data`
// (e.g. "voltage * current"). Only identifiers, numbers and + - * / ( ) are allowed.
function evalExpr(expr, data) {
    if (!/^[\w\s.+\-*/()]+$/.test(expr)) return NaN
    var names = Object.keys(data || {}).filter(function (k) { return /^[A-Za-z_]\w*$/.test(k) })
    var args = names.map(function (k) { return data[k] })
    try {
        return Function.apply(null, names.concat(["return (" + expr + ");"])).apply(null, args)
    } catch (e) {
        return NaN
    }
}
