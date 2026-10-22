#include "Types.h"

#include "MailError.h"

#include <QMetaType>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("MailFlags combine and test", "[types]") {
    ningmail::MailFlags f;
    f |= ningmail::MailFlag::Seen;
    f |= ningmail::MailFlag::Flagged;
    REQUIRE(f.testFlag(ningmail::MailFlag::Seen));
    REQUIRE(f.testFlag(ningmail::MailFlag::Flagged));
    REQUIRE_FALSE(f.testFlag(ningmail::MailFlag::Deleted));
}

TEST_CASE("Result carries value or error", "[types]") {
    auto ok = ningmail::Result<int>::success(42);
    REQUIRE(ok.ok());
    REQUIRE(ok.value.value() == 42);
    auto bad = ningmail::Result<int>::failure({ningmail::ErrorCode::Network, "offline", ""});
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error.code == ningmail::ErrorCode::Network);
}

TEST_CASE("MailError is truthy only on failure", "[types]") {
    REQUIRE_FALSE(ningmail::MailError{});
    REQUIRE(ningmail::MailError{ningmail::ErrorCode::Auth, "bad password", ""});
}

TEST_CASE("registerMetaTypes registers domain types", "[types]") {
    ningmail::registerMetaTypes();
    CHECK(QMetaType::fromType<QList<ningmail::Folder>>().isRegistered());
    CHECK(QMetaType::fromType<QList<ningmail::MessageHeader>>().isRegistered());
    CHECK(QMetaType::fromType<QList<ningmail::AttachmentMeta>>().isRegistered());
    CHECK(QMetaType::fromType<ningmail::MessageBody>().isRegistered());
    CHECK(QMetaType::fromType<ningmail::MailError>().isRegistered());
}
