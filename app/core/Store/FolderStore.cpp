#include "Store/FolderStore.h"

#include "Store/Database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ningmail {

namespace {

/// Column order matches the `folders` table (schema v1); sync_state is handled
/// separately by updateSyncState().
const char *kSelectColumns =
    "id, account_id, full_name, display_name, role, delimiter, uid_validity, uid_next, "
    "highest_mod_seq";

Folder folderFromQuery(const QSqlQuery &query) {
    Folder folder;
    folder.id = query.value(0).toLongLong();
    folder.accountId = query.value(1).toLongLong();
    folder.fullName = query.value(2).toString();
    folder.displayName = query.value(3).toString();
    folder.role = static_cast<FolderRole>(query.value(4).toInt());
    const QString delimiter = query.value(5).toString();
    folder.delimiter = delimiter.isEmpty() ? QChar{u'/'} : delimiter.at(0);
    folder.uidValidity = static_cast<quint32>(query.value(6).toULongLong());
    folder.uidNext = static_cast<quint32>(query.value(7).toULongLong());
    folder.highestModSeq = query.value(8).toULongLong();
    return folder;
}

MailError storageError(const char *context, const QSqlError &error) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Folder store %1 failed").arg(QLatin1String(context)),
                     error.text()};
}

bool isError(const QSqlError &error) {
    return error.isValid() && error.type() != QSqlError::NoError;
}

/// NOT NULL TEXT columns must never receive a NULL bind, which a default
/// (null) QString would produce; an empty string matches the schema defaults.
QString notNullText(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

} // namespace

FolderStore::FolderStore(Database &database) : m_database(database) {
}

Result<QList<Folder>> FolderStore::list(AccountId accountId) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(
            QStringLiteral("SELECT %1 FROM folders WHERE account_id = :account_id ORDER BY id")
                .arg(QLatin1String(kSelectColumns)))) {
        return Result<QList<Folder>>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":account_id"), accountId);
    if (!query.exec()) {
        return Result<QList<Folder>>::failure(storageError("select", query.lastError()));
    }

    QList<Folder> folders;
    while (query.next()) {
        folders.append(folderFromQuery(query));
    }
    return Result<QList<Folder>>::success(std::move(folders));
}

Result<FolderId> FolderStore::upsert(const Folder &folder) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return Result<FolderId>::failure(storageError("begin transaction", db.lastError()));
    }

    qint64 folderId = 0;
    {
        // The row identity is (account_id, full_name); existing sync state stays
        // untouched because only display fields are updated.
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("SELECT id FROM folders WHERE account_id = :account_id "
                                          "AND full_name = :full_name"))) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("prepare", error));
        }
        query.bindValue(QStringLiteral(":account_id"), folder.accountId);
        query.bindValue(QStringLiteral(":full_name"), notNullText(folder.fullName));
        if (!query.exec()) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("select", error));
        }
        if (query.next()) {
            folderId = query.value(0).toLongLong();
        }
    }

    if (folderId > 0) {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "UPDATE folders SET display_name = :display_name, role = :role, delimiter = "
                ":delimiter WHERE id = :id"))) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("prepare", error));
        }
        query.bindValue(QStringLiteral(":display_name"), notNullText(folder.displayName));
        query.bindValue(QStringLiteral(":role"), static_cast<int>(folder.role));
        query.bindValue(QStringLiteral(":delimiter"), QString(folder.delimiter));
        query.bindValue(QStringLiteral(":id"), folderId);
        if (!query.exec()) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("update", error));
        }
    } else {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "INSERT INTO folders (account_id, full_name, display_name, role, delimiter,"
                " uid_validity, uid_next, highest_mod_seq, sync_state)"
                " VALUES (:account_id, :full_name, :display_name, :role, :delimiter,"
                " :uid_validity, :uid_next, :highest_mod_seq, NULL)"))) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("prepare", error));
        }
        query.bindValue(QStringLiteral(":account_id"), folder.accountId);
        query.bindValue(QStringLiteral(":full_name"), notNullText(folder.fullName));
        query.bindValue(QStringLiteral(":display_name"), notNullText(folder.displayName));
        query.bindValue(QStringLiteral(":role"), static_cast<int>(folder.role));
        query.bindValue(QStringLiteral(":delimiter"), QString(folder.delimiter));
        query.bindValue(QStringLiteral(":uid_validity"), static_cast<qint64>(folder.uidValidity));
        query.bindValue(QStringLiteral(":uid_next"), static_cast<qint64>(folder.uidNext));
        query.bindValue(QStringLiteral(":highest_mod_seq"),
                        static_cast<qint64>(folder.highestModSeq));
        if (!query.exec()) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<FolderId>::failure(storageError("insert", error));
        }
        const QVariant lastInsertId = query.lastInsertId();
        if (!lastInsertId.isValid() || lastInsertId.toLongLong() <= 0) {
            db.rollback();
            return Result<FolderId>::failure(
                MailError{ErrorCode::Storage, QStringLiteral("Insert returned no id"),
                          QStringLiteral("folders INSERT for %1").arg(folder.fullName)});
        }
        folderId = lastInsertId.toLongLong();
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return Result<FolderId>::failure(storageError("commit", error));
    }
    return Result<FolderId>::success(folderId);
}

MailError FolderStore::updateSyncState(FolderId id, quint32 uidValidity, quint32 uidNext,
                                       quint64 highestModSeq) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowUpdated = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "UPDATE folders SET uid_validity = :uid_validity, uid_next = :uid_next,"
                " highest_mod_seq = :highest_mod_seq WHERE id = :id"))) {
            queryError = query.lastError();
        } else {
            query.bindValue(QStringLiteral(":uid_validity"), static_cast<qint64>(uidValidity));
            query.bindValue(QStringLiteral(":uid_next"), static_cast<qint64>(uidNext));
            query.bindValue(QStringLiteral(":highest_mod_seq"), static_cast<qint64>(highestModSeq));
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
        return MailError{ErrorCode::Storage, QStringLiteral("Folder not found"),
                         QStringLiteral("No folders row with id %1").arg(id)};
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

} // namespace ningmail
