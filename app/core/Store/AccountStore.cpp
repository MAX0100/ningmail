#include "Store/AccountStore.h"

#include "Store/Database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ningmail {

namespace {

/// Column order matches the `accounts` table (schema v1, 14 columns).
const char *kSelectColumns =
    "id, display_name, email, imap_host, imap_port, imap_security, smtp_host, smtp_port, "
    "smtp_security, auth_type, username, credential_ref, signature_html, sort_order";

Account accountFromQuery(const QSqlQuery &query) {
    Account account;
    account.id = query.value(0).toLongLong();
    account.displayName = query.value(1).toString();
    account.email = query.value(2).toString();
    account.imapHost = query.value(3).toString();
    account.imapPort = static_cast<quint16>(query.value(4).toInt());
    account.imapSecurity = static_cast<Security>(query.value(5).toInt());
    account.smtpHost = query.value(6).toString();
    account.smtpPort = static_cast<quint16>(query.value(7).toInt());
    account.smtpSecurity = static_cast<Security>(query.value(8).toInt());
    account.authType = static_cast<AuthType>(query.value(9).toInt());
    account.username = query.value(10).toString();
    account.credentialRef = query.value(11).toString();
    account.signatureHtml = query.value(12).toString();
    account.sortOrder = query.value(13).toInt();
    return account;
}

MailError storageError(const char *context, const QSqlError &error) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Account store %1 failed").arg(QLatin1String(context)),
                     error.text()};
}

MailError notFound(const char *what, qint64 id) {
    return MailError{ErrorCode::Storage, QStringLiteral("%1 not found").arg(QLatin1String(what)),
                     QStringLiteral("No accounts row with id %1").arg(id)};
}

/// True when the last SQLite error on `error` is a real failure, not "no rows".
bool isError(const QSqlError &error) {
    return error.isValid() && error.type() != QSqlError::NoError;
}

/// NOT NULL TEXT columns must never receive a NULL bind, which a default
/// (null) QString would produce; an empty string matches the schema defaults.
QString notNullText(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

/// Binds every non-id column of `account` to the prepared statement.
void bindAccountFields(QSqlQuery &query, const Account &account) {
    query.bindValue(QStringLiteral(":display_name"), notNullText(account.displayName));
    query.bindValue(QStringLiteral(":email"), notNullText(account.email));
    query.bindValue(QStringLiteral(":imap_host"), notNullText(account.imapHost));
    query.bindValue(QStringLiteral(":imap_port"), account.imapPort);
    query.bindValue(QStringLiteral(":imap_security"), static_cast<int>(account.imapSecurity));
    query.bindValue(QStringLiteral(":smtp_host"), notNullText(account.smtpHost));
    query.bindValue(QStringLiteral(":smtp_port"), account.smtpPort);
    query.bindValue(QStringLiteral(":smtp_security"), static_cast<int>(account.smtpSecurity));
    query.bindValue(QStringLiteral(":auth_type"), static_cast<int>(account.authType));
    query.bindValue(QStringLiteral(":username"), notNullText(account.username));
    query.bindValue(QStringLiteral(":credential_ref"), notNullText(account.credentialRef));
    query.bindValue(QStringLiteral(":signature_html"), notNullText(account.signatureHtml));
    query.bindValue(QStringLiteral(":sort_order"), account.sortOrder);
}

} // namespace

AccountStore::AccountStore(Database &database) : m_database(database) {
}

Result<QList<Account>> AccountStore::list() const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM accounts ORDER BY sort_order, id")
                           .arg(QLatin1String(kSelectColumns))) ||
        !query.exec()) {
        return Result<QList<Account>>::failure(storageError("select", query.lastError()));
    }

    QList<Account> accounts;
    while (query.next()) {
        accounts.append(accountFromQuery(query));
    }
    return Result<QList<Account>>::success(std::move(accounts));
}

Result<Account> AccountStore::get(AccountId id) const {
    QSqlQuery query(m_database.connection());
    if (!query.prepare(QStringLiteral("SELECT %1 FROM accounts WHERE id = :id")
                           .arg(QLatin1String(kSelectColumns)))) {
        return Result<Account>::failure(storageError("select", query.lastError()));
    }
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        return Result<Account>::failure(storageError("select", query.lastError()));
    }
    if (!query.next()) {
        return Result<Account>::failure(notFound("Account", id));
    }
    return Result<Account>::success(accountFromQuery(query));
}

Result<AccountId> AccountStore::create(const Account &account) {
    QSqlDatabase &db = m_database.connection();
    // Each write runs in its own transaction; any failure rolls back cleanly.
    if (!db.transaction()) {
        return Result<AccountId>::failure(storageError("begin transaction", db.lastError()));
    }

    qint64 newId = 0;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "INSERT INTO accounts (display_name, email, imap_host, imap_port, imap_security,"
                " smtp_host, smtp_port, smtp_security, auth_type, username, credential_ref,"
                " signature_html, sort_order)"
                " VALUES (:display_name, :email, :imap_host, :imap_port, :imap_security,"
                " :smtp_host, :smtp_port, :smtp_security, :auth_type, :username, :credential_ref,"
                " :signature_html, :sort_order)"))) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<AccountId>::failure(storageError("prepare", error));
        }
        bindAccountFields(query, account);
        if (!query.exec()) {
            const QSqlError error = query.lastError();
            db.rollback();
            return Result<AccountId>::failure(storageError("insert", error));
        }
        const QVariant lastInsertId = query.lastInsertId();
        if (!lastInsertId.isValid() || lastInsertId.toLongLong() <= 0) {
            db.rollback();
            return Result<AccountId>::failure(
                MailError{ErrorCode::Storage, QStringLiteral("Insert returned no id"),
                          QStringLiteral("accounts INSERT for %1").arg(account.email)});
        }
        newId = lastInsertId.toLongLong();
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return Result<AccountId>::failure(storageError("commit", error));
    }
    return Result<AccountId>::success(newId);
}

MailError AccountStore::update(const Account &account) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowUpdated = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral(
                "UPDATE accounts SET display_name = :display_name, email = :email,"
                " imap_host = :imap_host, imap_port = :imap_port,"
                " imap_security = :imap_security, smtp_host = :smtp_host,"
                " smtp_port = :smtp_port, smtp_security = :smtp_security,"
                " auth_type = :auth_type, username = :username,"
                " credential_ref = :credential_ref, signature_html = :signature_html,"
                " sort_order = :sort_order WHERE id = :id"))) {
            queryError = query.lastError();
        } else {
            bindAccountFields(query, account);
            query.bindValue(QStringLiteral(":id"), account.id);
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
        return notFound("Account", account.id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

MailError AccountStore::remove(AccountId id) {
    QSqlDatabase &db = m_database.connection();
    if (!db.transaction()) {
        return storageError("begin transaction", db.lastError());
    }

    bool rowDeleted = false;
    QSqlError queryError;
    {
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("DELETE FROM accounts WHERE id = :id"))) {
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
        if (isError(queryError)) {
            return storageError("delete", queryError);
        }
        return notFound("Account", id);
    }

    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return storageError("commit", error);
    }
    return MailError{};
}

} // namespace ningmail
