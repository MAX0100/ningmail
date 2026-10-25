#include "Store/DraftStore.h"

#include "Store/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

namespace ningmail {

namespace {

/// Column order matches the `drafts` table (schema v1).
const char *kDraftColumns =
    "id, account_id, subject, body_text, recipients_json, cc_json, updated_at";

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

Draft draftFromQuery(const QSqlQuery &query) {
    Draft draft;
    draft.id = query.value(0).toLongLong();
    draft.accountId = query.value(1).toLongLong();
    draft.subject = query.value(2).toString();
    draft.bodyText = query.value(3).toString();
    draft.recipients = addressesFromJson(query.value(4).toString());
    draft.cc = addressesFromJson(query.value(5).toString());
    draft.updatedAt = query.value(6).toLongLong();
    return draft;
}

MailError storageError(const char *context, const QSqlError &error) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Draft store %1 failed").arg(QLatin1String(context)),
                     error.text()};
}

MailError notFound(const char *what, qint64 id) {
    return MailError{ErrorCode::Storage, QStringLiteral("%1 not found").arg(QLatin1String(what)),
                     QStringLiteral("No drafts row with id %1").arg(id)};
}

/// NOT NULL TEXT columns must never receive a NULL bind, which a default
/// (null) QString would produce; an empty string matches the schema defaults.
QString notNullText(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

} // namespace

DraftStore::DraftStore(Database &database) : m_database(database) {
}

Result<qint64> DraftStore::save(const Draft &draft) {
    // R29/R78: updatedAt is store-owned bookkeeping, always the current UTC
    // time in milliseconds; caller-supplied values are ignored.
    const qint64 updatedAtMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return Result<qint64>::failure(storageError("begin transaction", db.lastError()));
    }

    MailError error;
    qint64 draftId = 0;
    if (draft.id <= 0) {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "INSERT INTO drafts (account_id, subject, body_text, recipients_json,"
                " cc_json, updated_at) VALUES (:account_id, :subject, :body_text,"
                " :recipients_json, :cc_json, :updated_at)"))) {
            error = storageError("prepare", query.lastError());
        }
        if (!error) {
            query.bindValue(QStringLiteral(":account_id"), draft.accountId);
            query.bindValue(QStringLiteral(":subject"), notNullText(draft.subject));
            query.bindValue(QStringLiteral(":body_text"), notNullText(draft.bodyText));
            query.bindValue(QStringLiteral(":recipients_json"), addressesToJson(draft.recipients));
            query.bindValue(QStringLiteral(":cc_json"), addressesToJson(draft.cc));
            query.bindValue(QStringLiteral(":updated_at"), updatedAtMs);
            if (!query.exec()) {
                // A missing account row violates the foreign key here.
                error = storageError("insert", query.lastError());
            } else {
                const QVariant lastInsertId = query.lastInsertId();
                if (!lastInsertId.isValid() || lastInsertId.toLongLong() <= 0) {
                    error = MailError{
                        ErrorCode::Storage, QStringLiteral("Insert returned no id"),
                        QStringLiteral("drafts INSERT for account %1").arg(draft.accountId)};
                } else {
                    draftId = lastInsertId.toLongLong();
                }
            }
        }
    } else {
        QSqlQuery query(db);
        // R80: ownership never changes on update. account_id appears in the
        // WHERE clause only, so a save that arrives under a different account
        // misses (0 rows) instead of migrating the draft. SQLite reports a
        // matched row as changed even when the values are identical
        // (numRowsAffected wraps sqlite3_changes, no raw call needed): a zero
        // count therefore means "no such row owned by this account".
        if (!query.prepare(QStringLiteral(
                "UPDATE drafts SET subject = :subject, body_text = :body_text,"
                " recipients_json = :recipients_json, cc_json = :cc_json, updated_at = :updated_at"
                " WHERE id = :id AND account_id = :account_id"))) {
            error = storageError("prepare", query.lastError());
        }
        if (!error) {
            query.bindValue(QStringLiteral(":account_id"), draft.accountId);
            query.bindValue(QStringLiteral(":subject"), notNullText(draft.subject));
            query.bindValue(QStringLiteral(":body_text"), notNullText(draft.bodyText));
            query.bindValue(QStringLiteral(":recipients_json"), addressesToJson(draft.recipients));
            query.bindValue(QStringLiteral(":cc_json"), addressesToJson(draft.cc));
            query.bindValue(QStringLiteral(":updated_at"), updatedAtMs);
            query.bindValue(QStringLiteral(":id"), draft.id);
            if (!query.exec()) {
                error = storageError("update", query.lastError());
            } else if (query.numRowsAffected() != 1) {
                // A missing id or an id owned by another account must fail,
                // never silently become an insert or a migration.
                error = MailError{ErrorCode::Storage, QStringLiteral("Draft not found"),
                                  QStringLiteral("No drafts row with id %1 owned by account %2")
                                      .arg(draft.id)
                                      .arg(draft.accountId)};
            } else {
                draftId = draft.id;
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
    return Result<qint64>::success(draftId);
}

Result<Draft> DraftStore::get(qint64 id) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM drafts WHERE id = :id")
                           .arg(QLatin1String(kDraftColumns)))) {
        return Result<Draft>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return Result<Draft>::failure(storageError("select", query.lastError()));
    }
    if (!query.next()) {
        return Result<Draft>::failure(notFound("Draft", id));
    }
    return Result<Draft>::success(draftFromQuery(query));
}

MailError DraftStore::remove(qint64 id) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowDeleted = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("DELETE FROM drafts WHERE id = :id"))) {
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
        return notFound("Draft", id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

Result<QList<Draft>> DraftStore::list(AccountId accountId) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM drafts WHERE account_id = :account_id"
                                      " ORDER BY updated_at DESC, id DESC")
                           .arg(QLatin1String(kDraftColumns)))) {
        return Result<QList<Draft>>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":account_id"), accountId);
    if (!query.exec()) {
        return Result<QList<Draft>>::failure(storageError("select", query.lastError()));
    }

    QList<Draft> drafts;
    while (query.next()) {
        drafts.append(draftFromQuery(query));
    }
    return Result<QList<Draft>>::success(std::move(drafts));
}

} // namespace ningmail
