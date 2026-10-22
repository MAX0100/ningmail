#pragma once

#include "MailError.h"

#include <QList>
#include <QString>

#include <optional>

namespace ningmail {

/// Secrets abstraction: accounts only persist a `credential_ref`; the actual
/// secret lives behind one of these (Secret Service keyring, or in-memory).
class CredentialStore {
  public:
    virtual ~CredentialStore() = default;

    CredentialStore(const CredentialStore &) = delete;
    CredentialStore &operator=(const CredentialStore &) = delete;

    virtual std::optional<QString> read(const QString &key) = 0;
    virtual bool write(const QString &key, const QString &secret) = 0;
    virtual void remove(const QString &key) = 0;
    /// True when the most recent write() reached durable storage.
    virtual bool isPersistent() const = 0;

  protected:
    CredentialStore() = default;
};

} // namespace ningmail
