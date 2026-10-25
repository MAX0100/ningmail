import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    objectName: "accountSetupDialog"
    title: qsTr("添加账号")
    modal: true
    width: 400

    signal accountCreateRequested(var fields)

    function portValid(text) {
        const port = parseInt(text, 10)
        return Number.isInteger(port) && port >= 1 && port <= 65535
    }

    // Mirrors AppController::createAccount's defence (R93⑦): required fields
    // and both ports; the security boxes always carry a legal value.
    readonly property bool inputsValid:
        emailField.text.trim().length > 0 && imapHostField.text.trim().length > 0
        && smtpHostField.text.trim().length > 0 && userField.text.trim().length > 0
        && passwordField.text.length > 0 && portValid(imapPortField.text)
        && portValid(smtpPortField.text)

    // Build the fields payload and report it; the dialog closes itself and
    // AppController owns the outcome (statusText covers failures later, R93⑦).
    function save() {
        if (!inputsValid) {
            return
        }
        dialog.accountCreateRequested({
            "displayName": nameField.text.trim(),
            "email": emailField.text.trim(),
            "imapHost": imapHostField.text.trim(),
            "imapPort": parseInt(imapPortField.text, 10),
            "imapSecurity": imapSecurityBox.currentValue,
            "smtpHost": smtpHostField.text.trim(),
            "smtpPort": parseInt(smtpPortField.text, 10),
            "smtpSecurity": smtpSecurityBox.currentValue,
            "username": userField.text.trim(),
            "password": passwordField.text
        })
        dialog.close()
    }

    footer: DialogButtonBox {
        Button {
            objectName: "saveButton"
            text: qsTr("保存")
            enabled: dialog.inputsValid
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: dialog.save()
        }

        Button {
            objectName: "cancelButton"
            text: qsTr("取消")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    ColumnLayout {
        spacing: Theme.spacing / 2

        Label {
            objectName: "displayNameLabel"
            text: qsTr("显示名（可选）")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        TextField {
            id: nameField

            objectName: "nameField"
            Layout.fillWidth: true
            placeholderText: qsTr("例如：Home")
        }

        Label {
            objectName: "emailLabel"
            text: qsTr("邮箱地址")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        TextField {
            id: emailField

            objectName: "emailField"
            Layout.fillWidth: true
            placeholderText: qsTr("例如：home@example.com")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing / 2

                Label {
                    objectName: "imapHostLabel"
                    text: qsTr("IMAP 服务器")
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }

                TextField {
                    id: imapHostField

                    objectName: "imapHostField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("imap.example.com")
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing / 2

                Label {
                    objectName: "imapPortLabel"
                    text: qsTr("端口（1–65535）")
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }

                TextField {
                    id: imapPortField

                    objectName: "imapPortField"
                    Layout.fillWidth: true
                    text: "993"
                    validator: IntValidator {
                        bottom: 1
                        top: 65535
                    }
                }
            }
        }

        Label {
            objectName: "imapSecurityLabel"
            text: qsTr("IMAP 加密")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        ComboBox {
            id: imapSecurityBox

            objectName: "imapSecurityBox"
            Layout.fillWidth: true
            model: ["tls", "starttls", "none"]
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing / 2

                Label {
                    objectName: "smtpHostLabel"
                    text: qsTr("SMTP 服务器")
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }

                TextField {
                    id: smtpHostField

                    objectName: "smtpHostField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("smtp.example.com")
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing / 2

                Label {
                    objectName: "smtpPortLabel"
                    text: qsTr("端口（1–65535）")
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }

                TextField {
                    id: smtpPortField

                    objectName: "smtpPortField"
                    Layout.fillWidth: true
                    text: "465"
                    validator: IntValidator {
                        bottom: 1
                        top: 65535
                    }
                }
            }
        }

        Label {
            objectName: "smtpSecurityLabel"
            text: qsTr("SMTP 加密")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        ComboBox {
            id: smtpSecurityBox

            objectName: "smtpSecurityBox"
            Layout.fillWidth: true
            model: ["tls", "starttls", "none"]
        }

        Label {
            objectName: "usernameLabel"
            text: qsTr("用户名（留空则用邮箱）")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        TextField {
            id: userField

            objectName: "userField"
            Layout.fillWidth: true
        }

        Label {
            objectName: "passwordLabel"
            text: qsTr("密码或授权码")
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        TextField {
            id: passwordField

            objectName: "passwordField"
            Layout.fillWidth: true
            // Secrets never render as plain text (task 15 self-review).
            echoMode: TextInput.Password
        }
    }
}
