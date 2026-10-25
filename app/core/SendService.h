#pragma once

#include "MailBackend.h"
#include "Store/OutboxStore.h"
#include "Types.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>

namespace ningmail {

class CredentialStore;

/// Drives outgoing messages: enqueue() stores the pending row (with a
/// pre-generated RFC message id and the built MIME), sendNow() performs one
/// backend round trip (connect → send → appendSent) on the QtConcurrent
/// thread pool and reports state changes back through itemStateChanged (R81).
///
/// Threading contract (same discipline as SyncService, T8): only enqueue() /
/// sendNow() run on the caller (GUI) thread. A worker holds the operation
/// mutex for its entire run, so credential reads and factory create/connect
/// never overlap with other workers, and every store access stays on the GUI
/// thread inside the queued completion steps. With one worker per outbox id
/// (the in-flight set), a double click can never start a second send.
///
/// M1: exactly one attempt per sendNow, no retry scheduler (next_retry_at
/// stays unset, failures park the row in Retry for a manual re-send).
class SendService final : public QObject {
    Q_OBJECT
  public:
    SendService(OutboxStore &outbox, MailBackendFactory &factory, CredentialStore &credentials,
                QObject *parent = nullptr);
    ~SendService() override;

    SendService(const SendService &) = delete;
    SendService &operator=(const SendService &) = delete;

    /// Stores the message as Pending (raw MIME pre-built, rfcMessageId
    /// generated when missing) and returns the new outbox row id; -1 when the
    /// store rejects the row. The account inside `message` is remembered as a
    /// snapshot for the later sendNow call.
    qint64 enqueue(const OutgoingMessage &message);
    /// Sends the stored row once. Items that are neither Pending nor Retry are
    /// skipped with a warning; an id already in flight is ignored (the
    /// double-click guard). Results always arrive through itemStateChanged, so
    /// every row always leaves the in-flight set again.
    void sendNow(qint64 outboxId);

  signals:
    void itemStateChanged(qint64 id, OutboxState state, QString error);

  private:
    /// Worker body on the QtConcurrent pool; reads the secret and drives the
    /// backend under the operation mutex, then posts the completion step.
    void runSend(qint64 outboxId, const OutgoingMessage &message, const QByteArray &rawMime);
    /// Queued on the service's (GUI) thread: parks the row in Retry (error =
    /// the backend message, attempts +1) and leaves the in-flight set.
    void finishFailure(qint64 outboxId, const MailError &error);
    /// Queued on the service's (GUI) thread: marks the row Sent (clearing
    /// last_error) and leaves the in-flight set.
    void finishSuccess(qint64 outboxId);
    /// Queues `step` on the service's (GUI) thread, where store access and the
    /// signal emission happen.
    void postStep(std::function<void()> step);

    OutboxStore &m_outbox;
    MailBackendFactory &m_factory;
    CredentialStore &m_credentials;
    // Serializes the workers' credential reads and backend factory usage.
    std::mutex m_operationMutex;
    // Guards the enqueue-registered account snapshots.
    QMutex m_snapshotMutex;
    QHash<AccountId, Account> m_snapshots;
    // Outbox ids with a send in flight: inserted on the calling thread (the
    // enqueue()/sendNow() caller), removed inside the service-thread (GUI,
    // via queued completion steps) — the single-threaded access semantics are
    // guaranteed by the service object's thread affinity, so no lock is used.
    QSet<qint64> m_inFlight;
    // Workers increment this on dispatch and decrement after their last member
    // access, so the destructor can wait for the m_* members to go quiet.
    std::atomic<int> m_activeWorkers;
};

} // namespace ningmail
