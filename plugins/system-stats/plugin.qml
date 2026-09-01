import QtQuick
import Qwin
import "../shared"

// CPU/RAM readout: the minimal text-only example (`graphs` is its graphical
// counterpart).
Text {
    // padStart holds each value 3 chars wide, so nothing jumps at a digit
    // boundary - the font is monospace.
    text: "CPU " + System.cpuUsage.toFixed(0).padStart(3) + "%   RAM " + System.memoryUsagePercent.toFixed(0).padStart(3) + "%"
    color: Colors.textMuted
    font.family: Theme.fontFamily
    font.pixelSize: 13
}
