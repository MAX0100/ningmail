#pragma once

#include "MailError.h"

#include <QChar>
#include <QDateTime>
#include <QFlags>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <optional>
#include <utility>

namespace ningmail {

using AccountId = qint64;
using FolderId = qint64;
using MessageId = qint64;

struct MailAddress {
    QString name;
    QString address;
    bool operator==(const MailAddress &) const = default;
};

enum class MailFlag : quint32 {
    None = 0,
    Seen = 1u << 0,
    Flagged = 1u << 1,
    Answered = 1u << 2,
    Draft = 1u << 3,
    Deleted = 1u << 4,
};

Q_DECLARE_FLAGS(MailFlags, MailFlag)

enum class FolderRole { Inbox, Sent, Drafts, Trash, Junk, Archive, Custom };
enum class BodyState { None, Fetching, Ready, Error };
enum class Security { None, StartTls, Tls };
enum class AuthType { Password, OAuth2 };

template <typename T> struct Result {
    std::optional<T> value;
    MailError error;

    bool ok() const {
        return value.has_value();
    }

    static Result success(T v) {
        return Result{std::move(v), MailError{}};
    }

    static Result failure(MailError e) {
        return Result{std::nullopt, std::move(e)};
    }
};

void registerMetaTypes();

struct AttachmentMeta {
    QString filename;
    QString mimeType;
    qint64 size = 0;
    QString contentId;
    bool isInline = false;
};

struct Account {
    AccountId id = 0;
    QString displayName;
    QString email;
    QString imapHost;
    quint16 imapPort = 993;
    Security imapSecurity = Security::Tls;
    QString smtpHost;
    quint16 smtpPort = 465;
    Security smtpSecurity = Security::Tls;
    AuthType authType = AuthType::Password;
    QString username;
    QString credentialRef;
    QString signatureHtml;
    int sortOrder = 0;
};

struct Folder {
    FolderId id = 0;
    AccountId accountId = 0;
    QString fullName;
    QString displayName;
    FolderRole role = FolderRole::Custom;
    QChar delimiter = u'/';
    quint32 uidValidity = 0;
    quint32 uidNext = 0;
    quint64 highestModSeq = 0;
};

struct MessageHeader {
    MessageId id = 0;
    AccountId accountId = 0;
    FolderId folderId = 0;
    quint32 uid = 0;
    QString rfcMessageId;
    QString threadId;
    QString subject;
    MailAddress from;
    QList<MailAddress> to;
    QList<MailAddress> cc;
    QList<MailAddress> bcc;
    QDateTime sentAt;
    QDateTime receivedAt;
    qint64 size = 0;
    MailFlags flags;
    bool hasAttachments = false;
    QString snippet;
    BodyState bodyState = BodyState::None;
};

struct MessageBody {
    MessageId messageId = 0;
    QString contentType;
    QString charset;
    QString html;
    QString plainText;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(MailFlags)

} // namespace ningmail
