#pragma once

#include "app/core/MailBackend.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

namespace ningmail {

struct FakeSentMessage {
    OutgoingMessage message;
    QByteArray rawMime;
};

// Per connected-account state shared by every backend created from one factory.
struct FakeAccountSeed {
    QList<Folder> folders;
    QList<MessageHeader> inboxHeaders;
    QList<FakeSentMessage> sent;
    bool failNextSend = false;
};

class FakeMailBackend final : public MailBackend {
  public:
    FakeMailBackend(std::shared_ptr<QHash<qint64, FakeAccountSeed>> seeds, qint64 accountId);

    MailError connect(const Account &account, const QString &secret) override;
    Result<QList<Folder>> listFolders() override;
    Result<QList<MessageHeader>> fetchHeaders(const Folder &folder, quint32 fromUid,
                                              int limit) override;
    Result<MessageBody> fetchBody(const MessageHeader &header) override;
    MailError send(const OutgoingMessage &message) override;
    MailError appendSent(const OutgoingMessage &message, const QByteArray &rawMime) override;
    void disconnect() override;

    // Read-only accessors for tests to assert shared seed state.
    const FakeAccountSeed &seed() const;
    int sentCount() const;
    const FakeSentMessage &sentRecord(int index) const;

  private:
    FakeAccountSeed &currentSeed();

    std::shared_ptr<QHash<qint64, FakeAccountSeed>> m_seeds;
    qint64 m_accountId = 0;
    std::optional<qint64> m_connectedAccountId;
};

class FakeMailBackendFactory final : public MailBackendFactory {
  public:
    FakeMailBackendFactory();

    std::unique_ptr<MailBackend> create(const Account &account) override;

    FakeAccountSeed &seed(qint64 accountId);

  private:
    std::shared_ptr<QHash<qint64, FakeAccountSeed>> m_seeds;
};

} // namespace ningmail
