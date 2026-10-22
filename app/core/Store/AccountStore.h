#pragma once

#include "MailError.h"
#include "Types.h"

#include <QList>

namespace ningmail {

class Database;

/// CRUD for the `accounts` table; rows are mapped to the Account domain type.
class AccountStore {
  public:
    explicit AccountStore(Database &database);

    Result<QList<Account>> list() const;
    Result<Account> get(AccountId id) const;
    Result<AccountId> create(const Account &account);
    MailError update(const Account &account);
    MailError remove(AccountId id);

  private:
    Database &m_database;
};

} // namespace ningmail
