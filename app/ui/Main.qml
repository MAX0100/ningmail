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

    // Compose flow lives here (task 11): Main owns the composer window, its
    // autosave chain and the send handoff, so ComposerWindow itself never
    // needs the appController context (kept unit-testable in isolation).
    property var composerWindow: null

    Component {
        id: composerComponent

        ComposerWindow {}
    }

    function openComposer(draft) {
        // R78: no window yet means create it (visible:false 起手); a later
        // compose request reuses the existing instance instead of stacking
        // windows.
        if (root.composerWindow === null) {
            root.composerWindow = composerComponent.createObject(root, {visible: false})
            if (root.composerWindow === null) {
                console.warn("Failed to create the composer window:",
                             composerComponent.errorString())
                return
            }
            root.composerWindow.draftSaveRequested.connect(
                function(draftId, to, cc, subject, body) {
                    const savedId = appController.saveDraft(draftId, to, cc, subject, body)
                    if (savedId > 0) {
                        root.composerWindow.draftId = savedId
                    }
                })
            root.composerWindow.sendRequested.connect(
                function(to, cc, subject, body) {
                    appController.sendMessage(to, cc, subject, body)
                    // R78: sending closes the window; the autosaved row stays
                    // in the drafts table as the M1 record of the message.
                    root.composerWindow.visible = false
                })
        }

        // Each request fully defines the window content: composeNew emits {}
        // and starts a blank draft (previous drafts stay in the store),
        // mailto/task 15 carries its own five-key payload.
        const composer = root.composerWindow
        composer.draftId = draft.draftId !== undefined ? draft.draftId : -1
        composer.initialTo = draft.to !== undefined ? draft.to : ""
        composer.initialCc = draft.cc !== undefined ? draft.cc : ""
        composer.initialSubject = draft.subject !== undefined ? draft.subject : ""
        composer.initialBody = draft.body !== undefined ? draft.body : ""
        // createObject() applies initial properties before onCompleted only on
        // first creation; a reused window must re-apply the prefill itself.
        composer.applyInitial()

        // Hidden until the prefill sits in place, so the user never sees a
        // half-filled window.
        composer.visible = true
        composer.requestActivate()
    }

    Connections {
        target: appController

        function onComposeRequested(draft) {
            root.openComposer(draft)
        }
    }

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
