#pragma once

#include "MailBackend.h"
#include "Types.h"

#include <QFuture>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>

namespace ningmail {

class CredentialStore;

/// Performs account syncs and single message body fetches on the QtConcurrent
/// thread pool and reports results back to the GUI thread through signals.
///
/// Threading contract (R48): at most one backend operation is active at any
/// time. A worker holds the operation mutex for its entire run, so factory
/// create/connect calls never overlap. A new syncAccount()/fetchBody() or a
/// stop() increments the epoch; a run whose epoch is stale abandons its
/// remaining phases and drops (never emits) its pending results.
class SyncService final : public QObject {
    Q_OBJECT
  public:
    explicit SyncService(MailBackendFactory *factory, CredentialStore *credentials,
                         QObject *parent = nullptr);
    ~SyncService() override;

    SyncService(const SyncService &) = delete;
    SyncService &operator=(const SyncService &) = delete;

  public slots:
    /// Runs connect -> listFolders -> fetchHeaders(inbox) in the background.
    void syncAccount(const Account &account);
    /// Runs connect -> fetchBody in the background (no database access).
    void fetchBody(const Account &account, const MessageHeader &header);
    /// Cancels the active operation; no further signals are emitted for it.
    void stop();

  signals:
    void syncStarted(qint64 accountId);
    void foldersFetched(qint64 accountId, const QList<Folder> &folders);
    void headersFetched(qint64 accountId, const QString &folderFullName,
                        const QList<MessageHeader> &headers);
    void syncProgress(qint64 accountId, int done, int total);
    /// Emitted as the last signal of a sync; a default MailError means success.
    void syncFinished(qint64 accountId, const MailError &error);
    /// A default MailError means success; on failure `body` is empty and
    /// `messageId` is still the requested header id.
    void bodyFetched(qint64 messageId, const MessageBody &body, const MailError &error);

  private:
    void runSync(const Account &account, const QString &secret, quint64 epoch);
    void runBodyFetch(const Account &account, const MessageHeader &header, quint64 epoch);
    void dispatch(std::function<void(quint64)> worker);
    /// Queues `step` on the service's (GUI) thread; the step runs only when
    /// `epoch` is still current, so superseded results never reach receivers,
    /// and pending steps are discarded when the service is destroyed.
    void postStep(quint64 epoch, std::function<void()> step);

    MailBackendFactory *m_factory;
    CredentialStore *m_credentials;
    std::mutex m_operationMutex;
    std::atomic<quint64> m_epoch;
    // Workers increment this on dispatch (in the caller's thread) and
    // decrement it after their last member access, so the destructor can
    // observe when no worker may touch members anymore.
    std::atomic<int> m_activeWorkers;
    QFuture<void> m_activeFuture;
};

} // namespace ningmail
