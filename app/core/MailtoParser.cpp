#include "MailtoParser.h"

#include "MailError.h"

#include <QUrlQuery>

#include <optional>

namespace ningmail {

namespace {

/// Comma-separated recipient string -> trimmed list without empty segments.
/// Plain split() keeps whitespace-only segments, which then fail the trim
/// check and are dropped here instead.
QStringList splitRecipients(const QString &raw) {
    QStringList recipients;
    for (const QString &part : raw.split(u',')) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) {
            recipients.append(trimmed);
        }
    }
    return recipients;
}

/// First occurrence of a query key, fully decoded; later duplicates and
/// unknown keys are simply skipped by the miss in the loop. (Malformed or
/// truncated `%` escapes are QUrl's business: in FullyDecoded mode Qt
/// substitutes them with the literal text instead of erroring.)
std::optional<QString> firstQueryValue(const QUrlQuery &query, const QString &key) {
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (item.first == key) {
            return item.second;
        }
    }
    return std::nullopt;
}

} // namespace

Result<MailtoRequest> MailtoParser::parse(const QUrl &url) {
    // R93③: the mailto scheme is mandatory. main.cpp resolves user input
    // through QUrl::fromUserInput first, so `url` may still be any scheme.
    if (url.scheme() != QLatin1String("mailto")) {
        return Result<MailtoRequest>::failure(
            MailError{ErrorCode::Protocol, QStringLiteral("Unsupported scheme"),
                      QStringLiteral("The mailto parser expects mailto: URLs")});
    }

    MailtoRequest request;
    const QUrlQuery query(url);
    // Recipients: the URL path first, then every (first) query `to` value.
    request.to = splitRecipients(url.path());
    if (const std::optional<QString> to = firstQueryValue(query, QStringLiteral("to"))) {
        request.to.append(splitRecipients(*to));
    }
    if (const std::optional<QString> cc = firstQueryValue(query, QStringLiteral("cc"))) {
        request.cc = splitRecipients(*cc);
    }
    if (const std::optional<QString> bcc = firstQueryValue(query, QStringLiteral("bcc"))) {
        request.bcc = splitRecipients(*bcc);
    }
    if (const std::optional<QString> subject = firstQueryValue(query, QStringLiteral("subject"))) {
        request.subject = *subject;
    }
    if (const std::optional<QString> body = firstQueryValue(query, QStringLiteral("body"))) {
        request.body = *body;
    }
    return Result<MailtoRequest>::success(std::move(request));
}

} // namespace ningmail
