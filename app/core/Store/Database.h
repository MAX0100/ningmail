#pragma once

#include "MailError.h"

#include <QSqlDatabase>
#include <QString>

namespace ningmail {

/// SQLite persistence: owns a uniquely-named connection and the database file.
/// Timestamps are stored as Unix milliseconds; enums as INTEGER; address lists
/// as JSON text; flags as an INTEGER bitmask (see Migrations.cpp for mappings).
class Database {
  public:
    explicit Database(QString path);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    /// Opens the file (creating parent directories), enables foreign keys and WAL.
    MailError open();
    /// Applies pending schema migrations; idempotent.
    MailError migrate();
    /// Current schema version, 0 when not yet migrated.
    int schemaVersion() const;
    QSqlDatabase &connection();
    QString path() const;

    /// QStandardPaths::AppDataLocation + "/ningmail.db".
    static QString defaultPath();

  private:
    void releaseConnection();
    MailError applyPragmas();

    QString m_path;
    QString m_connectionName;
    QSqlDatabase m_db;
};

} // namespace ningmail
