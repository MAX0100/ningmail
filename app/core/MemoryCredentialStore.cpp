#include "MemoryCredentialStore.h"

namespace ningmail {

std::optional<QString> MemoryCredentialStore::read(const QString &key) {
    return m_secrets.contains(key) ? std::optional<QString>(m_secrets.value(key)) : std::nullopt;
}

bool MemoryCredentialStore::write(const QString &key, const QString &secret) {
    m_secrets.insert(key, secret);
    return true;
}

void MemoryCredentialStore::remove(const QString &key) {
    m_secrets.remove(key);
}

bool MemoryCredentialStore::isPersistent() const {
    return false;
}

} // namespace ningmail
