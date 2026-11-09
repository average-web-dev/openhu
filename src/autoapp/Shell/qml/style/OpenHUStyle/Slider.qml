import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's slider with its sizes times Theme.scale (README.md): a larger handle and room for it
M.Slider {
    padding: 6 * Theme.scale
    handle.scale: Theme.scale
    background.implicitWidth: (horizontal ? 200 : 48) * Theme.scale
    background.implicitHeight: (horizontal ? 48 : 200) * Theme.scale
}
