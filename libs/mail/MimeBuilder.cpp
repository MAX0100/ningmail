#include "MimeBuilder.h"

#include <QDateTime>
#include <QUuid>

namespace ningmail {

namespace {

/// ASCII-only header values go out verbatim; anything else (and any control
/// character, which would break the line structure) becomes one RFC 2047
/// UTF-8/B encoded word.
bool needsEncoding(const QString &value) {
    for (const QChar ch : value) {
        if (ch.unicode() > 127 || ch == u'\r' || ch == u'\n') {
            return true;
        }
    }
    return false;
}

QString encode2047(const QString &value) {
    if (!needsEncoding(value)) {
        return value;
    }
    return QStringLiteral("=?UTF-8?B?") + QString::fromLatin1(value.toUtf8().toBase64()) +
           QStringLiteral("?=");
}

/// Header values are composed from user input: CR/LF inside them would inject
/// extra header lines, so they are dropped before use.
QString sanitizedHeaderText(const QString &value) {
    QString out = value;
    out.remove(u'\r');
    out.remove(u'\n');
    return out;
}

/// Whether an ASCII-only display name must travel inside a quoted-string: any
/// RFC 5322 specials that would break the atom, or leading/trailing whitespace.
bool needsQuotedString(const QString &name) {
    const QString specials = QStringLiteral("\"\\,:;<>()[]`@");
    for (const QChar ch : name) {
        if (specials.contains(ch)) {
            return true;
        }
    }
    const QChar first = name.isEmpty() ? QChar() : name.at(0);
    const QChar last = name.isEmpty() ? QChar() : name.at(name.size() - 1);
    return !(first.isNull() || (!first.isSpace() && !last.isSpace()));
}

/// Wraps a display name in an RFC 5322 quoted-string, backslash-escaping the
/// two characters the quoting defines (`"` and `\`).
QString quotedString(const QString &name) {
    QString out;
    out.reserve(name.size() + 2);
    out += u'"';
    for (const QChar ch : name) {
        if (ch == u'"' || ch == u'\\') {
            out += u'\\';
        }
        out += ch;
    }
    out += u'"';
    return out;
}

/// R53 display rule: a name renders as `name <address>`, an unnamed address as
/// the bare address. Non-ASCII names are 2047-encoded (header-safety first,
/// same rule as the subject); ASCII names with RFC 5322 specials (or padded
/// by whitespace) travel inside a quoted-string with `"`/`\` escaped.
QString formatAddress(const MailAddress &address) {
    const QString name = sanitizedHeaderText(address.name);
    const QString email = sanitizedHeaderText(address.address);
    if (name.isEmpty()) {
        return email;
    }
    QString rendered;
    if (needsEncoding(name)) {
        rendered = encode2047(name);
    } else {
        rendered = needsQuotedString(name) ? quotedString(name) : name;
    }
    return rendered + QStringLiteral(" <") + email + QStringLiteral(">");
}

/// Domain of the sender address for generated Message-Ids: the part after the
/// last '@', falling back to ningmail.local when the address has no domain.
QString messageDomainOf(const QString &senderAddress) {
    const int at = senderAddress.lastIndexOf(u'@');
    const QString domain = at >= 0 ? sanitizedHeaderText(senderAddress.mid(at + 1)) : QString();
    return domain.isEmpty() ? QStringLiteral("ningmail.local") : domain;
}

/// `<uuid@domain>` — MimeBuilder generates this only when the caller left
/// rfcMessageId empty (envelope consistency is anchored by the enqueue path,
/// which pre-generates the id and stores it in the outbox.rfc_message_id
/// column, so DB and MIME header can never drift apart).
QString generateMessageId(const QString &senderAddress) {
    return QStringLiteral("<") + QUuid::createUuid().toString(QUuid::WithoutBraces) +
           QStringLiteral("@") + messageDomainOf(senderAddress) + QStringLiteral(">");
}

/// UTF-8 base64 of the body, folded to at most 76 columns with CRLF (RFC 2045).
QByteArray encodeBody(const QString &bodyText) {
    const QByteArray base64 = bodyText.toUtf8().toBase64();
    QByteArray folded;
    folded.reserve(base64.size() + (base64.size() / 76 + 1) * 2);
    for (int offset = 0; offset < base64.size(); offset += 76) {
        folded += base64.mid(offset, 76);
        folded += "\r\n";
    }
    return folded;
}

} // namespace

QByteArray MimeBuilder::build(const OutgoingMessage &message) {
    // M1 notes (R82): message.attachments, inReplyTo and references are
    // deliberately ignored — the plain-text body is the whole message.
    // message.bcc must never reach the output: it stays local to the composer.

    // formatAddress sanitizes and 2047-encodes both sides of each address.
    const QString from =
        formatAddress(MailAddress{message.account.displayName, message.account.email});

    QStringList to;
    for (const MailAddress &address : message.to) {
        to.append(formatAddress(address));
    }
    QStringList cc;
    for (const MailAddress &address : message.cc) {
        cc.append(formatAddress(address));
    }

    const QString messageId = !message.rfcMessageId.isEmpty()
                                  ? message.rfcMessageId
                                  : generateMessageId(message.account.email);
    const QString date = QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date);

    QStringList headers;
    headers.append(QStringLiteral("From: %1").arg(from));
    // The receiver headers appear only when they have something to say
    // (R85): an empty To or Cc never emits a `Name: ` line with an empty
    // value.
    if (!to.isEmpty()) {
        headers.append(QStringLiteral("To: %1").arg(to.join(QStringLiteral(", "))));
    }
    if (!cc.isEmpty()) {
        headers.append(QStringLiteral("Cc: %1").arg(cc.join(QStringLiteral(", "))));
    }
    headers.append(
        QStringLiteral("Subject: %1").arg(encode2047(sanitizedHeaderText(message.subject))));
    headers.append(QStringLiteral("Date: %1").arg(date));
    headers.append(QStringLiteral("Message-ID: %1").arg(messageId));
    headers.append(QStringLiteral("MIME-Version: 1.0"));
    headers.append(QStringLiteral("Content-Type: text/plain; charset=UTF-8"));
    headers.append(QStringLiteral("Content-Transfer-Encoding: base64"));

    // CRLF line endings in the header block and one blank line (CRLF) between
    // the header block and the folded body.
    QByteArray raw;
    for (const QString &header : headers) {
        raw += header.toUtf8();
        raw += "\r\n";
    }
    raw += "\r\n";
    raw += encodeBody(message.bodyText);
    return raw;
}

} // namespace ningmail
