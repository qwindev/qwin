import QtQuick
import qwin 1.0
import "../shared"

// Live sparklines for CPU, RAM and network throughput. Each series samples
// off the singletons' own change signals rather than a private Timer:
// System.statsChanged for CPU/RAM, Wifi.throughputChanged for the network.
// Those are deliberately separate signals - reading throughput inside the
// stats handler would re-run the CPU/RAM bindings on the network's tick.
//
// config.json section (all keys optional):
//   "graphs": {
//       "history": 60,                 // samples kept per series
//       "show": ["cpu", "ram", "net"], // which cells, in order
//       "width": 46                    // sparkline width in px
//   }
Rectangle {
    id: graphs

    // Read once per load; a config.json edit triggers a full reload anyway.
    readonly property var cfg: Plugins.config("graphs")
    readonly property int historyLen: cfg.history || 60
    readonly property var showList: cfg.show || ["cpu", "ram", "net"]
    readonly property int sparkWidth: cfg.width || 46

    property var cpuHistory: []
    property var ramHistory: []
    property var netHistory: []

    // Returns a NEW array: mutating one in place is invisible to bindings,
    // so the Sparkline would sit frozen after its first paint.
    function pushSample(arr, value) {
        const next = arr.concat([value])
        if (next.length > historyLen)
            next.splice(0, next.length - historyLen)
        return next
    }

    // wifi/plugin.qml's padded K/M/G, over the combined rx+tx rate.
    function speedText(bytesPerSec) {
        const kb = bytesPerSec / 1024
        if (kb < 1)
            return "0K".padStart(6)
        if (kb < 1000)
            return (Math.round(kb) + "K").padStart(6)
        const mb = kb / 1024
        if (mb < 1000)
            return (mb.toFixed(1) + "M").padStart(6)
        return ((mb / 1024).toFixed(1) + "G").padStart(6)
    }

    Connections {
        target: System
        function onStatsChanged() {
            graphs.cpuHistory = graphs.pushSample(graphs.cpuHistory, System.cpuUsage)
            graphs.ramHistory = graphs.pushSample(graphs.ramHistory, System.memoryUsagePercent)
        }
    }

    Connections {
        target: Wifi
        function onThroughputChanged() {
            graphs.netHistory = graphs.pushSample(graphs.netHistory, Wifi.rxBytesPerSec + Wifi.txBytesPerSec)
        }
    }

    width: contentRow.implicitWidth + 14
    height: 24
    radius: 5
    color: hoverArea.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent"

    // Caption + Sparkline + value readout. Colors are passed in, since
    // shared/ components take them as properties rather than read Colors.
    component Cell: Row {
        id: cell
        property string caption
        property alias values: spark.values
        property real fixedMax: 0
        property real floorScale: 0
        property color lineColor: Colors.accent
        property alias valueText: valueLabel.text
        // Missing/true is visible, like the bar's `shown`; only NET sets it.
        property bool shown: true

        spacing: 6

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: cell.caption
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 11
        }

        Sparkline {
            id: spark
            anchors.verticalCenter: parent.verticalCenter
            width: graphs.sparkWidth
            height: 16
            maxValue: cell.fixedMax
            minScale: cell.floorScale
            lineColor: cell.lineColor
            fillColor: Qt.alpha(cell.lineColor, 0.22)
            lineWidth: 1.5
        }

        Text {
            id: valueLabel
            anchors.verticalCenter: parent.verticalCenter
            color: Colors.text
            font.family: Theme.fontFamily
            font.pixelSize: 12
        }
    }

    Component {
        id: cpuCell
        Cell {
            caption: "CPU"
            values: graphs.cpuHistory
            fixedMax: 100
            lineColor: Colors.accent
            valueText: System.cpuUsage.toFixed(0).padStart(3) + "%"
        }
    }

    Component {
        id: ramCell
        Cell {
            caption: "RAM"
            values: graphs.ramHistory
            fixedMax: 100
            lineColor: Colors.warning
            valueText: System.memoryUsagePercent.toFixed(0).padStart(3) + "%"
        }
    }

    Component {
        id: netCell
        Cell {
            caption: "NET"
            values: graphs.netHistory
            // Auto-scaled with a 64 KB/s floor, so an idle link's few-byte
            // jitter stays a flat line instead of reading as a spike.
            floorScale: 64 * 1024
            lineColor: Colors.error
            valueText: graphs.speedText(Wifi.rxBytesPerSec + Wifi.txBytesPerSec) + "B/s"
            shown: Wifi.available
        }
    }

    Row {
        id: contentRow
        anchors.centerIn: parent
        spacing: 14

        Repeater {
            model: graphs.showList
            Loader {
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: modelData === "cpu" ? cpuCell
                                : modelData === "ram" ? ramCell
                                : modelData === "net" ? netCell
                                : null
                // Like bar/plugin.qml's Section: a hidden cell (only NET,
                // without a WLAN adapter) leaves no Row gap, since
                // positioners ignore invisible children.
                visible: item !== null && (item.shown === undefined ? true : item.shown)
            }
        }
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
    }
}
