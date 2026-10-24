#include "AppController.h"

#include "CredentialStore.h"

#include <QList>
#include <utility>

namespace ningmail {

namespace {

/// Short Chinese label per error code; the original detail is appended so no
/// information is lost in translation (R51 修订版 keeps backend errors intact).
QString errorLabel(ErrorCode code) {
    switch (code) {
    case ErrorCode::None:
        return QString();
    case ErrorCode::Auth:
        return QStringLiteral("认证失败");
    case ErrorCode::Network:
        return QStringLiteral("网络错误");
    case ErrorCode::Tls:
        return QStringLiteral("TLS 错误");
    case ErrorCode::Protocol:
        return QStringLiteral("协议错误");
    case ErrorCode::Storage:
        return QStringLiteral("存储错误");
    case ErrorCode::Quota:
        return QStringLiteral("配额不足");
    case ErrorCode::Cancelled:
        return QStringLiteral("已取消");
    case ErrorCode::Unknown:
        return QStringLiteral("未知错误");
    }
    return QStringLiteral("未知错误");
}

QString syncStatusText(const MailError &error) {
    if (!error) {
        return QStringLiteral("同步完成");
    }
    QString text = QStringLiteral("同步失败：") + errorLabel(error.code);
    const QString detail = !error.detail.isEmpty() ? error.detail : error.message;
    if (!detail.isEmpty()) {
        text += QStringLiteral("（%1）").arg(detail);
    }
    return text;
}

} // namespace

AppController::AppController(AccountStore &accountStore, FolderStore &folderStore,
                             MessageStore &messageStore, SyncService &syncService,
                             CredentialStore &credentials, QObject *parent)
    : QObject(parent), m_accountStore(accountStore), m_folderStore(folderStore),
      m_messageStore(messageStore), m_syncService(syncService), m_credentials(credentials),
      m_folders(std::make_unique<FolderListModel>(folderStore)),
      m_messages(std::make_unique<MessageListModel>(messageStore)) {
    // R62: Qt::AutoConnection on every service connection. The service delivers
    // on the GUI thread today (direct calls); a future thread move would keep
    // working through queued delivery without any change here.

    connect(&m_syncService, &SyncService::syncStarted, this, [this](AccountId accountId) {
        if (accountId != m_currentAccountId) {
            return;
        }
        if (!m_syncing) {
            m_syncing = true;
            emit syncingChanged();
        }
        setStatusText(QStringLiteral("正在同步…"));
    });

    connect(&m_syncService, &SyncService::foldersFetched, this,
            [this](AccountId accountId, const QList<Folder> &folders) {
                // R58: upsert every folder and remember its DB id per
                // fullName; the fake backend's internal folder ids must never
                // reach messages.folder_id.
                m_folderIdsByFullName.clear();
                for (const Folder &folder : folders) {
                    const auto stored = m_folderStore.upsert(folder);
                    if (!stored.ok()) {
                        qWarning() << "Folder upsert failed:" << stored.error.message
                                   << stored.error.detail;
                        continue;
                    }
                    m_folderIdsByFullName.insert(folder.fullName, *stored.value);
                }
                m_folders->reload(accountId);
                // reload() is a no-op while no folder is selected; safe either way (R58).
                m_messages->reload();
            });

    connect(&m_syncService, &SyncService::headersFetched, this,
            [this](AccountId accountId, const QString &folderFullName,
                   const QList<MessageHeader> &headers) {
                // accountId keeps the value the backend reported (R58); only
                // folder_id is remapped to the DB id.
                FolderId folderId = m_folderIdsByFullName.value(folderFullName, 0);
                if (folderId <= 0) {
                    // Fallback for a folder without a prior upsert: create the
                    // flat row immediately instead of dropping the headers.
                    Folder fallback;
                    fallback.accountId = accountId;
                    fallback.fullName = folderFullName;
                    fallback.role = FolderRole::Custom;
                    const auto stored = m_folderStore.upsert(fallback);
                    if (!stored.ok()) {
                        // Intentional early exit, not a bug: with no row
                        // inserted there is nothing to reload and the map is
                        // left untouched; the failure surfaces via statusText.
                        qWarning() << "Folder upsert failed:" << stored.error.message
                                   << stored.error.detail;
                        setStatusText(
                            QStringLiteral("同步失败：存储错误（%1）").arg(stored.error.detail));
                        return;
                    }
                    folderId = *stored.value;
                    m_folderIdsByFullName.insert(folderFullName, folderId);
                }

                QList<MessageHeader> toInsert = headers;
                for (MessageHeader &header : toInsert) {
                    header.folderId = folderId;
                }
                if (!toInsert.isEmpty()) {
                    const MailError error = m_messageStore.insertBatch(toInsert);
                    if (error) {
                        qWarning()
                            << "Message batch insert failed:" << error.message << error.detail;
                        setStatusText(QStringLiteral("同步失败：存储错误（%1）").arg(error.detail));
                        return;
                    }
                }
                m_messages->reload();
            });

    connect(&m_syncService, &SyncService::syncFinished, this,
            [this](AccountId accountId, const MailError &error) {
                if (accountId != m_currentAccountId) {
                    return;
                }
                m_syncing = false;
                emit syncingChanged();
                setStatusText(syncStatusText(error));
            });

    clearReader();
}

void AppController::initialize() {
    clearReader();
    m_currentAccountId = 0;
    const auto accounts = m_accountStore.list();
    if (!accounts.ok() || accounts.value->isEmpty()) {
        // R57: no account keeps the shell empty and points at setup instead of
        // syncing anything.
        setStatusText(QStringLiteral("请先添加账号"));
        return;
    }
    const Account &first = accounts.value->first();
    m_currentAccountId = first.id;
    m_folders->reload(first.id);
    m_messages->reload();
    // R59: the demo mode never syncs on startup; the user clicks refresh.
    setStatusText(QStringLiteral("演示模式：已加载 %1，点击 ⟳ 刷新开始同步邮件")
                      .arg(first.displayName.isEmpty() ? first.email : first.displayName));
}

void AppController::selectFolder(FolderId folderId) {
    m_messages->setFolder(folderId);
    clearReader();
}

void AppController::refresh() {
    if (m_currentAccountId <= 0) {
        // No account: only the status hint changes, nothing syncs.
        setStatusText(QStringLiteral("请先添加账号，添加后才能刷新"));
        return;
    }
    const auto account = m_accountStore.get(m_currentAccountId);
    if (!account.ok()) {
        qWarning() << "Account read failed:" << account.error.message << account.error.detail;
        setStatusText(QStringLiteral("刷新失败：账号读取错误"));
        return;
    }
    m_syncService.syncAccount(*account.value);
}

void AppController::openMessage(MessageId messageId) {
    QVariantMap reader;
    reader.insert(QStringLiteral("id"), messageId);
    const auto header = m_messageStore.header(messageId);
    if (!header.ok()) {
        // R60: the 8-key structure is unchanged; only loading flips off and
        // the error fills in, everything else stays empty.
        reader.insert(QStringLiteral("subject"), QString());
        reader.insert(QStringLiteral("from"), QString());
        reader.insert(QStringLiteral("date"), QDateTime());
        reader.insert(QStringLiteral("loading"), false);
        reader.insert(QStringLiteral("html"), QString());
        reader.insert(QStringLiteral("plainText"), QString());
        reader.insert(QStringLiteral("error"), QStringLiteral("消息不存在"));
        setReader(reader);
        return;
    }

    // Body fields arrive in task 10b; this task only fills the header.
    reader.insert(QStringLiteral("subject"), header.value->subject);
    // Same display rule as FromRole: name when present, address otherwise (R60).
    reader.insert(QStringLiteral("from"), header.value->from.name.isEmpty()
                                              ? header.value->from.address
                                              : header.value->from.name);
    reader.insert(QStringLiteral("date"), header.value->sentAt);
    reader.insert(QStringLiteral("loading"), true);
    reader.insert(QStringLiteral("html"), QString());
    reader.insert(QStringLiteral("plainText"), QString());
    reader.insert(QStringLiteral("error"), QString());
    setReader(reader);
}

void AppController::composeNew() {
    emit composeRequested(QVariantMap{});
}

void AppController::setStatusText(const QString &text) {
    if (m_statusText == text) {
        return;
    }
    m_statusText = text;
    emit statusTextChanged();
}

void AppController::setReader(const QVariantMap &reader) {
    m_reader = reader;
    emit readerChanged();
}

void AppController::clearReader() {
    QVariantMap reader;
    reader.insert(QStringLiteral("id"), static_cast<qint64>(0));
    reader.insert(QStringLiteral("subject"), QString());
    reader.insert(QStringLiteral("from"), QString());
    reader.insert(QStringLiteral("date"), QDateTime());
    reader.insert(QStringLiteral("loading"), false);
    reader.insert(QStringLiteral("html"), QString());
    reader.insert(QStringLiteral("plainText"), QString());
    reader.insert(QStringLiteral("error"), QString());
    setReader(reader);
}

} // namespace ningmail
