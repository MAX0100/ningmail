// Integration test for LibetpanBackend SMTP send against a real account.
//
// One plain-text message is composed with MimeBuilder, handed to the
// backend's send() (openSmtp → esmtp/mailsmtp_send), and — so the reply can
// be verified deterministically — copied into the Sent folder, which is then
// re-read over IMAP. The self-addressed INBOX copy is additionally looked up
// because real providers deliver it asynchronously.
//
// In M1 LibetpanBackend::connect always performs the IMAP login first (one
// credential speaks both protocols), so the IMAP variables are required too.
// Credential model (honest disclosure): the SMTP password is THE connection
// secret — connect() feeds it to the IMAP LOGIN and mailsmtp_auth alike
// (M1 single-credential assumption, R88: QQ/163-style same authorization
// code for both). If IMAP_PASS is present but differs from SMTP_PASS, the
// IMAP-side roundtrip verification may fail with Auth; the SMTP send part
// is still verified and the case does NOT skip — no special-casing for that
// mismatch (dual-password support is an interface change, M2).
//
// The case is skipped unless every variable below is set: the development
// host carries no real account, so the network paths are verified on a real
// device in T17 (R1 — like the [imap] skeleton, this file has no RED: the
// unit suite owns the RED evidence for task 14, the appendSent best-effort
// fix in SendService).

#include "LibetpanBackend.h"
#include "MimeBuilder.h"

#include <QDateTime>

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef NINGMAIL_WITH_LIBETPAN

namespace {

struct SmtpTestAccount {
    bool available = false;
    QString missed;
    QString host;
    quint16 port = 465;
    QString user;
    QString password;
    ningmail::Security security = ningmail::Security::Tls;
};

SmtpTestAccount smtpAccountFromEnvironment() {
    SmtpTestAccount result;
    result.host = qEnvironmentVariable("NINGMAIL_TEST_SMTP_HOST");
    result.user = qEnvironmentVariable("NINGMAIL_TEST_SMTP_USER");
    result.password = qEnvironmentVariable("NINGMAIL_TEST_SMTP_PASS");
    // All four — host, port, user, pass — are mandatory (一路 SKIP 文案逐个点名).
    QString missed;
    if (result.host.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_SMTP_HOST ");
    }
    const QString port = qEnvironmentVariable("NINGMAIL_TEST_SMTP_PORT");
    if (port.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_SMTP_PORT ");
    }
    if (result.user.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_SMTP_USER ");
    }
    if (result.password.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_SMTP_PASS ");
    }
    // The backend connects IMAP first: the reply verification also needs it.
    const QString imapHost = qEnvironmentVariable("NINGMAIL_TEST_IMAP_HOST");
    const QString imapUser = qEnvironmentVariable("NINGMAIL_TEST_IMAP_USER");
    const QString imapPass = qEnvironmentVariable("NINGMAIL_TEST_IMAP_PASS");
    if (imapHost.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_IMAP_HOST ");
    }
    if (imapUser.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_IMAP_USER ");
    }
    if (imapPass.isEmpty()) {
        missed += QStringLiteral("NINGMAIL_TEST_IMAP_PASS ");
    }
    result.missed = missed.trimmed();

    if (!port.isEmpty()) {
        result.port = static_cast<quint16>(port.toUInt());
    }
    // Optional wire security: tls (default, 465) / starttls (587) / none (25).
    const QString security = qEnvironmentVariable("NINGMAIL_TEST_SMTP_SECURITY").toLower();
    if (security == QStringLiteral("starttls")) {
        result.security = ningmail::Security::StartTls;
    } else if (security == QStringLiteral("none")) {
        result.security = ningmail::Security::None;
    }

    result.available = result.missed.isEmpty() && result.port != 0;
    return result;
}

ningmail::Account accountConfig(const SmtpTestAccount &smtp, const QString &imapHost,
                                quint16 imapPort) {
    ningmail::Account config;
    config.id = 1;
    config.email = smtp.user;
    config.username = smtp.user;
    config.imapHost = imapHost;
    config.imapPort = imapPort;
    config.imapSecurity = ningmail::Security::Tls;
    config.smtpHost = smtp.host;
    config.smtpPort = smtp.port;
    config.smtpSecurity = smtp.security;
    return config;
}

} // namespace

TEST_CASE("libetpan backend sends a real SMTP message and re-reads it", "[integration][smtp]") {
    const SmtpTestAccount smtp = smtpAccountFromEnvironment();
    if (!smtp.available) {
        SKIP("set NINGMAIL_TEST_SMTP_HOST/PORT/USER/PASS and NINGMAIL_TEST_IMAP_HOST/USER/PASS "
             "to run against a real account; missing (or unparsable port): " +
             smtp.missed.toStdString());
    }

    // The SMTP password is the connection credential (M1 single-secret
    // model, see header): a different IMAP_PASS only risks the IMAP-side
    // verification below, never the send.
    const QString imapHost = qEnvironmentVariable("NINGMAIL_TEST_IMAP_HOST");
    const QString imapPort = qEnvironmentVariable("NINGMAIL_TEST_IMAP_PORT");
    const ningmail::Account config =
        accountConfig(smtp, imapHost,
                      imapPort.isEmpty() ? quint16{993} : static_cast<quint16>(imapPort.toUInt()));

    ningmail::LibetpanBackend backend;
    const ningmail::MailError connected = backend.connect(config, smtp.password);
    REQUIRE_FALSE(connected);

    // The Message-ID domain follows the sender so strict servers keep it.
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    const int at = smtp.user.lastIndexOf(u'@');
    const QString domain = at >= 0 ? smtp.user.mid(at + 1) : QStringLiteral("ningmail.test");

    ningmail::OutgoingMessage message;
    message.account = config;
    message.to.append(ningmail::MailAddress{QString(), smtp.user}); // self-addressed
    message.subject = QString::fromUtf8("宁邮 SMTP 联测 %1").arg(stamp);
    message.bodyText = QString::fromUtf8("这是宁邮 libetpan SMTP 联测的一封纯文本邮件。\n"
                                         "M0/M1 end-to-end send check (task 14).");
    message.rfcMessageId = QStringLiteral("<smtp-integration-%1@%2>").arg(stamp).arg(domain);

    // The backend builds its own MIME inside send(); the same builder output
    // is what production appendSent receives, so reuse it for the copy.
    const QByteArray mime = ningmail::MimeBuilder::build(message);

    const ningmail::MailError sent = backend.send(message);
    REQUIRE_FALSE(sent);

    // Production treats the Sent copy as best effort (R91): report it, but a
    // missing Sent folder (Protocol) must not fail this case — the smoke is
    // about send() being accepted.
    const ningmail::MailError appended = backend.appendSent(message, mime);
    CHECK_FALSE(appended);

    // Roundtrip: find the message by its Message-ID in the Sent folder first
    // (deterministic right after appendSent), then INBOX for the delivered
    // copy. Wait policy (honest): self-delivery is asynchronous on real
    // providers — the loop re-fetches headers up to 6 times, pausing 5 s
    // between rounds (≈25 s worst case), then fails loudly.
    const ningmail::Result<QList<ningmail::Folder>> folders = backend.listFolders();
    REQUIRE(folders.ok());

    QList<ningmail::Folder> candidates;
    for (const ningmail::Folder &folder : folders.value.value()) {
        if (folder.role == ningmail::FolderRole::Sent ||
            folder.role == ningmail::FolderRole::Inbox) {
            candidates.append(folder);
        }
    }
    // Sent folder candidates lead (our own copy exists immediately).
    std::sort(candidates.begin(), candidates.end(),
              [](const ningmail::Folder &left, const ningmail::Folder &right) {
                  return static_cast<int>(left.role) > static_cast<int>(right.role);
              });
    REQUIRE_FALSE(candidates.isEmpty());

    bool matched = false;
    for (int round = 0; round < 6 && !matched; ++round) {
        if (round > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        for (const ningmail::Folder &folder : candidates) {
            const ningmail::Result<QList<ningmail::MessageHeader>> headers =
                backend.fetchHeaders(folder, 1, 50);
            REQUIRE(headers.ok());
            for (const ningmail::MessageHeader &header : headers.value.value()) {
                if (header.rfcMessageId != message.rfcMessageId) {
                    continue;
                }
                matched = true;
                INFO("found in folder " << folder.fullName.toStdString() << " after " << round
                                        << " waiting rounds");
                CHECK(header.subject == message.subject);
                const ningmail::Result<ningmail::MessageBody> body = backend.fetchBody(header);
                REQUIRE(body.ok());
                CHECK(body.value->plainText.contains(message.bodyText));
                break;
            }
            if (matched) {
                break;
            }
        }
    }
    REQUIRE(matched);

    backend.disconnect();
}

#else // NINGMAIL_WITH_LIBETPAN

// libetpan is absent: there is no backend source to exercise.
TEST_CASE("libetpan backend is not compiled on this host", "[integration][smtp]") {
    INFO("libetpan-dev missing: LibetpanBackend compiled out (NINGMAIL_WITH_LIBETPAN=0)");
    SUCCEED();
}

#endif // NINGMAIL_WITH_LIBETPAN
