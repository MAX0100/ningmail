#include "LibSecretCredentialStore.h"

#include <libsecret/secret.h>

namespace ningmail {

namespace {

/// Keyring schema fixed for the application. `SECRET_SCHEMA_DONT_MATCH_NAME`
/// makes lookups attribute-based instead of relying on the stored schema name.
const SecretSchema *credentialSchema() {
    static const SecretSchema schema = {
        "org.ningmail.Credentials",
        SECRET_SCHEMA_DONT_MATCH_NAME,
        {
            {"key", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}, // terminator: name == NULL
        },
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr, // reserved
    };
    return &schema;
}

/// Converts a keyring answer into an owned QString; nullopt when absent.
std::optional<QString> adoptSecret(gchar *raw) {
    if (raw == nullptr) {
        return std::nullopt;
    }
    const QString secret = QString::fromUtf8(raw);
    g_free(raw);
    return secret;
}

/// Ends a var-args attribute list ("key", ..., NULL) for the -Wpedantic build.
constexpr void *kAttributesEnd = nullptr;

} // namespace

bool LibSecretCredentialStore::write(const QString &key, const QString &secret) {
    const QByteArray keyUtf8 = key.toUtf8();
    GError *error = nullptr;
    const gboolean stored = secret_password_store_sync(
        credentialSchema(), SECRET_COLLECTION_DEFAULT, keyUtf8.constData(),
        secret.toUtf8().constData(), nullptr, &error, "key", keyUtf8.constData(), kAttributesEnd);
    if (error != nullptr) {
        g_error_free(error);
    }

    // Only a successful keyring write counts as persistent; otherwise the
    // secret falls back to the in-process hash so read() still finds it.
    if (stored == TRUE) {
        m_fallback.remove(key);
        m_persistent = true;
        return true;
    }
    m_fallback.insert(key, secret);
    m_persistent = false;
    return false;
}

std::optional<QString> LibSecretCredentialStore::read(const QString &key) {
    if (const auto it = m_fallback.constFind(key); it != m_fallback.constEnd()) {
        return *it;
    }

    const QByteArray keyUtf8 = key.toUtf8();
    GError *error = nullptr;
    gchar *raw = secret_password_lookup_sync(credentialSchema(), nullptr, &error, "key",
                                             keyUtf8.constData(), kAttributesEnd);
    if (error != nullptr) {
        g_error_free(error);
    }
    return adoptSecret(raw);
}

void LibSecretCredentialStore::remove(const QString &key) {
    m_fallback.remove(key);

    const QByteArray keyUtf8 = key.toUtf8();
    GError *error = nullptr;
    secret_password_clear_sync(credentialSchema(), nullptr, &error, "key", keyUtf8.constData(),
                               kAttributesEnd);
    if (error != nullptr) {
        g_error_free(error);
    }
}

bool LibSecretCredentialStore::isPersistent() const {
    return m_persistent;
}

} // namespace ningmail
