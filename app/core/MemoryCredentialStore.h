#pragma once

#include "CredentialStore.h"

#include <QHash>
#include <QString>

#include <optional>

namespace ningmail {

/// Session-only credential store backed by a QHash, used for tests and as the
/// in-process fallback when the keyring is unavailable. Secrets never leave
/// the process; `isPersistent()` is always false.
class MemoryCredentialStore : public CredentialStore {
  public:
    std::optional<QString> read(const QString &key) override;
    bool write(const QString &key, const QString &secret) override;
    void remove(const QString &key) override;
    bool isPersistent() const override;

  private:
    QHash<QString, QString> m_secrets;
};

} // namespace ningmail
