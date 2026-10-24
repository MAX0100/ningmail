#pragma once

#include "MailError.h"
#include "Store/AccountStore.h"
#include "Store/FolderStore.h"
#include "Store/MessageStore.h"
#include "SyncService.h"
#include "Types.h"
#include "models/FolderListModel.h"
#include "models/MessageListModel.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace ningmail {

class CredentialStore;

/// GUI-thread orchestrator of the three-pane shell: owns the list models and
/// wires SyncService results through the stores and back into the models.
/// Every call happens on the GUI thread; connections to the service use
/// Qt::AutoConnection so a thread move would be handled by Qt, not by hand.
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(FolderListModel *folders READ folders CONSTANT)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QVariantMap reader READ reader NOTIFY readerChanged)
  public:
    explicit AppController(AccountStore &accountStore, FolderStore &folderStore,
                           MessageStore &messageStore, SyncService &syncService,
                           CredentialStore &credentials, QObject *parent = nullptr);

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void selectFolder(FolderId folderId);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openMessage(MessageId messageId);
    Q_INVOKABLE void composeNew();

    FolderListModel *folders() const {
        return m_folders.get();
    }

    MessageListModel *messages() const {
        return m_messages.get();
    }

    bool syncing() const {
        return m_syncing;
    }

    QString statusText() const {
        return m_statusText;
    }

    QVariantMap reader() const {
        return m_reader;
    }

  signals:
    void syncingChanged();
    void statusTextChanged();
    void readerChanged();
    void composeRequested(const QVariantMap &draft);

  private:
    void setStatusText(const QString &text);
    void setReader(const QVariantMap &reader);
    void clearReader();

    AccountStore &m_accountStore;
    FolderStore &m_folderStore;
    MessageStore &m_messageStore;
    SyncService &m_syncService;
    // Credentials are only ever touched on the GUI thread (R62); the sync
    // service reads the secret through its own reference.
    CredentialStore &m_credentials;
    std::unique_ptr<FolderListModel> m_folders;
    std::unique_ptr<MessageListModel> m_messages;
    /// Flat (M1) map of synced folder fullName -> DB id, filled in the
    /// foldersFetched handler and consulted by headersFetched (R58).
    QHash<QString, FolderId> m_folderIdsByFullName;
    AccountId m_currentAccountId = 0;
    bool m_syncing = false;
    QString m_statusText;
    QVariantMap m_reader;
};

} // namespace ningmail
