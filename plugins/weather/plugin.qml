import QtQuick
import qwin 1.0
import "../shared"

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

    Row {
        id: weatherRow
        anchors.centerIn: parent
        spacing: 6

        WeatherIcon {
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
                    text: weatherItem.haveData ? popupIcon.labelFor(weatherItem.current.code)
                          : (weatherItem.lastFetchFailed ? "Unavailable" : "Loading\u2026")
                    color: Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
            }

            Row {
                spacing: 10

                WeatherIcon {
                    id: popupIcon
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

                        WeatherIcon {
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
