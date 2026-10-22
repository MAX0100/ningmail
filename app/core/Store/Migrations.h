#pragma once

#include "MailError.h"

#include <QSqlDatabase>

namespace ningmail {

/// Applies schema migrations to the given database. Idempotent: no-op when the
/// schema is current, Storage error when the schema is newer than this build.
namespace Migrations {
MailError apply(QSqlDatabase &db);
}

} // namespace ningmail
