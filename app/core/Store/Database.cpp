#include "Store/Database.h"

#include "Store/Migrations.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace ningmail {

/// Returns the error condition as a failure MailError carrying the driver text.
static MailError sqlError(const QSqlError &error, const char *context) {
    return MailError{ErrorCode::Storage,
                     QStringLiteral("SQLite %1 failed").arg(QLatin1String(context)), error.text()};
}

Database::Database(QString path) : m_path(std::move(path)) {
    m_connectionName =
        QStringLiteral("ningmail-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

Database::~Database() {
    releaseConnection();
}

/// Drops the current Qt connection registration. All references held by this
/// object go first, so `removeDatabase` can never report "still in use".
void Database::releaseConnection() {
    if (m_db.isValid()) {
        m_db.close();
        m_db = QSqlDatabase{}; // drop our last handle before removing the registration
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

/// Sets foreign keys and WAL mode; the queries are scoped so no handle lasts
/// beyond this call. `m_db` must be open and WAL requires a file-backed path.
MailError Database::applyPragmas() {
    MailError failure;
    {
        QSqlQuery pragma(m_db);
        if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) {
            failure = sqlError(pragma.lastError(), "foreign_keys pragma");
        }
    }
    if (failure) {
        return failure;
    }

    // WAL is only supported for file-backed databases.
    if (m_path == QStringLiteral(":memory:")) {
        return MailError{};
    }
    QSqlQuery journal(m_db);
    if (!journal.exec(QStringLiteral("PRAGMA journal_mode=WAL"))) {
        return sqlError(journal.lastError(), "journal_mode pragma");
    }
    return MailError{};
}

MailError Database::open() {
    if (m_db.isValid() && m_db.isOpen()) {
        return MailError{};
    }

    // A previous open() may have failed after addDatabase(); drop the stale
    // registration first. Re-registering the same name would make Qt discard
    // the old connection with a warning and invalidate outstanding handles.
    releaseConnection();

    const QFileInfo info(m_path);
    if (!info.dir().mkpath(QStringLiteral("."))) {
        return MailError{ErrorCode::Storage, QStringLiteral("Could not create database directory"),
                         info.dir().absolutePath()};
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_path);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) {
        const QSqlError error = m_db.lastError();
        releaseConnection(); // leave nothing registered while open() reports failure
        return sqlError(error, "open");
    }

    MailError error = applyPragmas();
    if (error) {
        releaseConnection();
    }
    return error;
}

MailError Database::migrate() {
    return Migrations::apply(m_db);
}

int Database::schemaVersion() const {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT version FROM schema_version LIMIT 1")) ||
        !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QSqlDatabase &Database::connection() {
    return m_db;
}

QString Database::path() const {
    return m_path;
}

QString Database::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/ningmail.db");
}

} // namespace ningmail
