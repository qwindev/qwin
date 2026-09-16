import QtQuick
import Qwin
import Qwin.Ui

// Weather readout: temperature and condition glyph in the bar, place name,
// details and a 3-day forecast in the popup. The only plugin here that
// talks to the network - Open-Meteo, no API key, over a plain
// XMLHttpRequest (QML's XHR has no CORS restrictions).
//
//   "weather": { "latitude": 52.2297, "longitude": 21.0122, "name": "Warsaw",
//                "units": "metric", "refreshMinutes": 15 }
//
// All keys optional. Following the colors.json/config.json house rule, a
// failed or malformed response keeps the last good data on screen and only
// changes the muted footer to an error line; before the first success there
// is no last-good, so the bar shows a placeholder dash rather than jump. A
// failure retries after ~60s instead of waiting out refreshMinutes.
Rectangle {
    id: weatherItem

    readonly property var cfg: Plugins.config("weather")
    readonly property real latitude: cfg.latitude !== undefined ? cfg.latitude : 52.2297
    readonly property real longitude: cfg.longitude !== undefined ? cfg.longitude : 21.0122
    readonly property string placeName: cfg.name !== undefined ? cfg.name : "Warsaw"
    readonly property string units: cfg.units === "imperial" ? "imperial" : "metric"
    // A zero or negative value would make the refresh timer a busy loop.
    readonly property int refreshMinutes: (cfg.refreshMinutes !== undefined && cfg.refreshMinutes > 0)
                                           ? cfg.refreshMinutes : 15
    readonly property string windUnit: units === "imperial" ? "mph" : "km/h"

    // Last good fetch; haveData gates every read that would otherwise go
    // through a null `current`.
    property var current: null           // {temp, feelsLike, humidity, windSpeed, code, isDay}
    property var daily: []               // [{date, code, tMax, tMin}, ...], today first
    property bool haveData: false
    property bool lastFetchFailed: false
    property string lastError: ""
    property var lastUpdated: null       // Date of the last successful fetch

    // A constant 4 characters - the widest real reading in either unit - so
    // a digit boundary or a minus sign does not reflow the forecast column.
    // Padding wider just opens a gap around the "/" separator.
    function tempText(v) {
        return (Math.round(v) + "\u00B0").padStart(4)
    }

    // Human label for a WMO weather_code, the codes Open-Meteo returns.
    function labelFor(code) {
        if (code === 0)
            return "Clear"
        if (code >= 1 && code <= 2)
            return "Partly cloudy"
        if (code === 3)
            return "Overcast"
        if (code === 45 || code === 48)
            return "Fog"
        if (code >= 51 && code <= 57)
            return "Drizzle"
        if (code >= 61 && code <= 67)
            return "Rain"
        if (code >= 71 && code <= 77)
            return "Snow"
        if (code >= 80 && code <= 82)
            return "Rain showers"
        if (code === 85 || code === 86)
            return "Snow showers"
        if (code >= 95 && code <= 99)
            return "Thunderstorm"
        return "Unknown"
    }

    // daily.time entries are date-only ISO strings, which `new Date(str)`
    // reads as UTC midnight - rendered back in local time that shifts the
    // weekday a day in negative-offset zones. Build from local components.
    function dateFromISO(str) {
        const p = str.split("-")
        return new Date(parseInt(p[0]), parseInt(p[1]) - 1, parseInt(p[2]))
    }

    function requestUrl() {
        let url = "https://api.open-meteo.com/v1/forecast"
                + "?latitude=" + weatherItem.latitude
                + "&longitude=" + weatherItem.longitude
                + "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
                + "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                + "&timezone=auto&forecast_days=4"
        if (weatherItem.units === "imperial")
            url += "&temperature_unit=fahrenheit&wind_speed_unit=mph"
        return url
    }

    function applyData(data) {
        const c = data.current
        weatherItem.current = {
            temp: c.temperature_2m,
            feelsLike: c.apparent_temperature,
            humidity: c.relative_humidity_2m,
            windSpeed: c.wind_speed_10m,
            code: c.weather_code,
            isDay: c.is_day === 1
        }
        const d = data.daily
        const times = d.time || []
        const codes = d.weather_code || []
        const maxes = d.temperature_2m_max || []
        const mins = d.temperature_2m_min || []
        const out = []
        for (let i = 0; i < times.length; i++)
            out.push({ date: times[i], code: codes[i], tMax: maxes[i], tMin: mins[i] })
        weatherItem.daily = out
        weatherItem.haveData = true
    }

    function fetchWeather() {
        const xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            if (xhr.status === 200) {
                try {
                    weatherItem.applyData(JSON.parse(xhr.responseText))
                    weatherItem.lastFetchFailed = false
                    weatherItem.lastError = ""
                    weatherItem.lastUpdated = new Date()
                    retryTimer.stop()
                    refreshTimer.restart()
                } catch (e) {
                    weatherItem.fetchFailed("bad response")
                }
            } else {
                weatherItem.fetchFailed("HTTP " + xhr.status)
            }
        }
        xhr.open("GET", weatherItem.requestUrl())
        xhr.send()
    }

    function fetchFailed(reason) {
        weatherItem.lastFetchFailed = true
        weatherItem.lastError = reason
        // Keep the last-good data and retry sooner, pushing the normal tick
        // out so the two do not race.
        retryTimer.restart()
        refreshTimer.restart()
    }

    width: weatherRow.implicitWidth + 14
    height: 24
    radius: 5
    color: weatherMouse.containsMouse || weatherMenu.opened ? Qt.alpha(Colors.surface, 0.13)
                                                             : "transparent"

    // Weather glyph drawn from a WMO weather_code (weatherItem.labelFor above
    // has the group boundaries). `day` swaps the clear-sky glyph for a
    // crescent at night; every other group looks the same either way.
    // `litColor` draws the foreground shape, `dimColor` whatever sits behind
    // or beside it (a second cloud layer, alternating fog bands).
    component WeatherGlyph: Item {
        id: icon

        property int code: 0
        property bool day: true
        property color litColor: "white"
        property color dimColor: "#66888888"

        width: 16
        height: 16

        onCodeChanged: canvas.requestPaint()
        onDayChanged: canvas.requestPaint()
        onLitColorChanged: canvas.requestPaint()
        onDimColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                const cx = w / 2
                const s = Math.min(w, h) // the scale unit for every shape below
                const lit = icon.litColor
                const dim = icon.dimColor
                const code = icon.code

                ctx.lineCap = "round"
                ctx.lineJoin = "round"

                function drawSun(x, y, r, color) {
                    ctx.fillStyle = color
                    ctx.beginPath()
                    ctx.arc(x, y, r, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.strokeStyle = color
                    ctx.lineWidth = Math.max(1, s / 12)
                    for (let i = 0; i < 8; i++) {
                        const a = i * Math.PI / 4
                        ctx.beginPath()
                        ctx.moveTo(x + Math.cos(a) * r * 1.4, y + Math.sin(a) * r * 1.4)
                        ctx.lineTo(x + Math.cos(a) * r * 1.9, y + Math.sin(a) * r * 1.9)
                        ctx.stroke()
                    }
                }

                // A full disc with a shifted circle punched out (destination-out):
                // the simplest crescent on a transparent Canvas, which has no
                // background color to clip against.
                function drawMoon(x, y, r, color) {
                    ctx.save()
                    ctx.fillStyle = color
                    ctx.beginPath()
                    ctx.arc(x, y, r, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.globalCompositeOperation = "destination-out"
                    ctx.beginPath()
                    ctx.arc(x + r * 0.55, y - r * 0.4, r * 0.85, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.restore()
                }

                // Three overlapping circles and a base rect filled as one path;
                // the overlaps are harmless under the nonzero fill rule, since
                // every sub-path winds the same way.
                function drawCloud(x, y, cs, color) {
                    ctx.fillStyle = color
                    ctx.beginPath()
                    ctx.arc(x - cs * 0.85, y, cs * 0.55, 0, Math.PI * 2)
                    ctx.arc(x - cs * 0.15, y - cs * 0.4, cs * 0.7, 0, Math.PI * 2)
                    ctx.arc(x + cs * 0.55, y - cs * 0.05, cs * 0.55, 0, Math.PI * 2)
                    ctx.rect(x - cs * 0.85, y - cs * 0.05, cs * 1.65, cs * 0.6)
                    ctx.fill()
                }

                function drawDrops(x, y, cs, color, count, length) {
                    ctx.strokeStyle = color
                    ctx.lineWidth = Math.max(1, s / 9)
                    for (let i = 0; i < count; i++) {
                        const dx = x - cs * 0.55 + i * (cs * 1.1 / (count - 1))
                        ctx.beginPath()
                        ctx.moveTo(dx, y)
                        ctx.lineTo(dx - length * 0.35, y + length)
                        ctx.stroke()
                    }
                }

                function drawFlakes(x, y, cs, color, count) {
                    ctx.strokeStyle = color
                    ctx.lineWidth = Math.max(1, s / 11)
                    const r = s * 0.09
                    for (let i = 0; i < count; i++) {
                        const dx = x - cs * 0.55 + i * (cs * 1.1 / (count - 1))
                        const dy = y + s * 0.16
                        for (let a = 0; a < 3; a++) {
                            const ang = a * Math.PI / 3
                            ctx.beginPath()
                            ctx.moveTo(dx - Math.cos(ang) * r, dy - Math.sin(ang) * r)
                            ctx.lineTo(dx + Math.cos(ang) * r, dy + Math.sin(ang) * r)
                            ctx.stroke()
                        }
                    }
                }

                function drawBolt(x, y, cs, color) {
                    ctx.fillStyle = color
                    ctx.beginPath()
                    ctx.moveTo(x + cs * 0.05, y)
                    ctx.lineTo(x - cs * 0.28, y + cs * 0.55)
                    ctx.lineTo(x - cs * 0.02, y + cs * 0.55)
                    ctx.lineTo(x - cs * 0.22, y + cs * 1.05)
                    ctx.lineTo(x + cs * 0.32, y + cs * 0.35)
                    ctx.lineTo(x + cs * 0.05, y + cs * 0.35)
                    ctx.closePath()
                    ctx.fill()
                }

                function drawFog(color, altColor) {
                    ctx.lineWidth = Math.max(1, s / 9)
                    const rows = [
                        { y: h * 0.38, hw: w * 0.34, c: altColor },
                        { y: h * 0.58, hw: w * 0.42, c: color },
                        { y: h * 0.78, hw: w * 0.28, c: altColor }
                    ]
                    for (const row of rows) {
                        ctx.strokeStyle = row.c
                        ctx.beginPath()
                        ctx.moveTo(cx - row.hw, row.y)
                        ctx.lineTo(cx + row.hw, row.y)
                        ctx.stroke()
                    }
                }

                if (code === 45 || code === 48) {
                    // Fog: no sky glyph, just bands.
                    drawFog(lit, dim)
                } else if (code === 0 || (code >= 1 && code <= 2)) {
                    // Clear/partly cloudy: the sun or moon shrinks into the
                    // top-right corner once a cloud joins it.
                    const partly = code >= 1 && code <= 2
                    const skyX = partly ? w * 0.62 : cx
                    const skyY = partly ? h * 0.38 : h * 0.42
                    const skyR = partly ? s * 0.22 : s * 0.28
                    if (icon.day)
                        drawSun(skyX, skyY, skyR, lit)
                    else
                        drawMoon(skyX, skyY, skyR, lit)
                    if (partly)
                        drawCloud(w * 0.42, h * 0.66, s * 0.4, dim)
                } else if (code === 3) {
                    // Overcast: two cloud layers, no sky glyph.
                    drawCloud(w * 0.38, h * 0.42, s * 0.32, dim)
                    drawCloud(w * 0.56, h * 0.6, s * 0.4, lit)
                } else {
                    // Everything else is a cloud plus precipitation or a bolt,
                    // with the showers groups getting a sun/moon peeking from
                    // the top-left to set them apart from steady rain/snow.
                    const showers = (code >= 80 && code <= 82) || code === 85 || code === 86
                    if (showers) {
                        if (icon.day)
                            drawSun(w * 0.28, h * 0.3, s * 0.16, lit)
                        else
                            drawMoon(w * 0.28, h * 0.3, s * 0.16, lit)
                    }
                    drawCloud(w * 0.54, h * 0.44, s * 0.4, lit)
                    if (code >= 51 && code <= 57)
                        drawDrops(w * 0.54, h * 0.72, s * 0.4, dim, 3, s * 0.18)
                    else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
                        drawDrops(w * 0.54, h * 0.72, s * 0.4, lit, 3, s * 0.3)
                    else if ((code >= 71 && code <= 77) || code === 85 || code === 86)
                        drawFlakes(w * 0.54, h * 0.74, s * 0.4, lit, 3)
                    else if (code >= 95 && code <= 99)
                        drawBolt(w * 0.54, h * 0.68, s * 0.34, lit)
                }
            }
        }
    }

    Row {
        id: weatherRow
        anchors.centerIn: parent
        spacing: 6

        WeatherGlyph {
            id: barIcon
            anchors.verticalCenter: parent.verticalCenter
            width: 15; height: 15
            code: weatherItem.haveData ? weatherItem.current.code : 0
            day: weatherItem.haveData ? weatherItem.current.isDay : true
            // Code 0 (clear) is the pre-first-response placeholder, and a
            // fully lit sun next to a placeholder dash reads as a reading.
            litColor: weatherItem.haveData ? Colors.text : Qt.alpha(Colors.textMuted, 0.5)
            dimColor: Qt.alpha(Colors.textMuted, 0.4)
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: weatherItem.haveData ? Math.round(weatherItem.current.temp) + "\u00B0" : "\u2014"
            color: weatherItem.haveData ? Colors.text : Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: weatherMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: weatherMenu.toggle()
    }

    Popup {
        id: weatherMenu
        anchorItem: weatherItem
        contentWidth: 280
        contentHeight: weatherColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        Column {
            id: weatherColumn
            width: parent.width
            spacing: 12

            Row {
                spacing: 8

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: weatherItem.placeName
                    color: Colors.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.bold: true
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    // "Loading" only while a first response is still
                    // plausible; after a failure it contradicts the footer.
                    text: weatherItem.haveData ? weatherItem.labelFor(weatherItem.current.code)
                          : (weatherItem.lastFetchFailed ? "Unavailable" : "Loading\u2026")
                    color: Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
            }

            Row {
                spacing: 10

                WeatherGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 40; height: 40
                    code: weatherItem.haveData ? weatherItem.current.code : 0
                    day: weatherItem.haveData ? weatherItem.current.isDay : true
                    litColor: Colors.accent
                    dimColor: Qt.alpha(Colors.textMuted, 0.4)
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: weatherItem.haveData ? Math.round(weatherItem.current.temp) + "\u00B0" : "\u2014"
                    color: Colors.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 28
                    font.bold: true
                }
            }

            // Two lines on purpose: all three figures overflow the popup at
            // ordinary values and wrapMode breaks mid-"Wind 10 km/h", which
            // reads as a layout bug.
            Text {
                visible: weatherItem.haveData
                width: parent.width
                text: weatherItem.haveData
                      ? "Feels like " + Math.round(weatherItem.current.feelsLike) + "\u00B0"
                        + "   Humidity " + Math.round(weatherItem.current.humidity) + "%"
                        + "\nWind " + Math.round(weatherItem.current.windSpeed) + " " + weatherItem.windUnit
                      : ""
                lineHeight: 1.3
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // Tomorrow onward; daily[0] is today, shown above.
            Column {
                width: parent.width
                spacing: 4
                visible: weatherItem.daily.length > 1

                Repeater {
                    model: weatherItem.daily.slice(1, 4)

                    Row {
                        required property var modelData
                        spacing: 8

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: Qt.formatDate(weatherItem.dateFromISO(modelData.date), "ddd")
                            color: Colors.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            width: 30
                        }

                        WeatherGlyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14
                            code: modelData.code
                            day: true
                            litColor: Colors.textMuted
                            dimColor: Qt.alpha(Colors.textMuted, 0.35)
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: weatherItem.tempText(modelData.tMin) + " / " + weatherItem.tempText(modelData.tMax)
                            color: Colors.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: weatherItem.lastFetchFailed
                      ? "Update failed: " + weatherItem.lastError
                      : (weatherItem.lastUpdated
                         ? "Updated " + Qt.formatTime(weatherItem.lastUpdated, "hh:mm")
                         : "Updating\u2026")
                color: weatherItem.lastFetchFailed ? Colors.error : Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }

    Timer {
        id: refreshTimer
        interval: weatherItem.refreshMinutes * 60 * 1000
        running: true
        repeat: true
        triggeredOnStart: false
        onTriggered: weatherItem.fetchWeather()
    }

    // One retry per failure; a failure on the retry schedules the next
    // through fetchFailed, which also pushes refreshTimer out of the way.
    Timer {
        id: retryTimer
        interval: 60000
        running: false
        repeat: false
        onTriggered: weatherItem.fetchWeather()
    }

    Component.onCompleted: fetchWeather()
}
