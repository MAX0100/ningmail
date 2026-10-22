#include "Store/AccountStore.h"
#include "Store/Database.h"

#include <QTemporaryDir>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

/// Opens a schema v1 database in a temporary directory.
std::unique_ptr<ningmail::Database> migratedDatabase(const QTemporaryDir &dir) {
    auto db = std::make_unique<ningmail::Database>(dir.filePath("test.db"));
    REQUIRE_FALSE(db->open());
    REQUIRE_FALSE(db->migrate());
    return db;
}

ningmail::Account sampleAccount() {
    ningmail::Account account;
    account.displayName = "Work";
    account.email = "work@example.com";
    account.imapHost = "imap.example.com";
    account.imapPort = 1993;
    account.imapSecurity = ningmail::Security::StartTls;
    account.smtpHost = "smtp.example.com";
    account.smtpPort = 1587;
    account.smtpSecurity = ningmail::Security::None;
    account.authType = ningmail::AuthType::OAuth2;
    account.username = "work@example.com";
    account.credentialRef = "account/work";
    account.signatureHtml = "<p>Sent from NingMail</p>";
    account.sortOrder = 3;
    return account;
}

} // namespace

TEST_CASE("account store creates and reads back accounts", "[account]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore store(*database);

    const auto created = store.create(sampleAccount());
    REQUIRE(created.ok());
    REQUIRE(*created.value > 0);

    const auto fetched = store.get(*created.value);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->id == *created.value);
    // Every column must round-trip.
    REQUIRE(fetched.value->displayName == sampleAccount().displayName);
    REQUIRE(fetched.value->email == sampleAccount().email);
    REQUIRE(fetched.value->imapHost == sampleAccount().imapHost);
    REQUIRE(fetched.value->imapPort == 1993);
    REQUIRE(fetched.value->imapSecurity == ningmail::Security::StartTls);
    REQUIRE(fetched.value->smtpHost == sampleAccount().smtpHost);
    REQUIRE(fetched.value->smtpPort == 1587);
    REQUIRE(fetched.value->smtpSecurity == ningmail::Security::None);
    REQUIRE(fetched.value->authType == ningmail::AuthType::OAuth2);
    REQUIRE(fetched.value->username == sampleAccount().username);
    REQUIRE(fetched.value->credentialRef == sampleAccount().credentialRef);
    REQUIRE(fetched.value->signatureHtml == sampleAccount().signatureHtml);
    REQUIRE(fetched.value->sortOrder == 3);
}

TEST_CASE("account store lists by sort_order then id", "[account]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore store(*database);

    ningmail::Account first = sampleAccount();
    first.displayName = "Second";
    first.sortOrder = 5;
    const auto firstId = store.create(first);
    REQUIRE(firstId.ok());

    ningmail::Account second = sampleAccount();
    second.displayName = "First";
    second.sortOrder = 1;
    const auto secondId = store.create(second);
    REQUIRE(secondId.ok());

    ningmail::Account third = sampleAccount();
    third.displayName = "Third";
    third.sortOrder = 5;
    const auto thirdId = store.create(third);
    REQUIRE(thirdId.ok());

    const auto list = store.list();
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 3);
    REQUIRE(list.value->at(0).id == *secondId.value);
    REQUIRE(list.value->at(1).id == *firstId.value);
    REQUIRE(list.value->at(2).id == *thirdId.value);
}

TEST_CASE("account store update changes display name", "[account]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore store(*database);

    const auto created = store.create(sampleAccount());
    REQUIRE(created.ok());

    const auto before = store.get(*created.value);
    REQUIRE(before.ok());
    ningmail::Account changed = *before.value;
    changed.displayName = "Renamed";

    REQUIRE_FALSE(store.update(changed));

    const auto after = store.get(*created.value);
    REQUIRE(after.ok());
    REQUIRE(after.value->displayName == QStringLiteral("Renamed"));
    REQUIRE(after.value->email == sampleAccount().email);
    REQUIRE(after.value->sortOrder == 3);
}

TEST_CASE("account store remove hides the account and missing ids fail", "[account]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore store(*database);

    const auto created = store.create(sampleAccount());
    REQUIRE(created.ok());
    REQUIRE(store.get(*created.value).ok());

    REQUIRE_FALSE(store.remove(*created.value));
    const auto removed = store.get(*created.value);
    REQUIRE_FALSE(removed.ok());
    REQUIRE(removed.error.code == ningmail::ErrorCode::Storage);

    const auto unknown = store.get(12345);
    REQUIRE_FALSE(unknown.ok());
    REQUIRE(unknown.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(store.remove(12345).code == ningmail::ErrorCode::Storage);
}
