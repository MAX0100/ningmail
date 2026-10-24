#pragma once

#include "app/core/MailError.h"
#include "app/core/Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace ningmail {

struct ParsedMessage {
    QString subject;
    MailAddress from;
    QList<MailAddress> to, cc, bcc;
    QDateTime date;
    QString html;
    QString plainText;
    QList<AttachmentMeta> attachments;
};

// Parses a raw RFC 822/MIME message (.eml) into a structured message.
class MimeParser {
  public:
    static Result<ParsedMessage> parse(const QByteArray &raw);
    static QString decodeHeader(const QByteArray &value);
    static QString decodeText(const QByteArray &data, const QString &charset);
};

} // namespace ningmail
