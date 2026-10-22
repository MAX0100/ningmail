#pragma once

#include "CredentialStore.h"

#include <QHash>
#include <QString>

#include <optional>

namespace ningmail {

/// Secret Service credential store backed by libsecret-1 (schema
/// `org.ningmail.Credentials`, attribute `key`). Constructing performs no
/// D-Bus I/O, so it is safe without a running Secret Service. When a keyring
/// write fails, the secret is kept in an in-process fallback hash so the
/// application still works for the session.
class LibSecretCredentialStore : public CredentialStore {
  public:
    std::optional<QString> read(const QString &key) override;
    bool write(const QString &key, const QString &secret) override;
    void remove(const QString &key) override;
    bool isPersistent() const override;

  private:
    /// Secrets that failed to reach the keyring; checked before keyring reads.
    QHash<QString, QString> m_fallback;
    bool m_persistent = false;
};

} // namespace ningmail
