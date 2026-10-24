import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root

    objectName: "mainWindow"
    width: 1280
    height: 800
    visible: true
    title: qsTr("NingMail")
    color: Theme.background

    header: TopBar {}
    footer: StatusBar {}

    SplitView {
        id: splitView

        anchors.fill: parent

        FolderRail {
            SplitView.preferredWidth: 200
            SplitView.minimumWidth: 140
        }

        MessageList {
            SplitView.preferredWidth: 380
            SplitView.minimumWidth: 260
        }

        ReaderPane {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }
    }
}
