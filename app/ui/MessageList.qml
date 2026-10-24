import QtQuick
import QtQuick.Controls

Pane {
    id: root

    padding: 0
    background: Rectangle {
        color: Theme.surface
    }

    ListView {
        id: messageListView

        anchors.fill: parent
        clip: true
        model: appController.messages

        // Near the bottom, fetch the next page; the canFetchMore guard keeps
        // the call idempotent and a no-op once the folder is fully loaded.
        onContentYChanged: {
            const nearBottom = contentHeight - height - contentY < 120
            if (nearBottom && appController.messages.canFetchMore()) {
                appController.messages.loadMore()
            }
        }

        EmptyState {
            anchors.fill: parent
            visible: messageListView.count === 0
            title: qsTr("没有邮件")
            description: qsTr("选择一个文件夹，或点击 ⟳ 刷新同步邮件")
        }

        delegate: MessageListDelegate {}
    }
}
