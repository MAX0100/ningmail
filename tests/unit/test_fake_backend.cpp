#include "FakeMailBackend.h"

#include "Types.h"

#include <catch2/catch_test_macros.hpp>

namespace {

ningmail::Account makeTestAccount(ningmail::AccountId accountId) {
    ningmail::Account account;
    account.id = accountId;
    return account;
}

ningmail::OutgoingMessage makeTestMessage(ningmail::AccountId accountId) {
    ningmail::OutgoingMessage message;
    message.account.id = accountId;
    message.to.append({QString::fromUtf8("Peer"), QString::fromUtf8("peer@example.test")});
    message.subject = QString::fromUtf8("Hello");
    message.bodyText = QString::fromUtf8("Hi there");
    return message;
}

} // namespace

TEST_CASE("FakeMailBackend connect authenticates", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(ningmail::Account{});

    REQUIRE(backend != nullptr);
    REQUIRE_FALSE(backend->connect(ningmail::Account{}, QString::fromUtf8("secret")));
    auto failure = backend->connect(ningmail::Account{}, QString::fromUtf8("wrong"));
    REQUIRE(failure.code == ningmail::ErrorCode::Auth);
}

TEST_CASE("FakeMailBackend exposes only the connected account's folders", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(7));
    REQUIRE_FALSE(backend->connect(makeTestAccount(7), QString::fromUtf8("secret")));

    auto foldersResult = backend->listFolders();
    REQUIRE(foldersResult.ok());
    REQUIRE(foldersResult.value->size() == 2);
    REQUIRE(foldersResult.value->at(0).fullName == QString::fromUtf8("INBOX"));
    REQUIRE(foldersResult.value->at(0).accountId == 7);
    REQUIRE(foldersResult.value->at(0).uidValidity == 12345);
    REQUIRE(foldersResult.value->at(1).fullName == QString::fromUtf8("Sent"));
    REQUIRE(foldersResult.value->at(1).accountId == 7);
    REQUIRE(foldersResult.value->at(1).role == ningmail::FolderRole::Sent);

    auto sentHeaders = backend->fetchHeaders(foldersResult.value->at(1), 1, 100);
    REQUIRE(sentHeaders.ok());
    REQUIRE(sentHeaders.value->isEmpty());
}

TEST_CASE("FakeMailBackend fetchHeaders returns first limit with uid >= fromUid", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(7));
    REQUIRE_FALSE(backend->connect(makeTestAccount(7), QString::fromUtf8("secret")));
    auto foldersResult = backend->listFolders();
    REQUIRE(foldersResult.ok());
    const ningmail::Folder inbox = foldersResult.value->at(0);

    auto first100 = backend->fetchHeaders(inbox, 1, 100);
    REQUIRE(first100.ok());
    REQUIRE(first100.value->size() == 100);
    REQUIRE(first100.value->first().uid == 1);
    REQUIRE(first100.value->last().uid == 100);
    REQUIRE(first100.value->first().accountId == 7);
    REQUIRE(first100.value->first().folderId == inbox.id);

    auto tail = backend->fetchHeaders(inbox, 200, 100);
    REQUIRE(tail.ok());
    REQUIRE(tail.value->size() == 51);
    REQUIRE(tail.value->first().uid == 200);
    REQUIRE(tail.value->last().uid == 250);
}

TEST_CASE("FakeMailBackend fetchHeaders treats limit <= 0 as empty", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(1));
    REQUIRE_FALSE(backend->connect(makeTestAccount(1), QString::fromUtf8("secret")));
    auto foldersResult = backend->listFolders();
    REQUIRE(foldersResult.ok());
    const ningmail::Folder inbox = foldersResult.value->at(0);

    auto zero = backend->fetchHeaders(inbox, 1, 0);
    REQUIRE(zero.ok());
    REQUIRE(zero.value->isEmpty());

    auto negative = backend->fetchHeaders(inbox, 1, -3);
    REQUIRE(negative.ok());
    REQUIRE(negative.value->isEmpty());
}

TEST_CASE("FakeMailBackend rejects requests before connect", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(ningmail::Account{});

    auto folders = backend->listFolders();
    REQUIRE(folders.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(folders.error.detail == QString::fromUtf8("not connected"));

    auto headers = backend->fetchHeaders(ningmail::Folder{}, 1, 10);
    REQUIRE(headers.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(headers.error.detail == QString::fromUtf8("not connected"));

    auto body = backend->fetchBody(ningmail::MessageHeader{});
    REQUIRE(body.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(body.error.detail == QString::fromUtf8("not connected"));

    auto sendError = backend->send(makeTestMessage(1));
    REQUIRE(sendError.code == ningmail::ErrorCode::Storage);
    REQUIRE(sendError.detail == QString::fromUtf8("not connected"));
}

TEST_CASE("FakeMailBackend fetchHeaders rejects unknown folders", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(7));
    REQUIRE_FALSE(backend->connect(makeTestAccount(7), QString::fromUtf8("secret")));

    ningmail::Folder foreign;
    foreign.id = 1;
    foreign.accountId = 8;
    auto foreignResult = backend->fetchHeaders(foreign, 1, 10);
    REQUIRE(foreignResult.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(foreignResult.error.detail == QString::fromUtf8("Unknown folder"));

    ningmail::Folder unknown;
    unknown.id = 99;
    unknown.accountId = 7;
    auto unknownResult = backend->fetchHeaders(unknown, 1, 10);
    REQUIRE(unknownResult.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(unknownResult.error.detail == QString::fromUtf8("Unknown folder"));
}

TEST_CASE("FakeMailBackend fetchBody validates and resolves message id", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(7));
    REQUIRE_FALSE(backend->connect(makeTestAccount(7), QString::fromUtf8("secret")));
    auto foldersResult = backend->listFolders();
    REQUIRE(foldersResult.ok());
    const ningmail::Folder inbox = foldersResult.value->at(0);

    auto headers = backend->fetchHeaders(inbox, 1, 1);
    REQUIRE(headers.ok());
    REQUIRE(headers.value->size() == 1);

    auto known = backend->fetchBody(headers.value->at(0));
    REQUIRE(known.ok());
    REQUIRE(known.value->messageId == headers.value->at(0).id);
    REQUIRE(known.value->messageId != 0);

    ningmail::MessageHeader bogus;
    bogus.id = 99999;
    auto missing = backend->fetchBody(bogus);
    REQUIRE(missing.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(missing.error.detail == QString::fromUtf8("Unknown message"));
}

TEST_CASE("FakeMailBackend disconnect ends the session", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(5));
    REQUIRE_FALSE(backend->connect(makeTestAccount(5), QString::fromUtf8("secret")));

    backend->disconnect();

    auto folders = backend->listFolders();
    REQUIRE(folders.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(folders.error.detail == QString::fromUtf8("not connected"));
}

TEST_CASE("FakeMailBackend send appends to sent log", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(7));
    REQUIRE_FALSE(backend->connect(makeTestAccount(7), QString::fromUtf8("secret")));
    auto *fake = static_cast<ningmail::FakeMailBackend *>(backend.get());

    REQUIRE_FALSE(backend->send(makeTestMessage(7)));
    REQUIRE(fake->sentCount() == 1);
    REQUIRE(fake->sentRecord(0).message.subject == QString::fromUtf8("Hello"));
    REQUIRE(fake->sentRecord(0).message.account.id == 7);
}

TEST_CASE("FakeMailBackend failNextSend returns Network once", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto backend = factory.create(makeTestAccount(1));
    REQUIRE_FALSE(backend->connect(makeTestAccount(1), QString::fromUtf8("secret")));
    auto *fake = static_cast<ningmail::FakeMailBackend *>(backend.get());
    factory.seed(1).failNextSend = true;

    auto error = backend->send(makeTestMessage(1));
    REQUIRE(error.code == ningmail::ErrorCode::Network);
    REQUIRE(fake->sentCount() == 0);

    REQUIRE_FALSE(backend->send(makeTestMessage(1)));
    REQUIRE(fake->sentCount() == 1);
}

TEST_CASE("FakeMailBackendFactory shares seed across created backends", "[backend]") {
    ningmail::FakeMailBackendFactory factory;
    auto first = factory.create(makeTestAccount(3));
    auto second = factory.create(makeTestAccount(3));
    auto *firstFake = static_cast<ningmail::FakeMailBackend *>(first.get());
    auto *secondFake = static_cast<ningmail::FakeMailBackend *>(second.get());
    REQUIRE_FALSE(first->connect(makeTestAccount(3), QString::fromUtf8("secret")));
    REQUIRE_FALSE(second->connect(makeTestAccount(3), QString::fromUtf8("secret")));

    REQUIRE_FALSE(first->send(makeTestMessage(3)));
    REQUIRE_FALSE(second->send(makeTestMessage(3)));

    REQUIRE(firstFake->sentCount() == 2);
    REQUIRE(secondFake->sentCount() == 2);
    REQUIRE(firstFake->sentRecord(1).message.subject == QString::fromUtf8("Hello"));
    REQUIRE(factory.seed(3).sent.size() == 2);
}
