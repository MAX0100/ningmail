#pragma once

#include "Types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace ningmail {

struct OutgoingMessage {
    Account account;
    QList<MailAddress> to;
    QList<MailAddress> cc;
    QList<MailAddress> bcc;
    QString subject;
    QString bodyText;
    QList<AttachmentMeta> attachments;
    QString inReplyTo;
    QStringList references;
    QString rfcMessageId;
};

class MailBackend {
  public:
    virtual ~MailBackend() = default;

    virtual MailError connect(const Account &account, const QString &secret) = 0;
    virtual Result<QList<Folder>> listFolders() = 0;
    // Returns the first headers of `folder` with uid >= fromUid, capped at `limit`.
    // A limit <= 0 succeeds with an empty list; it never widens or defaults the count.
    virtual Result<QList<MessageHeader>> fetchHeaders(const Folder &folder, quint32 fromUid,
                                                      int limit) = 0;
    virtual Result<MessageBody> fetchBody(const MessageHeader &header) = 0;
    virtual MailError send(const OutgoingMessage &message) = 0;
    virtual MailError appendSent(const OutgoingMessage &message, const QByteArray &rawMime) = 0;
    virtual void disconnect() = 0;
};

class MailBackendFactory {
  public:
    virtual ~MailBackendFactory() = default;

    virtual std::unique_ptr<MailBackend> create(const Account &account) = 0;
};

} // namespace ningmail
