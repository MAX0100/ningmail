#include "FakeMailBackend.h"

#include <QDateTime>
#include <QTimeZone>

namespace ningmail {

namespace {

QDateTime seedTimestamp() {
    return QDateTime(QDate(2026, 1, 1), QTime(8, 0, 0), QTimeZone::UTC);
}

MessageHeader makeHeader(const Folder &folder, quint32 uid) {
    MessageHeader header;
    header.id = uid; // fake-internal message id (distinct from the real DB id)
    header.accountId = folder.accountId;
    header.folderId = folder.id;
    header.uid = uid;
    header.rfcMessageId = QString::fromUtf8("<seed-%1@ningmail.test>").arg(uid);
    header.subject = QString::fromUtf8("Seed message %1").arg(uid);
    header.from = {QString::fromUtf8("Seed Sender"),
                   QString::fromUtf8("sender%1@ningmail.test").arg(uid)};
    header.to.append(
        {QString::fromUtf8("Seed Recipient"), QString::fromUtf8("recipient@ningmail.test")});
    header.sentAt = seedTimestamp();
    header.receivedAt = seedTimestamp().addSecs(static_cast<qint64>(uid));
    header.size = 1024 + static_cast<qint64>(uid);
    header.snippet = QString::fromUtf8("Seed snippet %1").arg(uid);
    return header;
}

FakeAccountSeed makeSeed(qint64 accountId) {
    FakeAccountSeed seed;

    Folder inbox;
    inbox.id = 1; // fake-internal folder id, mapped to the real DB id by AppController
    inbox.accountId = accountId;
    inbox.fullName = QString::fromUtf8("INBOX");
    inbox.displayName = QString::fromUtf8("Inbox");
    inbox.role = FolderRole::Inbox;
    inbox.uidValidity = 12345;
    inbox.uidNext = 251;

    Folder sent;
    sent.id = 2; // fake-internal folder id, mapped to the real DB id by AppController
    sent.accountId = accountId;
    sent.fullName = QString::fromUtf8("Sent");
    sent.displayName = QString::fromUtf8("Sent");
    sent.role = FolderRole::Sent;
    sent.uidValidity = 12346;
    sent.uidNext = 1;

    seed.folders.append(inbox);
    seed.folders.append(sent);

    for (quint32 uid = 1; uid <= 250; ++uid) {
        seed.inboxHeaders.append(makeHeader(inbox, uid));
    }
    return seed;
}

} // namespace

FakeMailBackend::FakeMailBackend(std::shared_ptr<QHash<qint64, FakeAccountSeed>> seeds,
                                 qint64 accountId)
    : m_seeds(std::move(seeds)), m_accountId(accountId) {
}

MailError FakeMailBackend::connect(const Account &account, const QString &secret) {
    if (secret != QString::fromUtf8("secret")) {
        return MailError{ErrorCode::Auth, QString::fromUtf8("Invalid credentials"),
                         QString::fromUtf8("fake backend")};
    }
    if (!m_seeds->contains(account.id)) {
        m_seeds->insert(account.id, makeSeed(account.id));
    }
    m_connectedAccountId = account.id;
    return {};
}

Result<QList<Folder>> FakeMailBackend::listFolders() {
    if (!m_connectedAccountId) {
        return Result<QList<Folder>>::failure(MailError{ErrorCode::Storage,
                                                        QString::fromUtf8("Not connected"),
                                                        QString::fromUtf8("not connected")});
    }
    return Result<QList<Folder>>::success(currentSeed().folders);
}

Result<QList<MessageHeader>> FakeMailBackend::fetchHeaders(const Folder &folder, quint32 fromUid,
                                                           int limit) {
    if (!m_connectedAccountId) {
        return Result<QList<MessageHeader>>::failure(MailError{ErrorCode::Storage,
                                                               QString::fromUtf8("Not connected"),
                                                               QString::fromUtf8("not connected")});
    }
    if (limit <= 0) {
        return Result<QList<MessageHeader>>::success(QList<MessageHeader>{});
    }
    if (folder.accountId != m_connectedAccountId) {
        return Result<QList<MessageHeader>>::failure(
            MailError{ErrorCode::Storage, QString::fromUtf8("Folder not found"),
                      QString::fromUtf8("Unknown folder")});
    }
    const FakeAccountSeed &seed = currentSeed();
    const Folder *seedFolder = nullptr;
    for (const Folder &candidate : seed.folders) {
        if (candidate.id == folder.id) {
            seedFolder = &candidate;
            break;
        }
    }
    if (seedFolder == nullptr) {
        return Result<QList<MessageHeader>>::failure(
            MailError{ErrorCode::Storage, QString::fromUtf8("Folder not found"),
                      QString::fromUtf8("Unknown folder")});
    }
    QList<MessageHeader> headers;
    for (const MessageHeader &header : seed.inboxHeaders) {
        if (header.folderId == folder.id && header.uid >= fromUid) {
            headers.append(header);
            if (headers.size() >= limit) {
                break;
            }
        }
    }
    return Result<QList<MessageHeader>>::success(headers);
}

Result<MessageBody> FakeMailBackend::fetchBody(const MessageHeader &header) {
    if (!m_connectedAccountId) {
        return Result<MessageBody>::failure(MailError{ErrorCode::Storage,
                                                      QString::fromUtf8("Not connected"),
                                                      QString::fromUtf8("not connected")});
    }
    for (const MessageHeader &candidate : currentSeed().inboxHeaders) {
        if (candidate.id == header.id) {
            MessageBody body;
            body.messageId = candidate.id;
            body.contentType = QString::fromUtf8("text/plain");
            body.charset = QString::fromUtf8("utf-8");
            body.plainText = QString::fromUtf8("Seed body for uid %1").arg(candidate.uid);
            return Result<MessageBody>::success(body);
        }
    }
    return Result<MessageBody>::failure(MailError{ErrorCode::Storage,
                                                  QString::fromUtf8("Message not found"),
                                                  QString::fromUtf8("Unknown message")});
}

MailError FakeMailBackend::send(const OutgoingMessage &message) {
    if (!m_connectedAccountId) {
        return MailError{ErrorCode::Storage, QString::fromUtf8("Not connected"),
                         QString::fromUtf8("not connected")};
    }
    FakeAccountSeed &seed = currentSeed();
    if (seed.failNextSend) {
        seed.failNextSend = false;
        return MailError{ErrorCode::Network, QString::fromUtf8("Simulated network failure"),
                         QString::fromUtf8("fake backend")};
    }
    seed.sent.append(FakeSentMessage{message, QByteArray()});
    return {};
}

MailError FakeMailBackend::appendSent(const OutgoingMessage &message, const QByteArray &rawMime) {
    if (!m_connectedAccountId) {
        return MailError{ErrorCode::Storage, QString::fromUtf8("Not connected"),
                         QString::fromUtf8("not connected")};
    }
    currentSeed().sent.append(FakeSentMessage{message, rawMime});
    return {};
}

void FakeMailBackend::disconnect() {
    m_connectedAccountId.reset();
}

const FakeAccountSeed &FakeMailBackend::seed() const {
    const auto it = m_seeds->constFind(m_connectedAccountId.value_or(m_accountId));
    Q_ASSERT(it != m_seeds->constEnd());
    return it.value();
}

int FakeMailBackend::sentCount() const {
    return seed().sent.size();
}

const FakeSentMessage &FakeMailBackend::sentRecord(int index) const {
    return seed().sent.at(index);
}

FakeAccountSeed &FakeMailBackend::currentSeed() {
    return (*m_seeds)[m_connectedAccountId.value()];
}

FakeMailBackendFactory::FakeMailBackendFactory()
    : m_seeds(std::make_shared<QHash<qint64, FakeAccountSeed>>()) {
}

std::unique_ptr<MailBackend> FakeMailBackendFactory::create(const Account &account) {
    if (!m_seeds->contains(account.id)) {
        m_seeds->insert(account.id, makeSeed(account.id));
    }
    return std::make_unique<FakeMailBackend>(m_seeds, account.id);
}

FakeAccountSeed &FakeMailBackendFactory::seed(qint64 accountId) {
    if (!m_seeds->contains(accountId)) {
        m_seeds->insert(accountId, makeSeed(accountId));
    }
    return (*m_seeds)[accountId];
}

} // namespace ningmail
