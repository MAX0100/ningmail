#include "Store/OutboxStore.h"

#include "Store/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTimeZone>

namespace ningmail {

namespace {

/// Column order matches the `outbox` table (schema v1).
const char *kOutboxColumns =
    "id, account_id, recipients_json, cc_json, subject, body_text, raw_mime, state,"
    " attempts, next_retry_at, last_error, rfc_message_id, created_at";

// Address lists use the exact same JSON format as MessageStore.cpp
// (`[{"name":"...","address":"..."}]`, empty list -> `[]`); the helper bodies
// must stay character-identical to its addressesToJson/addressesFromJson so
// any consumer of one table can read the other's rows.
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

OutboxItem outboxItemFromQuery(const QSqlQuery &query) {
    OutboxItem item;
    item.id = query.value(0).toLongLong();
    item.accountId = query.value(1).toLongLong();
    item.recipients = addressesFromJson(query.value(2).toString());
    item.cc = addressesFromJson(query.value(3).toString());
    item.subject = query.value(4).toString();
    item.bodyText = query.value(5).toString();
    item.rawMime = query.value(6).toByteArray();
    item.state = static_cast<OutboxState>(query.value(7).toInt());
    item.attempts = query.value(8).toInt();
    const QVariant nextRetryAtMs = query.value(9);
    if (nextRetryAtMs.isValid() && !nextRetryAtMs.isNull()) {
        item.nextRetryAt =
            QDateTime::fromMSecsSinceEpoch(nextRetryAtMs.toLongLong(), QTimeZone::UTC);
    }
    item.lastError = query.value(10).toString();
    item.rfcMessageId = query.value(11).toString();
    return item;
}

MailError storageError(const char *context, const QSqlError &error) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Outbox store %1 failed").arg(QLatin1String(context)),
                     error.text()};
}

MailError notFound(const char *what, qint64 id) {
    return MailError{ErrorCode::Storage, QStringLiteral("%1 not found").arg(QLatin1String(what)),
                     QStringLiteral("No outbox row with id %1").arg(id)};
}

/// NOT NULL TEXT columns must never receive a NULL bind, which a default
/// (null) QString would produce; an empty string matches the schema defaults.
QString notNullText(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

} // namespace

OutboxStore::OutboxStore(Database &database) : m_database(database) {
}

Result<qint64> OutboxStore::save(const OutboxItem &item) {
    // created_at is store-owned bookkeeping (see DraftStore::save, R29): the
    // current UTC time in milliseconds; a caller-supplied value is ignored.
    const qint64 createdAtMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return Result<qint64>::failure(storageError("begin transaction", db.lastError()));
    }

    MailError error;
    qint64 itemId = 0;
    if (item.id <= 0) {
        QSqlQuery query(db);
        if (!query.prepare(
                QStringLiteral("INSERT INTO outbox (account_id, recipients_json, cc_json, subject,"
                               " body_text, raw_mime, state, attempts, next_retry_at, last_error,"
                               " rfc_message_id, created_at) VALUES (:account_id, :recipients_json,"
                               " :cc_json, :subject, :body_text, :raw_mime, :state, :attempts,"
                               " :next_retry_at, :last_error, :rfc_message_id, :created_at)"))) {
            error = storageError("prepare", query.lastError());
        }
        if (!error) {
            query.bindValue(QStringLiteral(":account_id"), item.accountId);
            query.bindValue(QStringLiteral(":recipients_json"), addressesToJson(item.recipients));
            query.bindValue(QStringLiteral(":cc_json"), addressesToJson(item.cc));
            query.bindValue(QStringLiteral(":subject"), notNullText(item.subject));
            query.bindValue(QStringLiteral(":body_text"), notNullText(item.bodyText));
            query.bindValue(QStringLiteral(":raw_mime"), item.rawMime);
            query.bindValue(QStringLiteral(":state"), static_cast<int>(item.state));
            query.bindValue(QStringLiteral(":attempts"), item.attempts);
            query.bindValue(QStringLiteral(":next_retry_at"),
                            item.nextRetryAt.isValid()
                                ? QVariant(item.nextRetryAt.toMSecsSinceEpoch())
                                : QVariant());
            query.bindValue(QStringLiteral(":last_error"), notNullText(item.lastError));
            query.bindValue(QStringLiteral(":rfc_message_id"), notNullText(item.rfcMessageId));
            query.bindValue(QStringLiteral(":created_at"), createdAtMs);
            if (!query.exec()) {
                // A missing account row violates the foreign key here.
                error = storageError("insert", query.lastError());
            } else {
                const QVariant lastInsertId = query.lastInsertId();
                if (!lastInsertId.isValid() || lastInsertId.toLongLong() <= 0) {
                    error = MailError{
                        ErrorCode::Storage, QStringLiteral("Insert returned no id"),
                        QStringLiteral("outbox INSERT for account %1").arg(item.accountId)};
                } else {
                    itemId = lastInsertId.toLongLong();
                }
            }
        }
    } else {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "UPDATE outbox SET account_id = :account_id, recipients_json ="
                " :recipients_json, cc_json = :cc_json, subject = :subject, body_text ="
                " :body_text, raw_mime = :raw_mime, state = :state, attempts = :attempts,"
                " next_retry_at = :next_retry_at, last_error = :last_error, rfc_message_id ="
                " :rfc_message_id, created_at = :created_at WHERE id = :id"))) {
            error = storageError("prepare", query.lastError());
        }
        if (!error) {
            query.bindValue(QStringLiteral(":account_id"), item.accountId);
            query.bindValue(QStringLiteral(":recipients_json"), addressesToJson(item.recipients));
            query.bindValue(QStringLiteral(":cc_json"), addressesToJson(item.cc));
            query.bindValue(QStringLiteral(":subject"), notNullText(item.subject));
            query.bindValue(QStringLiteral(":body_text"), notNullText(item.bodyText));
            query.bindValue(QStringLiteral(":raw_mime"), item.rawMime);
            query.bindValue(QStringLiteral(":state"), static_cast<int>(item.state));
            query.bindValue(QStringLiteral(":attempts"), item.attempts);
            query.bindValue(QStringLiteral(":next_retry_at"),
                            item.nextRetryAt.isValid()
                                ? QVariant(item.nextRetryAt.toMSecsSinceEpoch())
                                : QVariant());
            query.bindValue(QStringLiteral(":last_error"), notNullText(item.lastError));
            query.bindValue(QStringLiteral(":rfc_message_id"), notNullText(item.rfcMessageId));
            query.bindValue(QStringLiteral(":created_at"), createdAtMs);
            query.bindValue(QStringLiteral(":id"), item.id);
            if (!query.exec()) {
                error = storageError("update", query.lastError());
            } else if (query.numRowsAffected() != 1) {
                // A missing id must fail, never silently become an insert.
                error = notFound("Outbox item", item.id);
            } else {
                itemId = item.id;
            }
        }
    }

    if (error) {
        db.rollback();
        return Result<qint64>::failure(error);
    }

    if (!db.commit()) {
        const QSqlError queryError = db.lastError();
        db.rollback();
        return Result<qint64>::failure(storageError("commit", queryError));
    }
    return Result<qint64>::success(itemId);
}

Result<OutboxItem> OutboxStore::get(qint64 id) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM outbox WHERE id = :id")
                           .arg(QLatin1String(kOutboxColumns)))) {
        return Result<OutboxItem>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return Result<OutboxItem>::failure(storageError("select", query.lastError()));
    }
    if (!query.next()) {
        return Result<OutboxItem>::failure(notFound("Outbox item", id));
    }
    return Result<OutboxItem>::success(outboxItemFromQuery(query));
}

Result<QList<OutboxItem>> OutboxStore::listPending(AccountId accountId) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(
            QStringLiteral(
                "SELECT %1 FROM outbox WHERE account_id = :account_id AND state IN (:pending,"
                " :retry) ORDER BY created_at ASC, id ASC")
                .arg(QLatin1String(kOutboxColumns)))) {
        return Result<QList<OutboxItem>>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":account_id"), accountId);
    query.bindValue(QStringLiteral(":pending"), static_cast<int>(OutboxState::Pending));
    query.bindValue(QStringLiteral(":retry"), static_cast<int>(OutboxState::Retry));
    if (!query.exec()) {
        return Result<QList<OutboxItem>>::failure(storageError("select", query.lastError()));
    }

    QList<OutboxItem> items;
    while (query.next()) {
        items.append(outboxItemFromQuery(query));
    }
    return Result<QList<OutboxItem>>::success(std::move(items));
}

MailError OutboxStore::updateState(qint64 id, OutboxState state, const QString &error) {
    QSqlQuery query(m_database.connection());
    // `error` is the final value of last_error: an empty input clears the
    // column (the NOT NULL default keeps the bind as "").
    if (!query.prepare(QStringLiteral(
            "UPDATE outbox SET state = :state, last_error = :last_error WHERE id = :id"))) {
        return storageError("prepare", query.lastError());
    }
    query.bindValue(QStringLiteral(":state"), static_cast<int>(state));
    query.bindValue(QStringLiteral(":last_error"), notNullText(error));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return storageError("update", query.lastError());
    }
    if (query.numRowsAffected() != 1) {
        return notFound("Outbox item", id);
    }
    return MailError{};
}

MailError OutboxStore::bumpAttempts(qint64 id) {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(
            QStringLiteral("UPDATE outbox SET attempts = attempts + 1 WHERE id = :id"))) {
        return storageError("prepare", query.lastError());
    }
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return storageError("update", query.lastError());
    }
    if (query.numRowsAffected() != 1) {
        return notFound("Outbox item", id);
    }
    return MailError{};
}

MailError OutboxStore::remove(qint64 id) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowDeleted = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("DELETE FROM outbox WHERE id = :id"))) {
            queryError = query.lastError();
        } else {
            query.bindValue(QStringLiteral(":id"), id);
            if (!query.exec()) {
                queryError = query.lastError();
            } else {
                rowDeleted = query.numRowsAffected() == 1;
            }
        }
    }

    if (!rowDeleted) {
        db.rollback();
        if (queryError.isValid() && queryError.type() != QSqlError::NoError) {
            return storageError("delete", queryError);
        }
        return notFound("Outbox item", id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

} // namespace ningmail
