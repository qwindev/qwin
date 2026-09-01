pragma ComponentBehavior: Bound
import QtQuick
import qwin 1.0
import "../shared"

// Clock in the bar; clicking it opens a month calendar, the same
// module + Popup idiom as the wifi plugin. The bar text format comes from
// this plugin's config.json section ("clock": { "format": "..." },
// Qt.formatDateTime syntax). No `shown`: the clock never hides itself.
Rectangle {
    id: clockItem

    readonly property string format:
        Plugins.config("clock").format || "ddd dd MMM  hh:mm:ss"

    // `now` ticks every second; `today` truncates it to a calendar day.
    // Bindings re-notify only when the VALUE changes, so the 42 grid cells
    // re-evaluate their highlight on the day rollover, not every second.
    property date now: new Date()
    readonly property date today: new Date(now.getFullYear(), now.getMonth(), now.getDate())

    // ISO 8601: shift to the Thursday of the same Mon-Sun week, then count
    // whole weeks since the January Thursday that starts week 1.
    function isoWeek(date) {
        const d = new Date(date.getFullYear(), date.getMonth(), date.getDate())
        const dayNum = (d.getDay() + 6) % 7 // Monday=0 .. Sunday=6
        d.setDate(d.getDate() - dayNum + 3) // nearest Thursday
        const firstThursday = new Date(d.getFullYear(), 0, 4)
        const firstDayNum = (firstThursday.getDay() + 6) % 7
        firstThursday.setDate(firstThursday.getDate() - firstDayNum + 3)
        return 1 + Math.round((d - firstThursday) / (7 * 24 * 3600 * 1000))
    }

    width: clockText.implicitWidth + 14
    height: 24
    radius: 5
    color: clockMouse.containsMouse || calendarPopup.opened ? Qt.alpha(Colors.surface, 0.13)
                                                             : "transparent"

    Text {
        id: clockText
        anchors.centerIn: parent
        color: Colors.text
        font.family: Theme.fontFamily
        font.pixelSize: 14
        text: Qt.formatDateTime(clockItem.now, clockItem.format)
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: clockItem.now = new Date()
    }

    MouseArea {
        id: clockMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: calendarPopup.toggle()
    }

    Popup {
        id: calendarPopup
        anchorItem: clockItem
        contentWidth: 250
        contentHeight: calendarColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        // The month being paged through. The reset below takes over from the
        // binding on first open, so reopening starts back at today.
        property int viewYear: clockItem.today.getFullYear()
        property int viewMonth: clockItem.today.getMonth()

        onOpenedChanged: if (opened) {
            viewYear = clockItem.today.getFullYear()
            viewMonth = clockItem.today.getMonth()
        }

        function shiftMonth(delta) {
            let m = viewMonth + delta
            let y = viewYear
            if (m < 0) { m = 11; y -= 1 }
            else if (m > 11) { m = 0; y += 1 }
            viewMonth = m
            viewYear = y
        }

        // A fixed 42-cell Monday-first grid, so the popup's height never
        // changes between months. JS Date normalizes out-of-range
        // components, so month length, leap years and year boundaries need
        // no special-casing.
        function buildGrid() {
            const first = new Date(viewYear, viewMonth, 1)
            const lead = (first.getDay() + 6) % 7 // Monday=0 .. Sunday=6
            const start = new Date(viewYear, viewMonth, 1 - lead)
            const cells = []
            for (let i = 0; i < 42; i++)
                cells.push(new Date(start.getFullYear(), start.getMonth(), start.getDate() + i))
            return cells
        }

        readonly property var gridModel: buildGrid()

        Column {
            id: calendarColumn
            width: parent.width
            spacing: 10

            Row {
                width: parent.width
                height: 22

                Rectangle {
                    id: prevMonthBtn
                    width: 22; height: 22
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: prevMonthMouse.containsMouse ? Qt.alpha(Colors.surface, 0.2) : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "\u2039"
                        color: Colors.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: prevMonthMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: calendarPopup.shiftMonth(-1)
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - prevMonthBtn.width - nextMonthBtn.width
                    horizontalAlignment: Text.AlignHCenter
                    text: Qt.formatDate(new Date(calendarPopup.viewYear, calendarPopup.viewMonth, 1), "MMMM yyyy")
                    color: Colors.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.bold: true
                }

                Rectangle {
                    id: nextMonthBtn
                    width: 22; height: 22
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: nextMonthMouse.containsMouse ? Qt.alpha(Colors.surface, 0.2) : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "\u203A"
                        color: Colors.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: nextMonthMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: calendarPopup.shiftMonth(1)
                    }
                }
            }

            Row {
                width: parent.width

                Repeater {
                    model: ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
                    Text {
                        required property string modelData
                        width: calendarColumn.width / 7
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData
                        color: Colors.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                    }
                }
            }

            Grid {
                width: parent.width
                columns: 7

                Repeater {
                    model: calendarPopup.gridModel
                    Item {
                        id: dayCell
                        required property date modelData
                        width: calendarColumn.width / 7
                        height: 26

                        readonly property bool inMonth: modelData.getMonth() === calendarPopup.viewMonth
                                                        && modelData.getFullYear() === calendarPopup.viewYear
                        readonly property bool isToday: inMonth
                            && modelData.getFullYear() === clockItem.today.getFullYear()
                            && modelData.getMonth() === clockItem.today.getMonth()
                            && modelData.getDate() === clockItem.today.getDate()
                        readonly property bool isWeekend: {
                            const wd = modelData.getDay()
                            return wd === 0 || wd === 6
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: 20; height: 20
                            radius: 10
                            visible: dayCell.isToday
                            color: Colors.accent
                        }

                        Text {
                            anchors.centerIn: parent
                            text: dayCell.modelData.getDate()
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: dayCell.isToday ? Colors.background
                                   : !dayCell.inMonth ? Qt.alpha(Colors.textMuted, 0.45)
                                   : dayCell.isWeekend ? Qt.alpha(Colors.text, 0.7)
                                   : Colors.text
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: Qt.formatDate(clockItem.today, "dddd, d MMMM yyyy")
                      + "  \u00B7  Week " + clockItem.isoWeek(clockItem.today)
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }
}
