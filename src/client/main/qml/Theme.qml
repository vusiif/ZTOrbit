pragma Singleton
import QtQuick

QtObject {
    property bool dark: false
    readonly property bool transitioning: windowAnimation.running
    property color window: dark ? "#202830" : "#f4f7fa"
    property color surface: dark ? "#27323c" : "#ffffff"
    property color button: dark ? "#3b4c5e" : "#e7eff8"
    property color popup: dark ? "#363541" : "#f3f1f8"
    property color popupHeader: dark ? "#454351" : "#e7e3f0"
    property color popupBorder: dark ? "#8e91ad" : "#9698b4"
    property color field: dark ? "#1d252e" : "#ffffff"
    property color side: dark ? "#253746" : "#eaf1f7"
    property color text: dark ? "#e1ebf3" : "#20394c"
    property color muted: dark ? "#a9bdcb" : "#607687"
    property color border: dark ? "#455b6c" : "#b9cbd8"
    property color accent: dark ? "#9cceee" : "#21486b"
    property color selection: dark ? "#365772" : "#d8eaf6"
    property color hover: dark ? "#354e61" : "#e3eff8"
    property color danger: dark ? "#ffaaa1" : "#a7352d"
    Behavior on window { ColorAnimation { id: windowAnimation; duration: 180 } }
    Behavior on surface { ColorAnimation { duration: 180 } }
    Behavior on side { ColorAnimation { duration: 180 } }
    Behavior on text { ColorAnimation { duration: 180 } }
    Behavior on muted { ColorAnimation { duration: 180 } }
    Behavior on border { ColorAnimation { duration: 180 } }
    Behavior on accent { ColorAnimation { duration: 180 } }
    Behavior on selection { ColorAnimation { duration: 180 } }
    Behavior on hover { ColorAnimation { duration: 180 } }
    Behavior on button { ColorAnimation { duration: 180 } }
    Behavior on popup { ColorAnimation { duration: 180 } }
    Behavior on popupHeader { ColorAnimation { duration: 180 } }
    Behavior on popupBorder { ColorAnimation { duration: 180 } }
    Behavior on field { ColorAnimation { duration: 180 } }
}
