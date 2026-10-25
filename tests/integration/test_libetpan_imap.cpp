// Integration tests for LibetpanBackend against a real IMAP account.
//
// These drive the protocol-speaking backend end to end (connect, LIST,
// SELECT, FETCH, body transfer). They are skipped unless every
// NINGMAIL_TEST_IMAP_* variable is set: the development host carries no real
// account, so the network paths are additionally verified on a real device
// in T17 (task 13, R1).

#include "LibetpanBackend.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QtGlobal>

#ifdef NINGMAIL_WITH_LIBETPAN

namespace {

struct ImapTestAccount {
    bool available = false;
    QString host;
    quint16 port = 993;
    QString user;
    QString password;
};

ImapTestAccount imapAccountFromEnvironment() {
    ImapTestAccount result;
    result.host = qEnvironmentVariable("NINGMAIL_TEST_IMAP_HOST");
    result.user = qEnvironmentVariable("NINGMAIL_TEST_IMAP_USER");
    result.password = qEnvironmentVariable("NINGMAIL_TEST_IMAP_PASS");
    result.available =
        !result.host.isEmpty() && !result.user.isEmpty() && !result.password.isEmpty();
    const QString port = qEnvironmentVariable("NINGMAIL_TEST_IMAP_PORT");
    if (!port.isEmpty()) {
        result.port = static_cast<quint16>(port.toUInt());
    }
    return result;
}

} // namespace

TEST_CASE("libetpan backend walks a real IMAP account", "[integration][imap]") {
    const ImapTestAccount account = imapAccountFromEnvironment();
    if (!account.available) {
        SKIP("set NINGMAIL_TEST_IMAP_HOST/PORT/USER/PASS to run against a real account");
    }

    ningmail::Account accountConfig;
    accountConfig.id = 1;
    accountConfig.email = account.user;
    accountConfig.username = account.user;
    accountConfig.imapHost = account.host;
    accountConfig.imapPort = account.port;
    accountConfig.imapSecurity = ningmail::Security::Tls;

    ningmail::LibetpanBackend backend;
    const ningmail::MailError connected = backend.connect(accountConfig, account.password);
    REQUIRE_FALSE(connected);

    const ningmail::Result<QList<ningmail::Folder>> folders = backend.listFolders();
    REQUIRE(folders.ok());
    REQUIRE_FALSE(folders.value->isEmpty());

    // Pick a role-assigned folder (INBOX by convention) and read headers.
    const ningmail::Folder *inbox = nullptr;
    for (const ningmail::Folder &folder : folders.value.value()) {
        if (folder.role == ningmail::FolderRole::Inbox) {
            inbox = &folder;
            break;
        }
    }
    REQUIRE(inbox != nullptr);

    const ningmail::Result<QList<ningmail::MessageHeader>> headers =
        backend.fetchHeaders(*inbox, 1, 10);
    REQUIRE(headers.ok());

    if (!headers.value->isEmpty()) {
        // Body transfer reuses the fetched raw message through MimeParser
        // (R70 note); message ids stay equal to uids end to end.
        const ningmail::Result<ningmail::MessageBody> body =
            backend.fetchBody(headers.value->first());
        REQUIRE(body.ok());
    }

    backend.disconnect();
}

#else // NINGMAIL_WITH_LIBETPAN

// libetpan is absent: there is no backend source to exercise.
TEST_CASE("libetpan backend is not compiled on this host", "[integration][imap]") {
    INFO("libetpan-dev missing: LibetpanBackend compiled out (NINGMAIL_WITH_LIBETPAN=0)");
    SUCCEED();
}

#endif // NINGMAIL_WITH_LIBETPAN
