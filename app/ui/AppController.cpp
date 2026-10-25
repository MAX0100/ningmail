#include "AppController.h"

#include "CredentialStore.h"
#include "MailtoParser.h"
#include "SendService.h"
#include "Store/DraftStore.h"

#include <QList>
#include <utility>

#include <optional>

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

/// Task 15 (R93⑦): the createAccount defence helpers. QML numbers arrive as
/// doubles and possibly as strings, so both paths go through QVariant first.

/// "tls"/"starttls"/"none" map onto the Security enum; anything else fails
/// validation so a bogus string never reaches the database.
std::optional<Security> securityFrom(const QVariant &value) {
    const QString text = value.toString().trimmed();
    if (text == QLatin1String("tls")) {
        return Security::Tls;
    }
    if (text == QLatin1String("starttls")) {
        return Security::StartTls;
    }
    if (text == QLatin1String("none")) {
        return Security::None;
    }
    return std::nullopt;
}

/// Port values must convert and sit in the 1–65535 range.
bool portInRange(const QVariant &value) {
    bool ok = false;
    const int port = value.toInt(&ok);
    return ok && port >= 1 && port <= 65535;
}

/// Required text fields trim to something non-empty.
bool hasText(const QVariant &value) {
    return !value.toString().trimmed().isEmpty();
}

// R72: plain-text-only bodies are shown as HTML through an escaped, pre-wrapped
// block so spacing survives and the text still wraps; html bodies pass through
// as-is (LiteHtmlRenderer sanitizes again inside the render pipeline, so the
// reader view can carry raw html safely).
void fillReaderBody(QVariantMap &reader, const MessageBody &body) {
    QString html;
    if (!body.html.isEmpty()) {
        html = body.html;
    } else if (!body.plainText.isEmpty()) {
        html = QStringLiteral("<pre style=\"white-space:pre-wrap\">") +
               body.plainText.toHtmlEscaped() + QStringLiteral("</pre>");
    }
    reader.insert(QStringLiteral("html"), html);
    reader.insert(QStringLiteral("plainText"), body.plainText);
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

    // Task 10b: body fetch results. SyncService delivers through its queued
    // postStep, so this slot runs on the GUI thread; a result whose message id
    // no longer matches the open reader is stale and dropped (the store still
    // records a delivered body, errors leave the reader untouched).
    connect(&m_syncService, &SyncService::bodyFetched, this,
            [this](qint64 messageId, const MessageBody &body, const MailError &error) {
                const bool visible = m_reader.value(QStringLiteral("id")).toLongLong() == messageId;
                if (error) {
                    // Persist the failure in the header so reopening cannot
                    // look stuck; the reader shows the error message only when
                    // it belongs to the open message.
                    m_messageStore.setBodyState(messageId, BodyState::Error);
                    if (visible) {
                        QVariantMap reader = m_reader;
                        const QString detail =
                            !error.message.isEmpty() ? error.message : errorLabel(error.code);
                        reader.insert(QStringLiteral("loading"), false);
                        reader.insert(QStringLiteral("error"), detail);
                        setReader(reader);
                    }
                    return;
                }

                const MailError saveError = m_messageStore.saveBody(body);
                if (saveError) {
                    m_messageStore.setBodyState(messageId, BodyState::Error);
                    if (visible) {
                        QVariantMap reader = m_reader;
                        reader.insert(QStringLiteral("loading"), false);
                        reader.insert(QStringLiteral("error"),
                                      QStringLiteral("保存正文失败：") + saveError.message);
                        setReader(reader);
                    }
                    return;
                }
                m_messageStore.setBodyState(messageId, BodyState::Ready);

                if (visible) {
                    QVariantMap reader = m_reader;
                    fillReaderBody(reader, body);
                    reader.insert(QStringLiteral("loading"), false);
                    reader.insert(QStringLiteral("error"), QString());
                    setReader(reader);
                }
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

    reader.insert(QStringLiteral("subject"), header.value->subject);
    // Same display rule as FromRole: name when present, address otherwise (R60).
    reader.insert(QStringLiteral("from"), header.value->from.name.isEmpty()
                                              ? header.value->from.address
                                              : header.value->from.name);
    reader.insert(QStringLiteral("date"), header.value->sentAt);

    // Task 10b: a fetched and stored body is served straight from the store.
    const auto body = m_messageStore.body(messageId);
    const bool bodyReady = header.value->bodyState == BodyState::Ready && body.ok();
    if (bodyReady) {
        fillReaderBody(reader, *body.value);
        reader.insert(QStringLiteral("loading"), false);
        reader.insert(QStringLiteral("error"), QString());
        setReader(reader);
        return;
    }

    // Body miss: keep loading=true and fetch in the background. Errors are
    // delivered through bodyFetched (never silently dropped on the UI).
    reader.insert(QStringLiteral("loading"), true);
    reader.insert(QStringLiteral("html"), QString());
    reader.insert(QStringLiteral("plainText"), QString());
    reader.insert(QStringLiteral("error"), QString());
    setReader(reader);

    // Guard the R62 rule: credentials and account state are only touched on
    // the GUI thread. No account → the reader cannot fetch; surface it
    // immediately instead of waiting for a bodyFetched that never comes.
    if (m_currentAccountId <= 0) {
        QVariantMap stalled = m_reader;
        stalled.insert(QStringLiteral("loading"), false);
        stalled.insert(QStringLiteral("error"), QStringLiteral("未配置账号，无法获取正文"));
        setReader(stalled);
        return;
    }
    const auto account = m_accountStore.get(m_currentAccountId);
    if (!account.ok()) {
        qWarning() << "Account read failed:" << account.error.message << account.error.detail;
        QVariantMap stalled = m_reader;
        stalled.insert(QStringLiteral("loading"), false);
        stalled.insert(QStringLiteral("error"),
                       errorLabel(account.error.code) + QStringLiteral("（获取正文失败）"));
        setReader(stalled);
        return;
    }
    m_messageStore.setBodyState(messageId, BodyState::Fetching);
    m_syncService.fetchBody(*account.value, *header.value);
}

void AppController::composeNew() {
    emit composeRequested(QVariantMap{});
}

// Task 15 (R93④): mailto requests become the draft map consumed by
// Main.qml's openComposer — to/cc are pre-joined into strings because the
// composer's initialTo/initialCc are plain string properties; bcc is carried
// in the map but M1 does not render it (R91-style deferral).
void AppController::openComposer(const MailtoRequest &request) {
    QVariantMap draft;
    draft.insert(QStringLiteral("draftId"), -1);
    draft.insert(QStringLiteral("to"), request.to.join(QStringLiteral(", ")));
    draft.insert(QStringLiteral("cc"), request.cc.join(QStringLiteral(", ")));
    draft.insert(QStringLiteral("bcc"), request.bcc.join(QStringLiteral(", ")));
    draft.insert(QStringLiteral("subject"), request.subject);
    draft.insert(QStringLiteral("body"), request.body);
    emit composeRequested(draft);
}

// Task 15 (R93⑦): Q_INVOKABLE on the GUI thread, so every store and credential
// write below happens synchronously inside this call (R62 family) and the
// password never enters a log or status message.
void AppController::createAccount(const QVariantMap &fields) {
    // Defence first: bogus input never touches the database. The dialog
    // already validated; this mirrors it exactly (R93⑦).
    if (!hasText(fields.value(QStringLiteral("email"))) ||
        !hasText(fields.value(QStringLiteral("imapHost"))) ||
        !hasText(fields.value(QStringLiteral("smtpHost"))) ||
        !portInRange(fields.value(QStringLiteral("imapPort"))) ||
        !portInRange(fields.value(QStringLiteral("smtpPort")))) {
        setStatusText(QStringLiteral("请检查输入"));
        return;
    }
    const std::optional<Security> imapSecurity =
        securityFrom(fields.value(QStringLiteral("imapSecurity")));
    const std::optional<Security> smtpSecurity =
        securityFrom(fields.value(QStringLiteral("smtpSecurity")));
    if (!imapSecurity.has_value() || !smtpSecurity.has_value()) {
        setStatusText(QStringLiteral("请检查输入"));
        return;
    }

    Account account;
    account.displayName = fields.value(QStringLiteral("displayName")).toString().trimmed();
    account.email = fields.value(QStringLiteral("email")).toString().trimmed();
    account.imapHost = fields.value(QStringLiteral("imapHost")).toString().trimmed();
    account.imapPort = static_cast<quint16>(fields.value(QStringLiteral("imapPort")).toInt());
    account.imapSecurity = *imapSecurity;
    account.smtpHost = fields.value(QStringLiteral("smtpHost")).toString().trimmed();
    account.smtpPort = static_cast<quint16>(fields.value(QStringLiteral("smtpPort")).toInt());
    account.smtpSecurity = *smtpSecurity;
    // An empty username falls back to the email address.
    account.username = fields.value(QStringLiteral("username")).toString().trimmed();
    if (account.username.isEmpty()) {
        account.username = account.email;
    }
    const QString password = fields.value(QStringLiteral("password")).toString();

    // Step 1 of the two-step credentialRef contract: create the row, then
    // update it with its own id-derived ref (no store extension needed).
    const auto created = m_accountStore.create(account);
    if (!created.ok()) {
        qWarning() << "Account create failed:" << created.error.message << created.error.detail;
        setStatusText(QStringLiteral("账号保存失败"));
        return;
    }
    account.id = *created.value;
    account.credentialRef = QStringLiteral("account-%1").arg(account.id);
    if (const MailError updated = m_accountStore.update(account); updated) {
        qWarning() << "Account update failed:" << updated.message << updated.detail;
        // R94③: a clean rollback only reaches debug level; a failed rollback
        // is a warning carrying both error texts (the row may have survived).
        if (const MailError rolledBack = m_accountStore.remove(account.id); rolledBack) {
            qWarning() << "Account ROLLBACK FAILED for account" << account.id
                       << "- update:" << updated.message << updated.detail
                       << "- remove:" << rolledBack.message << rolledBack.detail;
        } else {
            qDebug() << "Rolled back account" << account.id << "after a failed update";
        }
        setStatusText(QStringLiteral("账号保存失败"));
        return;
    }

    // Step 2: the secret. A keyring failure rolls the account row back again.
    if (!m_credentials.write(account.credentialRef, password)) {
        qWarning() << "Credential write failed for account" << account.id;
        // Same R94③ split as above.
        if (const MailError rolledBack = m_accountStore.remove(account.id); rolledBack) {
            qWarning() << "Account ROLLBACK FAILED for account" << account.id
                       << "- credential write failed and removal also failed - remove:"
                       << rolledBack.message << rolledBack.detail;
        } else {
            qDebug() << "Rolled back account" << account.id << "after a failed credential write";
        }
        setStatusText(QStringLiteral("凭据写入失败"));
        return;
    }

    setStatusText(m_credentials.isPersistent() ? QStringLiteral("账号已添加")
                                               : QStringLiteral("密码仅本次会话有效"));

    // Follow-up wiring unchanged from initialize()/refresh(): the new account
    // becomes current, its (empty) folder list reloads, and the sync starts.
    m_currentAccountId = account.id;
    m_folders->reload(account.id);
    m_messages->reload();
    m_syncService.syncAccount(account);
}

void AppController::setDraftStore(DraftStore *store) {
    m_draftStore = store;
}

void AppController::setSendService(SendService *service) {
    if (m_sendService != nullptr) {
        // First injection wins; the itemStateChanged connection below is
        // made exactly once.
        return;
    }
    m_sendService = service;
    // Queued steps from SendService already arrive on the GUI thread; the
    // connection only maps the final states into user-visible status text.
    connect(service, &SendService::itemStateChanged, this,
            [this](qint64 outboxId, OutboxState state, const QString &error) {
                Q_UNUSED(outboxId);
                switch (state) {
                case OutboxState::Sent:
                    setStatusText(QStringLiteral("已发送"));
                    break;
                case OutboxState::Retry:
                case OutboxState::Failed: {
                    // The stored error text (backend message or wiring hint)
                    // travels with the failure hint.
                    setStatusText(error.isEmpty() ? QStringLiteral("发送失败")
                                                  : QStringLiteral("发送失败：") + error);
                    break;
                }
                case OutboxState::Pending:
                case OutboxState::Sending:
                    // Transient states stay invisible; the final delivery
                    // decides the status line.
                    break;
                }
            });
}

qint64 AppController::saveDraft(qint64 draftId, const QStringList &to, const QStringList &cc,
                                const QString &subject, const QString &body) {
    if (m_currentAccountId <= 0) {
        setStatusText(QStringLiteral("请先添加账号"));
        return -1;
    }
    if (m_draftStore == nullptr) {
        setStatusText(QStringLiteral("草稿保存未启用"));
        return -1;
    }

    Draft draft;
    draft.id = draftId;
    draft.accountId = m_currentAccountId;
    draft.subject = subject;
    draft.bodyText = body;
    for (const QString &address : to) {
        draft.recipients.append(MailAddress{QString(), address});
    }
    for (const QString &address : cc) {
        draft.cc.append(MailAddress{QString(), address});
    }

    const auto saved = m_draftStore->save(draft);
    if (!saved.ok()) {
        qWarning() << "Draft save failed:" << saved.error.message << saved.error.detail;
        setStatusText(QStringLiteral("草稿保存失败：") + saved.error.message);
        return -1;
    }
    setStatusText(QStringLiteral("草稿已保存"));
    return *saved.value;
}

void AppController::sendMessage(const QStringList &to, const QStringList &cc,
                                const QString &subject, const QString &body) {
    if (m_currentAccountId <= 0) {
        // Same guard order as saveDraft: the account question comes first.
        setStatusText(QStringLiteral("请先添加账号"));
        return;
    }
    if (m_sendService == nullptr) {
        setStatusText(QStringLiteral("发送服务未启用"));
        return;
    }
    // T11 kept the parameters part of the stable call contract; task 12 turns
    // them into the OutgoingMessage the outbox (bcc intentionally left empty)
    // stores and sends.
    const auto account = m_accountStore.get(m_currentAccountId);
    if (!account.ok()) {
        qWarning() << "Account read failed:" << account.error.message << account.error.detail;
        setStatusText(QStringLiteral("发送失败：账号读取错误"));
        return;
    }

    OutgoingMessage message;
    message.account = *account.value;
    for (const QString &address : to) {
        message.to.append(MailAddress{QString(), address});
    }
    for (const QString &address : cc) {
        message.cc.append(MailAddress{QString(), address});
    }
    message.subject = subject;
    message.bodyText = body;

    const qint64 outboxId = m_sendService->enqueue(message);
    if (outboxId <= 0) {
        qWarning() << "Send enqueue failed for account" << m_currentAccountId;
        setStatusText(QStringLiteral("发送失败：入队失败"));
        return;
    }
    // The itemStateChanged handler inside setSendService carries the outcome
    // (已发送 / 发送失败…) into statusText once the worker reports back.
    m_sendService->sendNow(outboxId);
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
