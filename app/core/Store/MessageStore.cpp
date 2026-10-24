#include "Store/MessageStore.h"

#include "Store/Database.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ningmail {

namespace {

/// Column order matches the `messages` table (schema v1); created_at is
/// write-only bookkeeping and is not mapped back into MessageHeader.
const char *kHeaderColumns =
    "id, account_id, folder_id, uid, rfc_message_id, thread_id, subject, from_name, from_addr, "
    "to_addrs, cc_addrs, bcc_addrs, sent_at, received_at, size, flags, has_attachments, snippet, "
    "body_state";

/// Address lists are stored as JSON array text `[{"name":"...","address":"..."}]`;
/// an empty list serializes to `[]`.
QString addressesToJson(const QList<MailAddress> &addresses) {
    QJsonArray array;
    for (const MailAddress &address : addresses) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), address.name);
        object.insert(QStringLiteral("address"), address.address);
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QList<MailAddress> addressesFromJson(const QString &text) {
    QList<MailAddress> addresses;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isArray()) {
        return addresses;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        MailAddress address;
        address.name = object.value(QLatin1String("name")).toString();
        address.address = object.value(QLatin1String("address")).toString();
        addresses.append(address);
    }
    return addresses;
}

MessageHeader headerFromQuery(const QSqlQuery &query) {
    MessageHeader header;
    header.id = query.value(0).toLongLong();
    header.accountId = query.value(1).toLongLong();
    header.folderId = query.value(2).toLongLong();
    header.uid = static_cast<quint32>(query.value(3).toULongLong());
    header.rfcMessageId = query.value(4).toString();
    header.threadId = query.value(5).toString();
    header.subject = query.value(6).toString();
    header.from.name = query.value(7).toString();
    header.from.address = query.value(8).toString();
    header.to = addressesFromJson(query.value(9).toString());
    header.cc = addressesFromJson(query.value(10).toString());
    header.bcc = addressesFromJson(query.value(11).toString());
    header.sentAt = QDateTime::fromMSecsSinceEpoch(query.value(12).toLongLong());
    header.receivedAt = QDateTime::fromMSecsSinceEpoch(query.value(13).toLongLong());
    header.size = query.value(14).toLongLong();
    header.flags = MailFlags{static_cast<MailFlag>(query.value(15).toULongLong())};
    header.hasAttachments = query.value(16).toInt() != 0;
    header.snippet = query.value(17).toString();
    header.bodyState = static_cast<BodyState>(query.value(18).toInt());
    return header;
}

MailError storageError(const char *context, const QSqlError &error) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Message store %1 failed").arg(QLatin1String(context)),
                     error.text()};
}

MailError notFound(const char *what, qint64 id) {
    return MailError{ErrorCode::Storage, QStringLiteral("%1 not found").arg(QLatin1String(what)),
                     QStringLiteral("No messages row with id %1").arg(id)};
}

/// True when the last SQLite error on `error` is a real failure, not "no rows".
bool isError(const QSqlError &error) {
    return error.isValid() && error.type() != QSqlError::NoError;
}

/// count()/unreadCount() cannot surface MailError (int signature, fixed by the
/// brief). On M1 the database is a single-connection local file, so an SQL
/// failure means a globally broken session; returning 0 is the forced contract,
/// and this warning is the only observability of that failure.
void reportSilentCountFailure(const char *context, const QSqlError &error) {
    const MailError failure = storageError(context, error);
    qWarning() << failure.message << failure.detail;
}

/// NOT NULL TEXT columns must never receive a NULL bind, which a default
/// (null) QString would produce; an empty string matches the schema defaults.
QString notNullText(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

/// Invalid timestamps (unset sentAt) are stored as epoch zero so the
/// NOT NULL column never sees garbage values.
qint64 nullableMillis(const QDateTime &time) {
    return time.isValid() ? time.toMSecsSinceEpoch() : 0;
}

/// Binds the insert columns of `header` (everything except id).
void bindHeaderValues(QSqlQuery &query, const MessageHeader &header) {
    query.bindValue(QStringLiteral(":account_id"), header.accountId);
    query.bindValue(QStringLiteral(":folder_id"), header.folderId);
    query.bindValue(QStringLiteral(":uid"), static_cast<qint64>(header.uid));
    query.bindValue(QStringLiteral(":rfc_message_id"), notNullText(header.rfcMessageId));
    query.bindValue(QStringLiteral(":thread_id"), notNullText(header.threadId));
    query.bindValue(QStringLiteral(":subject"), notNullText(header.subject));
    query.bindValue(QStringLiteral(":from_name"), notNullText(header.from.name));
    query.bindValue(QStringLiteral(":from_addr"), notNullText(header.from.address));
    query.bindValue(QStringLiteral(":to_addrs"), addressesToJson(header.to));
    query.bindValue(QStringLiteral(":cc_addrs"), addressesToJson(header.cc));
    query.bindValue(QStringLiteral(":bcc_addrs"), addressesToJson(header.bcc));
    query.bindValue(QStringLiteral(":sent_at"), nullableMillis(header.sentAt));
    // Schema v1 has no distinct received_at semantics; the column mirrors sentAt.
    query.bindValue(QStringLiteral(":received_at"), nullableMillis(header.sentAt));
    query.bindValue(QStringLiteral(":size"), header.size);
    query.bindValue(QStringLiteral(":flags"), static_cast<qint64>(header.flags.toInt()));
    query.bindValue(QStringLiteral(":has_attachments"), header.hasAttachments ? 1 : 0);
    query.bindValue(QStringLiteral(":snippet"), notNullText(header.snippet));
    query.bindValue(QStringLiteral(":body_state"), static_cast<int>(header.bodyState));
    query.bindValue(QStringLiteral(":created_at"),
                    QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
}

/// Upsert shared by insert() and insertBatch(); runs inside the caller's
/// transaction. Conflicts on UNIQUE(folder_id, uid) DO NOTHING; the existing
/// row id is read back and its mutable fields are refreshed.
MailError upsertHeader(QSqlDatabase &db, const MessageHeader &header, qint64 &outId) {
    outId = 0;
    qint64 existingId = 0;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "INSERT INTO messages (account_id, folder_id, uid, rfc_message_id, thread_id,"
                " subject, from_name, from_addr, to_addrs, cc_addrs, bcc_addrs, sent_at,"
                " received_at, size, flags, has_attachments, snippet, body_state, created_at)"
                " VALUES (:account_id, :folder_id, :uid, :rfc_message_id, :thread_id, :subject,"
                " :from_name, :from_addr, :to_addrs, :cc_addrs, :bcc_addrs, :sent_at,"
                " :received_at, :size, :flags, :has_attachments, :snippet, :body_state,"
                " :created_at) ON CONFLICT(folder_id, uid) DO NOTHING"))) {
            return storageError("prepare", query.lastError());
        }
        bindHeaderValues(query, header);
        if (!query.exec()) {
            return storageError("insert", query.lastError());
        }
        if (query.numRowsAffected() > 0) {
            const QVariant lastInsertId = query.lastInsertId();
            if (!lastInsertId.isValid() || lastInsertId.toLongLong() <= 0) {
                return MailError{ErrorCode::Storage, QStringLiteral("Insert returned no id"),
                                 QStringLiteral("messages INSERT for uid %1").arg(header.uid)};
            }
            outId = lastInsertId.toLongLong();
            return MailError{};
        }
    }

    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "SELECT id FROM messages WHERE folder_id = :folder_id AND uid = :uid"))) {
            return storageError("prepare", query.lastError());
        }
        query.bindValue(QStringLiteral(":folder_id"), header.folderId);
        query.bindValue(QStringLiteral(":uid"), static_cast<qint64>(header.uid));
        if (!query.exec()) {
            return storageError("select", query.lastError());
        }
        if (!query.next()) {
            return MailError{ErrorCode::Storage, QStringLiteral("Message not found"),
                             QStringLiteral("No messages row with folder_id %1 and uid %2")
                                 .arg(header.folderId)
                                 .arg(header.uid)};
        }
        existingId = query.value(0).toLongLong();
    }

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral(
            "UPDATE messages SET flags = :flags, subject = :subject, snippet = :snippet,"
            " body_state = :body_state WHERE id = :id"))) {
        return storageError("prepare", query.lastError());
    }
    query.bindValue(QStringLiteral(":flags"), static_cast<qint64>(header.flags.toInt()));
    query.bindValue(QStringLiteral(":subject"), notNullText(header.subject));
    query.bindValue(QStringLiteral(":snippet"), notNullText(header.snippet));
    query.bindValue(QStringLiteral(":body_state"), static_cast<int>(header.bodyState));
    query.bindValue(QStringLiteral(":id"), existingId);
    if (!query.exec()) {
        return storageError("update", query.lastError());
    }
    outId = existingId;
    return MailError{};
}

} // namespace

MessageStore::MessageStore(Database &database) : m_database(database) {
}

Result<MessageId> MessageStore::insert(const MessageHeader &header) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return Result<MessageId>::failure(storageError("begin transaction", db.lastError()));
    }

    qint64 messageId = 0;
    const MailError error = upsertHeader(db, header, messageId);
    if (error) {
        db.rollback();
        return Result<MessageId>::failure(error);
    }

    if (!db.commit()) {
        const QSqlError queryError = db.lastError();
        db.rollback();
        return Result<MessageId>::failure(storageError("commit", queryError));
    }
    return Result<MessageId>::success(messageId);
}

MailError MessageStore::insertBatch(const QList<MessageHeader> &headers) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    for (const MessageHeader &header : headers) {
        qint64 messageId = 0;
        const MailError error = upsertHeader(db, header, messageId);
        if (error) {
            db.rollback();
            return error;
        }
    }

    if (!db.commit()) {
        const QSqlError queryError = db.lastError();
        db.rollback();
        return storageError("commit", queryError);
    }
    return MailError{};
}

MailError MessageStore::updateFlags(MessageId id, MailFlags flags) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowUpdated = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("UPDATE messages SET flags = :flags WHERE id = :id"))) {
            queryError = query.lastError();
        } else {
            query.bindValue(QStringLiteral(":flags"), static_cast<qint64>(flags.toInt()));
            query.bindValue(QStringLiteral(":id"), id);
            if (!query.exec()) {
                queryError = query.lastError();
            } else {
                rowUpdated = query.numRowsAffected() == 1;
            }
        }
    }

    if (!rowUpdated) {
        db.rollback();
        if (isError(queryError)) {
            return storageError("update", queryError);
        }
        return notFound("Message", id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

MailError MessageStore::setBodyState(MessageId id, BodyState state) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowUpdated = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(
                QStringLiteral("UPDATE messages SET body_state = :body_state WHERE id = :id"))) {
            queryError = query.lastError();
        } else {
            query.bindValue(QStringLiteral(":body_state"), static_cast<int>(state));
            query.bindValue(QStringLiteral(":id"), id);
            if (!query.exec()) {
                queryError = query.lastError();
            } else {
                rowUpdated = query.numRowsAffected() == 1;
            }
        }
    }

    if (!rowUpdated) {
        db.rollback();
        if (isError(queryError)) {
            return storageError("update", queryError);
        }
        return notFound("Message", id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

Result<MessageHeader> MessageStore::header(MessageId id) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = :id")
                           .arg(QLatin1String(kHeaderColumns)))) {
        return Result<MessageHeader>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return Result<MessageHeader>::failure(storageError("select", query.lastError()));
    }
    if (!query.next()) {
        return Result<MessageHeader>::failure(notFound("Message", id));
    }
    return Result<MessageHeader>::success(headerFromQuery(query));
}

Result<QList<MessageHeader>> MessageStore::list(FolderId folderId, int limit,
                                                std::optional<MessageCursor> before) const {
    if (limit <= 0) {
        return Result<QList<MessageHeader>>::success(QList<MessageHeader>{});
    }

    QString sql = QStringLiteral("SELECT %1 FROM messages WHERE folder_id = :folder_id")
                      .arg(QLatin1String(kHeaderColumns));
    if (before.has_value()) {
        sql += QStringLiteral(" AND (sent_at < :sent_at OR (sent_at = :sent_at AND id < :id))");
    }
    sql += QStringLiteral(" ORDER BY sent_at DESC, id DESC LIMIT :limit");

    QSqlQuery query(m_database.connection());
    if (!query.prepare(sql)) {
        return Result<QList<MessageHeader>>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":folder_id"), folderId);
    if (before.has_value()) {
        query.bindValue(QStringLiteral(":sent_at"), before->sentAtMs);
        query.bindValue(QStringLiteral(":id"), before->id);
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        return Result<QList<MessageHeader>>::failure(storageError("select", query.lastError()));
    }

    QList<MessageHeader> messages;
    while (query.next()) {
        messages.append(headerFromQuery(query));
    }
    return Result<QList<MessageHeader>>::success(std::move(messages));
}

int MessageStore::count(FolderId folderId) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(
            QStringLiteral("SELECT COUNT(*) FROM messages WHERE folder_id = :folder_id"))) {
        reportSilentCountFailure("count", query.lastError());
        return 0;
    }
    query.bindValue(QStringLiteral(":folder_id"), folderId);
    if (!query.exec() || !query.next()) {
        reportSilentCountFailure("count", query.lastError());
        return 0;
    }
    return query.value(0).toInt();
}

int MessageStore::unreadCount(FolderId folderId) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT COUNT(*) FROM messages WHERE folder_id = :folder_id "
                                      "AND (flags & :seen) = 0"))) {
        reportSilentCountFailure("unread count", query.lastError());
        return 0;
    }
    query.bindValue(QStringLiteral(":folder_id"), folderId);
    query.bindValue(QStringLiteral(":seen"), static_cast<int>(MailFlag::Seen));
    if (!query.exec() || !query.next()) {
        reportSilentCountFailure("unread count", query.lastError());
        return 0;
    }
    return query.value(0).toInt();
}

MailError MessageStore::saveBody(const MessageBody &body) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral(
            "INSERT INTO bodies (message_pk, content_type, charset, html_raw, text_plain,"
            " fetched_at, size) VALUES (:message_pk, :content_type, :charset, :html_raw,"
            " :text_plain, :fetched_at, :size) ON CONFLICT(message_pk) DO UPDATE SET"
            " content_type = excluded.content_type, charset = excluded.charset,"
            " html_raw = excluded.html_raw, text_plain = excluded.text_plain,"
            " fetched_at = excluded.fetched_at, size = excluded.size"))) {
        const QSqlError error = query.lastError();
        db.rollback();
        return storageError("prepare", error);
    }
    const qint64 fetchedAt = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const qint64 size = body.html.toUtf8().size() + body.plainText.toUtf8().size();
    query.bindValue(QStringLiteral(":message_pk"), body.messageId);
    query.bindValue(QStringLiteral(":content_type"), notNullText(body.contentType));
    query.bindValue(QStringLiteral(":charset"), notNullText(body.charset));
    query.bindValue(QStringLiteral(":html_raw"), notNullText(body.html));
    query.bindValue(QStringLiteral(":text_plain"), notNullText(body.plainText));
    query.bindValue(QStringLiteral(":fetched_at"), fetchedAt);
    query.bindValue(QStringLiteral(":size"), size);
    if (!query.exec()) {
        const QSqlError error = query.lastError();
        db.rollback();
        return storageError("upsert body", error);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

Result<MessageBody> MessageStore::body(MessageId id) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(
            QStringLiteral("SELECT content_type, charset, html_raw, text_plain FROM bodies"
                           " WHERE message_pk = :message_pk"))) {
        return Result<MessageBody>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":message_pk"), id);
    if (!query.exec()) {
        return Result<MessageBody>::failure(storageError("select", query.lastError()));
    }
    if (!query.next()) {
        // No fetched body yet: an empty body is the success contract.
        MessageBody body;
        body.messageId = id;
        return Result<MessageBody>::success(body);
    }
    MessageBody body;
    body.messageId = id;
    body.contentType = query.value(0).toString();
    body.charset = query.value(1).toString();
    body.html = query.value(2).toString();
    body.plainText = query.value(3).toString();
    return Result<MessageBody>::success(body);
}

MailError MessageStore::clearFolder(FolderId folderId) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral("DELETE FROM messages WHERE folder_id = :folder_id"))) {
        const QSqlError error = query.lastError();
        db.rollback();
        return storageError("prepare", error);
    }
    query.bindValue(QStringLiteral(":folder_id"), folderId);
    if (!query.exec()) {
        const QSqlError error = query.lastError();
        db.rollback();
        return storageError("delete", error);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

} // namespace ningmail
