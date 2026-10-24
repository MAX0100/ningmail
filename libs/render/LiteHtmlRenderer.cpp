#include "LiteHtmlRenderer.h"

#include "HtmlSanitizer.h"
#include "litehtml_qt/QtDocumentContainer.h"

#include <QElapsedTimer>
#include <QPainter>

#include <litehtml.h>

namespace ningmail {
namespace {

// Placeholder geometry for rejected input per the ruling (600x200).
constexpr int kPlaceholderWidth = 600;
constexpr int kPlaceholderHeight = 200;
// Gate for raw HTML input size (bytes, ruling 7).
constexpr qint64 kMaxHtmlBytes = 1024 * 1024;

} // namespace

QImage LiteHtmlRenderer::placeholderImage(int width, qreal scale) const {
    const int deviceWidth = qRound(width * scale);
    QImage image(QSize(deviceWidth, qRound(kPlaceholderHeight * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    return image;
}

QImage LiteHtmlRenderer::render(const QString &html, int width, qreal scale) {
    if (!(scale > 0.0)) {
        scale = 1.0;
    }
    if (width <= 0) {
        width = kPlaceholderWidth;
    }

    // 1. Size gate: drop the request before any parsing or sanitizing work.
    const QByteArray rawHtml = html.toUtf8();
    if (rawHtml.size() > kMaxHtmlBytes) {
        QMutexLocker lock(&m_stateMutex);
        m_lastError = QStringLiteral("html-too-large");
        m_lastHeight = qRound(kPlaceholderHeight * scale);
        return placeholderImage(width, scale);
    }

    // 2. Sanitize once at this call site — the single sanitization point of
    // the whole body pipeline, immediately before parse: deeper than the
    // brief's "inside MessageViewItem::setHtml" wording, so the reader view can
    // carry raw html and it is still impossible to reach litehtml unsanitized.
    // Both the <pre>-wrapped plain-text path (AppController::fillReaderBody)
    // and builder HTML pass through here.
    const QString sanitized = HtmlSanitizer::sanitize(html).html;

    // 3+4. Layout with the renderer's own Qt container, then paint. The
    // container must outlive every doc call (litehtml::document stores the
    // container pointer); litehtml::document owns the fonts created through it
    // and releases them in its destructor, so no font or image cache survives
    // container must outlive every doc call AND the doc release: a destroyed
    // container would make ~document's delete_font call hit a dangling
    // vtable ("pure virtual method called"). Declaring container first means
    // it is destroyed last.
    QtDocumentContainer container(width, scale);
    litehtml::document::ptr doc;
    bool ok = false;
    doc = litehtml::document::createFromString(litehtml::estring(sanitized.toStdString()),
                                               &container);
    if (doc) {
        // Deterministic layout width; the viewport height only feeds media
        // queries (see QtDocumentContainer::get_viewport).
        doc->render(static_cast<float>(width));
        ok = true;
    }

    if (!ok || doc->height() <= 0.0f) {
        QMutexLocker lock(&m_stateMutex);
        m_lastError = QStringLiteral("render-failed");
        m_lastHeight = qRound(kPlaceholderHeight * scale);
        return placeholderImage(width, scale);
    }

    // Height cap (ruling 7): clamp the layout before allocating the image.
    float layoutHeight = doc->height();
    if (layoutHeight > static_cast<float>(kMaxRenderHeight)) {
        layoutHeight = static_cast<float>(kMaxRenderHeight);
    }

    const QSize deviceSize(qRound(width * scale), qRound(layoutHeight * scale));
    QImage image(deviceSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::white);

    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        // hdc carries the QPainter*; drawing coordinates are logical because
        // the painter scales by the QImage device pixel ratio.
        doc->draw(reinterpret_cast<litehtml::uint_ptr>(&painter), 0, 0, nullptr);
    }
    // `doc` is destroyed (releasing its fonts) before `container`.

    QMutexLocker lock(&m_stateMutex);
    m_lastError.clear();
    m_lastHeight = image.height();
    return image;
}

int LiteHtmlRenderer::lastHeight() const {
    QMutexLocker lock(&m_stateMutex);
    return m_lastHeight;
}

QString LiteHtmlRenderer::lastError() const {
    QMutexLocker lock(&m_stateMutex);
    return m_lastError;
}

} // namespace ningmail
