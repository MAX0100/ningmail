#include "MimeParser.h"

#include "Charset.h"

#include <QDateTime>
#include <QMap>

#include <exception>
#include <optional>

namespace ningmail {
namespace {

// Hard limit on multipart nesting; deeper structures fail with a Protocol error.
constexpr int kMaxDepth = 20;

struct FieldParams {
    // Main value of a structured field, lower case ("text/plain", "attachment", ...).
    QString value;
    // Parameter map with lower case keys and unquoted values.
    QMap<QString, QString> params;
};

// Splits on sep, ignoring separators inside quoted strings and, when asked,
// inside angle brackets (for address lists).
QList<QString> splitList(const QString &raw, QChar sep, bool respectAngles) {
    QList<QString> out;
    QString current;
    bool inQuotes = false;
    bool escaped = false;
    int angleDepth = 0;
    for (const QChar ch : raw) {
        if (escaped) {
            escaped = false;
            current += ch;
            continue;
        }
        if (inQuotes) {
            if (ch == u'\\') {
                escaped = true;
                current += ch;
                continue;
            }
            if (ch == u'"')
                inQuotes = false;
            current += ch;
            continue;
        }
        if (ch == u'"') {
            inQuotes = true;
            current += ch;
            continue;
        }
        if (respectAngles) {
            if (ch == u'<') {
                ++angleDepth;
                current += ch;
                continue;
            }
            if (ch == u'>' && angleDepth > 0) {
                --angleDepth;
                current += ch;
                continue;
            }
            if (ch == sep && angleDepth > 0) {
                current += ch;
                continue;
            }
        }
        if (ch == sep) {
            out.append(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    out.append(current);
    return out;
}

// Removes surrounding double quotes and backslash escapes from a parameter value.
QString unquote(const QString &value) {
    QString v = value.trimmed();
    if (v.size() < 2 || !v.startsWith(u'"') || !v.endsWith(u'"'))
        return v;
    v = v.mid(1, v.size() - 2);
    QString out;
    bool escaped = false;
    for (const QChar ch : v) {
        if (escaped || ch != u'\\') {
            out += ch;
            escaped = false;
        } else {
            escaped = true;
        }
    }
    if (escaped)
        out += u'\\';
    return out;
}

// Parses "value; param=x; param*" style fields (Content-Type, Content-Disposition).
FieldParams parseStructured(const QString &raw) {
    FieldParams result;
    QList<QString> segments = splitList(raw, u';', false);
    if (segments.isEmpty())
        return result;
    result.value = segments.takeFirst().trimmed().toLower();
    for (const QString &segment : segments) {
        const int eq = segment.indexOf(u'=');
        if (eq < 0)
            continue;
        const QString name = segment.left(eq).trimmed().toLower();
        const QString value = unquote(segment.mid(eq + 1));
        if (!name.isEmpty())
            result.params.insert(name, value);
    }
    return result;
}

// Splits a message or part block into its header block and body at the first
// empty line. Accepts both CRLF and bare LF conventions.
void splitHeaderBody(const QByteArray &raw, QByteArray &headerBlock, QByteArray &body) {
    for (int i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && i + 3 < raw.size() && raw[i + 2] == '\r' &&
            raw[i + 3] == '\n') {
            headerBlock = raw.left(i + 2);
            body = raw.mid(i + 4);
            return;
        }
        if (raw[i] == '\n' && raw[i + 1] == '\n') {
            headerBlock = raw.left(i + 1);
            body = raw.mid(i + 2);
            return;
        }
    }
    headerBlock = raw;
    body = QByteArray();
}

// Parses an unfolded header block into a lowercase-name map.
QMap<QString, QString> parseHeaders(const QByteArray &headerBlock) {
    QMap<QString, QString> headers;
    QList<QByteArray> lines;
    int start = 0;
    while (true) {
        const int nl = headerBlock.indexOf('\n', start);
        QByteArray line = headerBlock.mid(start, nl < 0 ? -1 : nl - start);
        if (!line.isEmpty() && line.endsWith('\r'))
            line.chop(1);
        lines.append(line);
        if (nl < 0)
            break;
        start = nl + 1;
    }

    QString currentName;
    QString currentValue;
    auto flush = [&] {
        if (!currentName.isEmpty())
            headers.insert(currentName.toLower(), currentValue.trimmed());
        currentName.clear();
        currentValue.clear();
    };
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) {
            flush();
            continue;
        }
        const char first = line.at(0);
        if (first == ' ' || first == '\t') {
            // Folded continuation: merge into the pending header value.
            if (!currentName.isEmpty())
                currentValue += u' ' + QString::fromLatin1(line.trimmed());
            continue;
        }
        flush();
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue; // Not a header line; skipped rather than failing the parse.
        currentName = QString::fromLatin1(line.left(colon)).trimmed();
        currentValue += QString::fromLatin1(line.mid(colon + 1));
    }
    flush();
    return headers;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Q-encoding payload (RFC 2047): '_' stands for space, "=XY" for one byte.
QByteArray decodeQPayload(const QString &payload) {
    const QByteArray in = payload.toLatin1();
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '_') {
            out.append(' ');
            continue;
        }
        if (c == '=' && i + 2 < in.size()) {
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.append(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.append(c);
    }
    return out;
}

QByteArray decodeQuotedPrintable(const QByteArray &in) {
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '=') {
            out.append(c);
            continue;
        }
        if (i + 1 >= in.size()) {
            out.append('=');
            break;
        }
        const char next = in[i + 1];
        if (next == '\r') {
            i += (i + 2 < in.size() && in[i + 2] == '\n') ? 2 : 1; // Soft line break.
            continue;
        }
        if (next == '\n') {
            i += 1;
            continue;
        }
        if (i + 2 < in.size()) {
            const int hi = hexValue(next);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.append(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.append('='); // Malformed sequence: keep verbatim.
    }
    return out;
}

QByteArray decodeTransfer(const QByteArray &data, const QString &encodingRaw) {
    const QString encoding = encodingRaw.trimmed().toLower();
    if (encoding == QLatin1String("base64"))
        return QByteArray::fromBase64(data);
    if (encoding == QLatin1String("quoted-printable"))
        return decodeQuotedPrintable(data);
    return data; // 7bit, 8bit, binary, anything unknown.
}

QByteArray percentDecode(const QString &value) {
    const QByteArray in = value.toLatin1();
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.append(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.append(in[i]);
    }
    return out;
}

// RFC 2231 extended parameter value: charset'lang'%percent-encoded text.
QString decodeRfc2231Value(const QString &value) {
    const int first = value.indexOf(u'\'');
    if (first < 0)
        return MimeParser::decodeText(percentDecode(value), QString());
    const int second = value.indexOf(u'\'', first + 1);
    if (second < 0)
        return value;
    const QString charset = value.left(first);
    return MimeParser::decodeText(percentDecode(value.mid(second + 1)), charset);
}

QString stripContentId(const QString &raw) {
    QString v = raw.trimmed();
    if (v.startsWith(u'<'))
        v.remove(0, 1);
    if (v.endsWith(u'>'))
        v.chop(1);
    return v.trimmed();
}

QList<MailAddress> parseAddresses(const QString &raw) {
    QList<MailAddress> result;
    for (const QString &token : splitList(raw, u',', true)) {
        MailAddress address;
        const QString trimmed = token.trimmed();
        if (trimmed.isEmpty())
            continue;
        const int lt = trimmed.indexOf(u'<');
        if (lt >= 0) {
            const int gt = trimmed.indexOf(u'>', lt);
            address.name = unquote(trimmed.left(lt));
            address.address = trimmed.mid(lt + 1, gt > lt ? gt - lt - 1 : -1).trimmed();
        } else {
            address.address = unquote(trimmed);
        }
        if (!address.name.isEmpty() || !address.address.isEmpty())
            result.append(address);
    }
    return result;
}

QDateTime parseDate(const QString &raw) {
    const QDateTime parsed = QDateTime::fromString(raw.trimmed(), Qt::RFC2822Date);
    if (parsed.isValid())
        return parsed;
    return QDateTime::currentDateTimeUtc();
}

// Returns the index just past an encoded word starting at start, or -1.
int encodedWordEnd(const QString &in, int start) {
    const int charsetEnd = in.indexOf(u'?', start + 2);
    if (charsetEnd <= start + 2)
        return -1;
    const int encEnd = in.indexOf(u'?', charsetEnd + 1);
    if (encEnd != charsetEnd + 2)
        return -1; // Encoding must be exactly one character.
    const int payloadEnd = in.indexOf(u'?', encEnd + 1);
    if (payloadEnd < 0 || payloadEnd + 1 >= in.size() || in[payloadEnd + 1] != u'=')
        return -1;
    return payloadEnd + 2;
}

QString decodeEncodedWord(const QString &word) {
    if (word.size() < 6)
        return word;
    const int charsetStart = 2;
    const int charsetEnd = word.indexOf(u'?', charsetStart);
    if (charsetEnd < 0)
        return word;
    const int encEnd = word.indexOf(u'?', charsetEnd + 1);
    if (encEnd < 0 || encEnd != charsetEnd + 2)
        return word;
    const QChar encoding = word[charsetEnd + 1];
    const int payloadEnd = word.lastIndexOf(u'?');
    if (payloadEnd <= encEnd + 1)
        return word;
    const QString charsetToken = word.mid(charsetStart, charsetEnd - charsetStart);
    const int langSep = charsetToken.indexOf(u'\'');
    const QString charset = langSep >= 0 ? charsetToken.left(langSep) : charsetToken;
    const QString payload = word.mid(encEnd + 1, payloadEnd - (encEnd + 1));
    if (encoding == u'B' || encoding == u'b')
        return MimeParser::decodeText(QByteArray::fromBase64(payload.toLatin1()), charset);
    if (encoding == u'Q' || encoding == u'q')
        return MimeParser::decodeText(decodeQPayload(payload), charset);
    return word; // Unknown encoding: leave the word as-is.
}

} // namespace

static QString decodeHeaderText(const QString &in) {
    QString out;
    int pendingSpaceStart = -1;
    bool lastWasEncodedWord = false;
    int i = 0;
    while (i < in.size()) {
        const bool wordStart = in[i] == u'=' && i + 1 < in.size() && in[i + 1].toLatin1() == '?';
        if (wordStart) {
            const int end = encodedWordEnd(in, i);
            if (end > 0) {
                // Whitespace between adjacent encoded words is dropped; otherwise
                // the pending whitespace belongs to plain text and is kept.
                if (pendingSpaceStart >= 0 && !lastWasEncodedWord)
                    out += in.mid(pendingSpaceStart, i - pendingSpaceStart);
                out += decodeEncodedWord(in.mid(i, end - i));
                pendingSpaceStart = -1;
                i = end;
                lastWasEncodedWord = true;
                continue;
            }
        }
        if (in[i].isSpace()) {
            if (pendingSpaceStart < 0)
                pendingSpaceStart = i;
        } else {
            if (pendingSpaceStart >= 0) {
                out += in.mid(pendingSpaceStart, i - pendingSpaceStart);
                pendingSpaceStart = -1;
            }
            out += in[i];
            lastWasEncodedWord = false;
        }
        ++i;
    }
    if (pendingSpaceStart >= 0)
        out += in.mid(pendingSpaceStart, in.size() - pendingSpaceStart);
    return out;
}

QString MimeParser::decodeHeader(const QByteArray &rawValue) {
    return decodeHeaderText(QString::fromLatin1(rawValue));
}

QString MimeParser::decodeText(const QByteArray &data, const QString &charset) {
    return Charset::decode(data, charset);
}

namespace {

// Finds a "--boundary" line (with optional closing "--") starting at or after
// from. Requires the line to start at the beginning of a body or line, and to
// be terminated by a line break or the end of the data.
bool findBoundaryLine(const QByteArray &body, int from, const QByteArray &boundaryLine,
                      bool &closing, int &markerPos, int &lineEnd) {
    for (int idx = body.indexOf(boundaryLine, from); idx >= 0;
         idx = body.indexOf(boundaryLine, idx + static_cast<int>(boundaryLine.size()))) {
        if (idx > 0 && body[idx - 1] != '\n')
            continue;
        int after = idx + static_cast<int>(boundaryLine.size());
        closing = body.mid(after, 2) == QByteArray("--");
        if (closing)
            after += 2;
        if (after < body.size()) {
            if (body[after] == '\r') {
                if (after + 1 >= body.size() || body[after + 1] != '\n')
                    continue;
                after += 2;
            } else if (body[after] == '\n') {
                after += 1;
            } else {
                continue;
            }
        }
        markerPos = idx;
        lineEnd = after;
        return true;
    }
    return false;
}

bool splitMultipart(const QByteArray &body, const QString &boundary, QList<QByteArray> &parts,
                    MailError &error) {
    const QByteArray boundaryLine = "--" + boundary.toUtf8();
    const MailError missingEnd{ErrorCode::Protocol,
                               QString::fromUtf8("multipart without terminating boundary"),
                               QString()};
    // contentStart: bytes after the boundary line that opened the part currently
    // being collected; -1 means no part is open (preamble / between parts).
    int contentStart = -1;
    int seek = 0;
    while (true) {
        bool closing = false;
        int markerPos = -1;
        int lineEnd = -1;
        if (!findBoundaryLine(body, seek, boundaryLine, closing, markerPos, lineEnd))
            return error = missingEnd, false;
        if (contentStart < 0) {
            if (closing)
                return true;        // Multipart with no parts at all.
            contentStart = lineEnd; // This boundary opens the next part.
            seek = lineEnd;
            continue;
        }
        // The current part ends right before this boundary line.
        QByteArray content = body.mid(contentStart, markerPos - contentStart);
        if (content.endsWith("\r\n"))
            content.chop(2);
        else if (content.endsWith('\n'))
            content.chop(1);
        parts.append(content);
        if (closing)
            return true;
        contentStart = lineEnd;
        seek = lineEnd;
    }
}

QString resolveFilename(const FieldParams &contentType, const FieldParams &disposition) {
    const QString extension = disposition.params.value(QLatin1String("filename*"));
    if (!extension.isEmpty())
        return decodeRfc2231Value(extension);
    const QString plain = disposition.params.value(QLatin1String("filename"));
    if (!plain.isEmpty())
        return plain;
    return contentType.params.value(QLatin1String("name"));
}

struct PartContent {
    QMap<QString, QString> headers;
    QByteArray body;
};

PartContent parsePartBlock(const QByteArray &block) {
    PartContent part;
    QByteArray headerBlock;
    splitHeaderBody(block, headerBlock, part.body);
    part.headers = parseHeaders(headerBlock);
    return part;
}

std::optional<MailError> processPart(const QMap<QString, QString> &headers, const QByteArray &body,
                                     int depth, bool forceAttachment, ParsedMessage &out);

void appendAttachment(const QMap<QString, QString> &headers, const FieldParams &contentType,
                      const FieldParams &disposition, const QByteArray &data, ParsedMessage &out) {
    AttachmentMeta attachment;
    attachment.filename = resolveFilename(contentType, disposition);
    attachment.mimeType = contentType.value;
    attachment.size = data.size();
    attachment.contentId = stripContentId(headers.value(QLatin1String("content-id")));
    attachment.isInline = disposition.value == QLatin1String("inline");
    out.attachments.append(attachment);
}

std::optional<MailError> processLeafPart(const QMap<QString, QString> &headers,
                                         const FieldParams &contentType,
                                         const FieldParams &disposition, const QByteArray &body,
                                         bool forceAttachment, ParsedMessage &out) {
    const QString mime = contentType.value;
    const QByteArray data =
        decodeTransfer(body, headers.value(QLatin1String("content-transfer-encoding")));

    const bool isAttachment = forceAttachment || disposition.value == QLatin1String("attachment") ||
                              !mime.startsWith(QLatin1String("text/"));
    if (isAttachment) {
        appendAttachment(headers, contentType, disposition, data, out);
        return std::nullopt;
    }

    const QString charset = contentType.params.value(QLatin1String("charset"));
    const QString text = MimeParser::decodeText(data, charset);
    if (mime == QLatin1String("text/html")) {
        if (out.html.isEmpty())
            out.html = text;
    } else if (out.plainText.isEmpty()) {
        out.plainText = text;
    }
    return std::nullopt;
}

std::optional<MailError> processPart(const QMap<QString, QString> &headers, const QByteArray &body,
                                     int depth, bool forceAttachment, ParsedMessage &out) {
    const FieldParams contentType = parseStructured(headers.value(QLatin1String("content-type")));
    const FieldParams disposition =
        parseStructured(headers.value(QLatin1String("content-disposition")));
    const QString mime =
        contentType.value.isEmpty() ? QLatin1String("text/plain") : contentType.value;

    if (mime.startsWith(QLatin1String("multipart/"))) {
        if (depth > kMaxDepth)
            return MailError{
                ErrorCode::Protocol,
                QString::fromUtf8("multipart nesting exceeds depth limit of %1").arg(kMaxDepth),
                QString()};
        const QString boundary = contentType.params.value(QLatin1String("boundary"));
        if (boundary.isEmpty())
            return MailError{ErrorCode::Protocol, QString::fromUtf8("multipart without boundary"),
                             QString()};
        QList<QByteArray> partBlocks;
        MailError splitError;
        if (!splitMultipart(body, boundary, partBlocks, splitError))
            return splitError;
        const QString subtype = mime.mid(QString::fromUtf8("multipart/").size());

        for (int index = 0; index < partBlocks.size(); ++index) {
            const PartContent part = parsePartBlock(partBlocks.at(index));
            const int childDepth = depth + 1;
            if (subtype == QLatin1String("alternative")) {
                if (auto error = processPart(part.headers, part.body, childDepth, false, out))
                    return error;
                continue;
            }
            if (subtype == QLatin1String("related")) {
                if (index == 0) {
                    if (auto error = processPart(part.headers, part.body, childDepth, false, out))
                        return error;
                    continue;
                }
                const bool hasContentId =
                    !part.headers.value(QLatin1String("content-id")).isEmpty();
                if (auto error =
                        processPart(part.headers, part.body, childDepth, hasContentId, out))
                    return error;
                continue;
            }
            // multipart/mixed and siblings: text parts feed the body, anything
            // else is a normal attachment.
            const FieldParams childType =
                parseStructured(part.headers.value(QLatin1String("content-type")));
            const QString childMime =
                childType.value.isEmpty() ? QLatin1String("text/plain") : childType.value;
            const bool childIsAttachment = !childMime.startsWith(QLatin1String("multipart/")) &&
                                           !childMime.startsWith(QLatin1String("text/"));
            if (auto error =
                    processPart(part.headers, part.body, childDepth, childIsAttachment, out))
                return error;
        }
        return std::nullopt;
    }

    return processLeafPart(headers, contentType, disposition, body, forceAttachment, out);
}

} // namespace

Result<ParsedMessage> MimeParser::parse(const QByteArray &raw) {
    try {
        if (raw.isEmpty())
            return Result<ParsedMessage>::failure(
                MailError{ErrorCode::Protocol, QString::fromUtf8("empty message"), QString()});

        QByteArray headerBlock;
        QByteArray body;
        splitHeaderBody(raw, headerBlock, body);
        const QMap<QString, QString> headers = parseHeaders(headerBlock);

        ParsedMessage message;
        message.subject = decodeHeaderText(headers.value(QLatin1String("subject")));
        message.date = parseDate(headers.value(QLatin1String("date")));
        const QList<MailAddress> fromList = parseAddresses(headers.value(QLatin1String("from")));
        if (!fromList.isEmpty())
            message.from = fromList.first();
        message.to = parseAddresses(headers.value(QLatin1String("to")));
        message.cc = parseAddresses(headers.value(QLatin1String("cc")));
        message.bcc = parseAddresses(headers.value(QLatin1String("bcc")));

        if (auto error = processPart(headers, body, 1, false, message))
            return Result<ParsedMessage>::failure(*error);
        return Result<ParsedMessage>::success(std::move(message));
    } catch (const std::exception &) {
        return Result<ParsedMessage>::failure(
            MailError{ErrorCode::Protocol, QString::fromUtf8("unhandled exception while parsing"),
                      QString()});
    } catch (...) {
        return Result<ParsedMessage>::failure(
            MailError{ErrorCode::Protocol, QString::fromUtf8("unhandled exception while parsing"),
                      QString()});
    }
}

} // namespace ningmail
