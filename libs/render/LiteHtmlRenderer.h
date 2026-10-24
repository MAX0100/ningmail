#pragma once

#include <QImage>
#include <QMutex>
#include <QString>

namespace ningmail {

/// Sanitizes, lays out and paints email HTML into a QImage (task 10b, R73).
///
/// render() is call-anywhere code: it must not touch the GUI thread and keeps
/// no cross-call caching, so the same (html, width, scale) input always yields
/// the same image. Deterministic pipeline per call:
///   1. size gate (>1MB → 600x200 placeholder, lastError = "html-too-large")
///   2. HtmlSanitizer (network/remote content already stripped by T10a)
///   3. litehtml v0.10 parse + layout via QtDocumentContainer
///   4. paint into a QImage carrying `scale` as its device pixel ratio
/// The last state (lastHeight/lastError) is mutex-guarded because callers may
/// run concurrent renders on the QtConcurrent pool.
class LiteHtmlRenderer {
  public:
    /// `width` is the logical render width; `scale` is a device pixel ratio
    /// applied to the returned QImage (device pixels = logical * scale).
    QImage render(const QString &html, int width, qreal scale = 1.0);

    int lastHeight() const;
    QString lastError() const;

  private:
    QImage placeholderImage(int width, qreal scale) const;

    mutable QMutex m_stateMutex;
    int m_lastHeight = 0;
    QString m_lastError;
};

} // namespace ningmail
