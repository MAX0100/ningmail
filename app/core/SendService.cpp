#include "SendService.h"

#include "CredentialStore.h"
#include "MimeBuilder.h"

#include <QDateTime>
#include <QMetaObject>
#include <QThread>
#include <QThreadPool>
#include <QUuid>

namespace ningmail {

namespace {

/// The snapshot-missing failure is a wiring error (enqueue never ran for this
/// account on this service), so it parks the row in Failed — a re-send without
/// a snapshot would deterministically fail again.
const char *const kSnapshotMissingError = "account snapshot unavailable";

/// `<uuid@domain>` with domain = the account address after '@', falling back
/// to "ningmail.local". Mirrored from MimeBuilder's generator; enqueue keeps
/// this exact value in the rfc_message_id column AND passes it into
/// MimeBuilder::build, so column and Message-ID header can never drift apart
/// (R82).
QString generateMessageId(const QString &senderAddress) {
    const int at = senderAddress.lastIndexOf(u'@');
    const QString domain = at >= 0 ? senderAddress.mid(at + 1) : QString();
    return QStringLiteral("<") + QUuid::createUuid().toString(QUuid::WithoutBraces) +
           QStringLiteral("@") + (domain.isEmpty() ? QStringLiteral("ningmail.local") : domain) +
           QStringLiteral(">");
}

/// The stored error text carries the backend message; the detail fills in when
/// the message is empty.
QString errorText(const MailError &error) {
    return !error.message.isEmpty() ? error.message : error.detail;
}

} // namespace

SendService::SendService(OutboxStore &outbox, MailBackendFactory &factory,
                         CredentialStore &credentials, QObject *parent)
    : QObject(parent), m_outbox(outbox), m_factory(factory), m_credentials(credentials),
      m_activeWorkers(0) {
    // The itemStateChanged payload types must survive queued cross-thread
    // delivery (idempotent).
    qRegisterMetaType<OutboxState>();
}

SendService::~SendService() {
    // Every worker runs unchallenged to its completion step (its results are
    // never dropped), so waiting for the counter means no thread can touch
    // m_* members any more; pending queued steps die with the QObject.
    while (m_activeWorkers.load(std::memory_order_acquire) != 0) {
        QThread::msleep(1);
    }
}

qint64 SendService::enqueue(const OutgoingMessage &message) {
    // A missing id is generated here, not inside MimeBuilder, so the same
    // value lands in the rfc_message_id column and the Message-ID header.
    OutgoingMessage composed = message;
    if (composed.rfcMessageId.isEmpty()) {
        composed.rfcMessageId = generateMessageId(composed.account.email);
    }

    OutboxItem item;
    item.accountId = composed.account.id;
    item.recipients = composed.to;
    item.cc = composed.cc;
    item.subject = composed.subject;
    item.bodyText = composed.bodyText;
    item.state = OutboxState::Pending;
    item.rfcMessageId = composed.rfcMessageId;
    item.rawMime = MimeBuilder::build(composed);

    // Account snapshot for the later sendNow (R81): remembered even when the
    // store rejects the row, harmless either way (account ids repeat).
    {
        QMutexLocker lock(&m_snapshotMutex);
        m_snapshots.insert(composed.account.id, composed.account);
    }

    const auto saved = m_outbox.save(item);
    if (!saved.ok()) {
        return -1;
    }
    return *saved.value;
}

void SendService::sendNow(qint64 outboxId) {
    // Single-flight set (R81): a second click for the same id is swallowed
    // before any store or backend work happens. The id leaves the set in the
    // completion step, on every path (success, failure, missing row).
    if (m_inFlight.contains(outboxId)) {
        return;
    }
    m_inFlight.insert(outboxId);

    const auto item = m_outbox.get(outboxId);
    if (!item.ok()) {
        qWarning() << "SendService: outbox item" << outboxId
                   << "disappeared before sending:" << item.error.message;
        m_inFlight.remove(outboxId);
        return;
    }

    // R81: only Pending/Retry rows are sendable; anything else (Sent, Sending,
    // Failed) is skipped with a warning and stays untouched.
    if (item.value->state != OutboxState::Pending && item.value->state != OutboxState::Retry) {
        qWarning() << "SendService: skipping outbox item" << outboxId << "in state"
                   << static_cast<int>(item.value->state);
        m_inFlight.remove(outboxId);
        return;
    }

    const MailError sendingError = m_outbox.updateState(outboxId, OutboxState::Sending);
    if (sendingError) {
        qWarning() << "SendService: failed to mark outbox item" << outboxId
                   << "Sending:" << sendingError.message << sendingError.detail;
        m_inFlight.remove(outboxId);
        return;
    }
    emit itemStateChanged(outboxId, OutboxState::Sending, QString());

    // The snapshot must exist: enqueue registered it for this account (R81).
    Account snapshot;
    {
        QMutexLocker lock(&m_snapshotMutex);
        snapshot = m_snapshots.value(item.value->accountId);
    }
    if (snapshot.id != item.value->accountId) {
        const QString error = QString::fromUtf8(kSnapshotMissingError);
        const MailError failedError = m_outbox.updateState(outboxId, OutboxState::Failed, error);
        if (failedError) {
            qWarning() << "SendService: failed to mark outbox item" << outboxId
                       << "Failed:" << failedError.message << failedError.detail;
        }
        emit itemStateChanged(outboxId, OutboxState::Failed, error);
        m_inFlight.remove(outboxId);
        return;
    }

    // The worker gets everything by value: it never re-reads the store (all
    // store access stays on the GUI thread) and only touches the members it
    // needs under the operation mutex (T8 discipline from SyncService).
    OutgoingMessage message;
    message.account = snapshot;
    message.to = item.value->recipients;
    message.cc = item.value->cc;
    message.subject = item.value->subject;
    message.bodyText = item.value->bodyText;
    message.rfcMessageId = item.value->rfcMessageId;
    const QByteArray rawMime = item.value->rawMime;

    m_activeWorkers.fetch_add(1, std::memory_order_relaxed);
    // QThreadPool::start (Qt) instead of QtConcurrent::run: the future is not
    // tracked — every completion reaches the GUI thread through the queued
    // steps and the destructor waits on m_activeWorkers.
    QThreadPool::globalInstance()->start([this, outboxId, message = std::move(message), rawMime]() {
        runSend(outboxId, message, rawMime);
        // Last member access: the destructor waits on this counter.
        m_activeWorkers.fetch_sub(1, std::memory_order_release);
    });
}

void SendService::runSend(qint64 outboxId, const OutgoingMessage &message,
                          const QByteArray &rawMime) {
    {
        // One exclusive run: credential read + factory create + connect +
        // send + appendSent never overlap with another worker (R81).
        const std::scoped_lock lock(m_operationMutex);

        // The secret is read inside the worker (same as SyncService::fetchBody,
        // with m_operationMutex serializing the store access).
        const std::optional<QString> secret = m_credentials.read(message.account.credentialRef);
        if (!secret.has_value()) {
            const MailError failure{ErrorCode::Auth, QString::fromUtf8("Credential not found"),
                                    QString::fromUtf8("missing credential")};
            postStep([this, outboxId, failure]() { finishFailure(outboxId, failure); });
            return;
        }

        std::unique_ptr<MailBackend> backend = m_factory.create(message.account);
        const MailError connectError = backend->connect(message.account, *secret);
        if (connectError) {
            postStep(
                [this, outboxId, failure = connectError]() { finishFailure(outboxId, failure); });
            return;
        }

        // Backend errors pass through untouched (R51): the failure moves the
        // row to Retry with the backend message and bumps attempts exactly
        // once. M1 has no retry scheduler (next_retry_at stays unset).
        const MailError sendError = backend->send(message);
        if (sendError) {
            postStep([this, outboxId, failure = sendError]() { finishFailure(outboxId, failure); });
            return;
        }

        // Success takes the sent-copy detour first: appendSent records the
        // raw MIME with the backend, then the row lands in Sent.
        const MailError appendError = backend->appendSent(message, rawMime);
        if (appendError) {
            postStep(
                [this, outboxId, failure = appendError]() { finishFailure(outboxId, failure); });
            return;
        }
    }

    postStep([this, outboxId]() { finishSuccess(outboxId); });
}

void SendService::finishFailure(qint64 outboxId, const MailError &error) {
    // Every result lands on the API thread, so the in-flight set always
    // empties again (R81: 成功/失败都要离场).
    m_inFlight.remove(outboxId);
    // Defensive (R85): if the row left the table under us (only an external
    // remove can do this today), the state report would be a lie — the callers
    // cannot receive a state the row no longer carries. Skip the emit, the
    // warning explains why the queue view may not match the last click.
    const QString text = errorText(error);
    const MailError bumpError = m_outbox.bumpAttempts(outboxId);
    if (bumpError) {
        qWarning() << "SendService: outbox item" << outboxId
                   << "left the store during the failed send, dropping the Retry report:"
                   << bumpError.message << bumpError.detail;
        return;
    }
    const MailError updateError = m_outbox.updateState(outboxId, OutboxState::Retry, text);
    if (updateError) {
        qWarning() << "SendService: outbox item" << outboxId
                   << "state update to Retry failed, dropping the Retry report:"
                   << updateError.message << updateError.detail;
        return;
    }
    emit itemStateChanged(outboxId, OutboxState::Retry, text);
}

void SendService::finishSuccess(qint64 outboxId) {
    m_inFlight.remove(outboxId);
    // Same defensive rule as finishFailure: a vanished/unsavable row cannot
    // honestly report Sent, so the corresponding signal is not emitted.
    if (m_outbox.updateState(outboxId, OutboxState::Sent, QString())) {
        qWarning() << "SendService: outbox item" << outboxId
                   << "state update to Sent failed, dropping the Sent report";
        return;
    }
    // An empty error clears last_error on the way to Sent.
    emit itemStateChanged(outboxId, OutboxState::Sent, QString());
}

void SendService::postStep(std::function<void()> step) {
    // Result delivery happens on the service's (GUI) thread; queued
    // invocations targeted at a destroyed service are removed by the event
    // machinery, which also makes destruction safe while steps are pending.
    QMetaObject::invokeMethod(this, std::move(step), Qt::QueuedConnection);
}

} // namespace ningmail
