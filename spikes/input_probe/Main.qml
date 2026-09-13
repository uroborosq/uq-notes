import QtQuick
import QtQuick.Controls
import UqInputProbe

ApplicationWindow {
    width: 1000
    height: 780
    visible: true
    color: "#101014"
    title: qsTr("Спайк 1.1 — ввод: тип устройства, давление, pointerType")

    // Зона ввода отделена от панелей: списки и кнопки не должны перехватывать
    // события пера, иначе спайк измерял бы не то.
    Rectangle {
        id: inputArea
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: parent.height * 0.45
        color: "#17171e"
        border.color: "#3a3a4a"

        ProbeItem {
            id: probe
            anchors.fill: parent
        }

        Text {
            anchors.centerIn: parent
            horizontalAlignment: Text.AlignHCenter
            color: "#5a5a6a"
            font.pixelSize: 16
            text: qsTr("Пишите здесь пером.\nПереверните перо и потрите ластиком.\nКоснитесь пальцами и сделайте pinch.\nПотом закройте окно — итог уйдёт в консоль.")
        }
    }

    Rectangle {
        id: summaryPanel
        anchors { left: parent.left; right: parent.right; top: inputArea.bottom }
        height: 190
        color: "#1b1b22"

        Text {
            anchors.fill: parent
            anchors.margins: 10
            color: "#9de5a0"
            font.family: "monospace"
            font.pixelSize: 13
            text: probe.summary
        }

        Button {
            anchors { right: parent.right; bottom: parent.bottom; margins: 10 }
            text: qsTr("Сброс")
            onClicked: probe.reset()
        }
    }

    ListView {
        anchors { left: parent.left; right: parent.right; top: summaryPanel.bottom; bottom: parent.bottom }
        clip: true
        model: probe.log
        verticalLayoutDirection: ListView.BottomToTop
        delegate: Text {
            required property string modelData
            color: "#c8c8d0"
            font.family: "monospace"
            font.pixelSize: 12
            text: modelData
        }
    }
}
