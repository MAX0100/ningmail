#pragma once

#include "Types.h"

#include <QString>
#include <QStringList>
#include <QUrl>

namespace ningmail {

/// Task 15 (R93③): the five-key payload carried by a mailto: URL. `to` may
/// stay empty — RFC 6068 allows subject/body-only mailto links.
struct MailtoRequest {
    QStringList to;
    QStringList cc;
    QStringList bcc;
    QString subject;
    QString body;
};

/// Stateless mailto: parser. Recipients come from the URL path plus the `to`
/// query key (appended, not merged); cc/bcc/subject/body live in the query
/// with QUrlQuery FullyDecoded semantics — `%20` becomes a space while a `+`
/// keeps its literal meaning (not form encoding). Unknown query keys are
/// ignored and duplicate keys fall back to the first occurrence.
class MailtoParser {
  public:
    static Result<MailtoRequest> parse(const QUrl &url);
};

} // namespace ningmail
