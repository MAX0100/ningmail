import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: composer

    objectName: "composerWindow"
    width: 560
    height: 480
    title: qsTr("写信")
    color: Theme.background

    signal sendRequested(var to, var cc, var subject, var body)
    signal draftSaveRequested(var draftId, var to, var cc, var subject, var body)

    /// -1 = unsaved/new draft; Main writes the store id back after saveDraft()
    /// so later autosaves update the same row instead of inserting another.
    property int draftId: -1
    /// Debounce before each save tick; 3000 ms for the app, tests shrink it.
    property int autosaveInterval: 3000
    /// Prefill source applied once at creation (see applyInitial below).
    property string initialTo
    property string initialCc
    property string initialSubject
    property string initialBody

    /// Comma-separated field text -> trimmed, de-duplicated-on-empty list of
    /// plain email addresses (M1 has no address book; no name resolution).
    function parseAddresses(text) {
        const addresses = []
        for (const part of text.split(",")) {
            const address = part.trim()
            if (address.length > 0) {
                addresses.push(address)
            }
        }
        return addresses
    }

    // Valid recipient list; empty until toField carries content. cc-only
    // never enables sending (R78).
    property var recipients: parseAddresses(toField ? toField.text : "")

    function applyInitial() {
        toField.text = composer.initialTo
        ccField.text = composer.initialCc
        subjectField.text = composer.initialSubject
        bodyField.text = composer.initialBody
    }

    /// Guarded send path, shared by the button and the shortcut.
    function doSend() {
        const to = parseAddresses(toField.text)
        if (to.length === 0) {
            return // errorLabel already points at the missing recipients
        }
        composer.sendRequested(to, parseAddresses(ccField.text), subjectField.text, bodyField.text)
    }

    // Prefill must go through imperative assignment, not field bindings:
    // user edits would otherwise rip the binding out with a warning.
    Component.onCompleted: {
        composer.applyInitial()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing
        spacing: Theme.spacing

        TextField {
            id: toField

            objectName: "toField"
            placeholderText: qsTr("收件人，多个用逗号分隔")
            Layout.fillWidth: true
            onTextChanged: autosaveTimer.restart()
        }

        TextField {
            id: ccField

            objectName: "ccField"
            placeholderText: qsTr("抄送，多个用逗号分隔")
            Layout.fillWidth: true
            onTextChanged: autosaveTimer.restart()
        }

        TextField {
            id: subjectField

            objectName: "subjectField"
            placeholderText: qsTr("主题")
            Layout.fillWidth: true
            onTextChanged: autosaveTimer.restart()
        }

        TextArea {
            id: bodyField

            objectName: "bodyField"
            placeholderText: qsTr("正文…")
            wrapMode: TextArea.Wrap
            persistentSelection: true
            textFormat: TextArea.PlainText
            Layout.fillWidth: true
            Layout.fillHeight: true
            background: Rectangle {
                color: Theme.surface
                border.color: Theme.divider
                radius: Theme.radius
            }
            onTextChanged: autosaveTimer.restart()
        }

        Label {
            id: errorLabel

            objectName: "errorLabel"
            text: composer.recipients.length > 0 ? "" : qsTr("请填写收件人")
            visible: text.length > 0
            color: Theme.danger
        }

        Button {
            id: sendButton

            objectName: "sendButton"
            text: qsTr("发送")
            enabled: composer.recipients.length > 0
            Layout.alignment: Qt.AlignRight
            onClicked: composer.doSend()
        }
    }

    Timer {
        id: autosaveTimer

        interval: composer.autosaveInterval
        running: composer.visible
        onTriggered: {
            // A pristine, never-typed window holds no content yet: skipping
            // the tick keeps "open the composer, change nothing, close" from
            // piling empty draft rows into the store. Once any field carries
            // data (or the draft already has an id to update), the tick saves.
            if (composer.draftId < 0 && parseAddresses(toField.text).length === 0
                    && parseAddresses(ccField.text).length === 0 && subjectField.text.length === 0
                    && bodyField.text.length === 0) {
                return
            }
            composer.draftSaveRequested(composer.draftId, parseAddresses(toField.text),
                                        parseAddresses(ccField.text), subjectField.text,
                                        bodyField.text)
        }
    }

    Shortcut {
        sequence: "Ctrl+Return"
        onActivated: composer.doSend()
    }
}
