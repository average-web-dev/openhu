# OpenHUStyle

The Qt Quick Controls style of the shell and the plugin pages: Material (dark, set in `autoapp.cpp`) with the sizes
of the controls the shell uses times `Theme.scale`. The shell is designed for 1280x720 and scales its own items with
`Theme.scale`; Material's controls have fixed pixel sizes (button height, paddings, switch indicator), so on a larger
display they stayed small next to the scaled text. At 1280x720 (`Theme.scale` 1) they look exactly like Material.

Every file derives from Material's control (`import QtQuick.Controls.Material as M`) and only changes numbers;
controls without a file here come from Material unchanged (`import QtQuick.Controls.Material` in `qmldir`). The text
size comes from the window font (`Main.qml`: Material's 14 px times `Theme.scale`), text fields, spin and combo
boxes and menu items use Material's 16 px, buttons its medium weight. Indicators drawn by Material at a fixed size
(switch, combo box arrow, slider handle) are scaled with `scale`.
