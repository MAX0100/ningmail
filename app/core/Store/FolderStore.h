#pragma once

#include "MailError.h"
#include "Types.h"

#include <QList>

namespace ningmail {

class Database;

/// Access to the `folders` table. Upserts keep the per-(account, full_name)
/// row identity and never touch sync state columns; use updateSyncState for
/// uid_validity / uid_next / highest_mod_seq.
class FolderStore {
  public:
    explicit FolderStore(Database &database);

    /// Folders of one account, ordered by id.
    Result<QList<Folder>> list(AccountId accountId) const;
    /// Inserts or updates by (accountId, fullName); returns the folder id.
    Result<FolderId> upsert(const Folder &folder);
    MailError updateSyncState(FolderId id, quint32 uidValidity, quint32 uidNext,
                              quint64 highestModSeq);

  private:
    Database &m_database;
};

} // namespace ningmail
