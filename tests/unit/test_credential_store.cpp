#include "CredentialStore.h"
#include "MemoryCredentialStore.h"

#include <QString>
#include <QUuid>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

#ifdef NINGMAIL_HAVE_LIBSECRET

#include <platform/kylin/LibSecretCredentialStore.h>

#endif // NINGMAIL_HAVE_LIBSECRET

namespace {

/// Never logged or printed; only used to drive the assertions below.
const QString kTestKey = QStringLiteral("account/1");
const QString kTestSecret = QStringLiteral("secret-value-a");
const QString kRotatedSecret = QStringLiteral("secret-value-b");

#ifdef NINGMAIL_HAVE_LIBSECRET

/// Secret Service tests are opt-in only (spec: they never run in CI): without
/// this gate they could block on D-Bus service activation and would write to
/// the developer's real keyring.
bool secretTestsEnabled() {
    return qEnvironmentVariable("NINGMAIL_ENABLE_SECRET_TESTS") == QStringLiteral("1");
}

/// Uniquely named keyring entry with an obvious marker; removed by the store
/// on scope exit even when an assertion fails mid-test.
class SecretTestKey {
  public:
    SecretTestKey(ningmail::LibSecretCredentialStore &store, QString key)
        : m_store(store), m_key(std::move(key)) {
    }

    ~SecretTestKey() {
        m_store.remove(m_key);
    }

    SecretTestKey(const SecretTestKey &) = delete;
    SecretTestKey &operator=(const SecretTestKey &) = delete;

    const QString &key() const {
        return m_key;
    }

  private:
    ningmail::LibSecretCredentialStore &m_store;
    QString m_key;
};

#endif // NINGMAIL_HAVE_LIBSECRET

} // namespace

TEST_CASE("memory credential store round-trips secrets", "[credential]") {
    ningmail::MemoryCredentialStore store;

    REQUIRE_FALSE(store.isPersistent());
    REQUIRE(store.write(kTestKey, kTestSecret));
    REQUIRE(store.read(kTestKey) == kTestSecret);

    // Overwrite keeps the same key.
    REQUIRE(store.write(kTestKey, kRotatedSecret));
    REQUIRE(store.read(kTestKey) == kRotatedSecret);

    store.remove(kTestKey);
    REQUIRE_FALSE(store.read(kTestKey).has_value());
}

TEST_CASE("memory credential store misses unknown keys", "[credential]") {
    ningmail::MemoryCredentialStore store;

    REQUIRE_FALSE(store.read(kTestKey).has_value());
    store.remove(kTestKey); // removing an absent key is a no-op
    REQUIRE_FALSE(store.isPersistent());
}

#ifdef NINGMAIL_HAVE_LIBSECRET

TEST_CASE("libsecret store does not touch the secret service at construction", "[.][secret]") {
    if (!secretTestsEnabled()) {
        SKIP("Secret Service tests are opt-in; set NINGMAIL_ENABLE_SECRET_TESTS=1 to enable");
    }

    // Must not block, start a D-Bus session or crash without a Secret Service.
    ningmail::LibSecretCredentialStore store;
    REQUIRE_FALSE(store.isPersistent());
}

TEST_CASE("libsecret store falls back to memory when keyring write fails", "[.][secret]") {
    if (!secretTestsEnabled()) {
        SKIP("Secret Service tests are opt-in; set NINGMAIL_ENABLE_SECRET_TESTS=1 to enable");
    }

    ningmail::LibSecretCredentialStore store;
    SecretTestKey testKey(
        store,
        QStringLiteral("ningmail-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    const bool persisted = store.write(testKey.key(), kTestSecret);
    if (persisted) {
        REQUIRE(store.isPersistent());
        REQUIRE(store.read(testKey.key()) == kTestSecret);
    } else {
        // No Secret Service: the secret must stay readable from the in-process fallback.
        REQUIRE_FALSE(store.isPersistent());
        REQUIRE(store.read(testKey.key()) == kTestSecret);
    }
    // Explicit removal verifies remove() semantics; the RAII guard still
    // covers failures that abort the test before this point.
    store.remove(testKey.key());
    REQUIRE_FALSE(store.read(testKey.key()).has_value());
}

#endif // NINGMAIL_HAVE_LIBSECRET
