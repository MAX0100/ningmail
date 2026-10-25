#pragma once

#include "app/core/MailBackend.h"

#include <QByteArray>

namespace ningmail {

/// Composes one RFC 5322/MIME plain-text message (R82). M1 scope: no
/// attachments (message.attachments, inReplyTo and references are ignored),
/// one text/plain part encoded as base64; every byte answered by the M2
/// extensions is documented in the .cpp.
class MimeBuilder {
  public:
    /// Builds the full message (headers + body) with CRLF line endings.
    static QByteArray build(const OutgoingMessage &message);
};

} // namespace ningmail
