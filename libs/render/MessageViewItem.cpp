#include "MessageViewItem.h"

#include <QQuickWindow>

#include <QElapsedTimer>
#include <QPainter>

#include <QtConcurrent/QtConcurrentRun>

namespace ningmail {

MessageViewItem::MessageViewItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    // The result handler always runs on the GUI thread (QFutureWatcher delivers
    // via queued connection from the pool).
    connect(&m_watcher, &QFutureWatcher<RenderJob>::finished, this, [this]() {
        if (!m_watcher.future().isFinished()) {
            return;
        }
        const RenderJob job = m_watcher.result();
        // Latest-wins (R73): a stale result is dropped without update().
        if (job.epoch != m_epoch) {
            return;
        }
        m_cache = job.image;
        const qreal scale = job.scale > 0 ? job.scale : 1.0;
        // Logical height for QML layout (device pixels / dpr).
        const int logicalHeight = qRound(job.image.height() / scale);
        if (m_contentHeight != logicalHeight) {
            m_contentHeight = logicalHeight;
            emit contentHeightChanged();
        }
        update();
    });
}

void MessageViewItem::setHtml(const QString &html) {
    if (html == m_html) {
        return;
    }
    // GUI thread: copy only (R73); all parsing/painting happens in the job.
    m_html = html;
    emit htmlChanged();
    if (m_html.isEmpty()) {
        // Nothing to show: invalidate the cache synchronously and drop any
        // in-flight result through the epoch bump.
        ++m_epoch;
        m_cache = QImage();
        if (m_contentHeight != 0) {
            m_contentHeight = 0;
            emit contentHeightChanged();
        }
        update();
        return;
    }
    requestRender();
}

void MessageViewItem::paint(QPainter *painter) {
    // paint() blits the cache only — never renders synchronously (R73).
    if (m_cache.isNull()) {
        return;
    }
    // The cached image carries the render-time device pixel ratio, so
    // QPainter maps device pixels back to the item's logical coordinates.
    painter->drawImage(QPointF(0, 0), m_cache);
}

void MessageViewItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (m_html.isEmpty()) {
        return;
    }
    const int newWidth = qRound(newGeometry.width());
    const int oldWidth = qRound(oldGeometry.width());
    if (newWidth > 0 && newWidth != oldWidth) {
        requestRender(); // width change → re-render
    }
}

void MessageViewItem::requestRender() {
    const int w = qRound(width());
    if (w <= 0) {
        return; // laid out later; geometryChange re-arms the request
    }
    const quint64 epoch = ++m_epoch;
    const QString html = m_html;
    const qreal scale = renderScale();

    // The job struct is value-copied through the future; the epoch is stamped
    // at request time so late results cannot impersonate a newer request.
    QFuture<RenderJob> future = QtConcurrent::run([html, w, scale, epoch]() {
        // One renderer per job: LiteHtmlRenderer keeps no cross-call caching,
        // so concurrent jobs cannot race shared state.
        LiteHtmlRenderer renderer;
        RenderJob job;
        job.epoch = epoch;
        job.scale = scale;
        job.image = renderer.render(html, w, scale);
        return job;
    });
    m_watcher.setFuture(future);
}

qreal MessageViewItem::renderScale() const {
    const QQuickWindow *window = this->window();
    if (window != nullptr) {
        const qreal dpr = window->effectiveDevicePixelRatio();
        if (dpr > 0) {
            return dpr;
        }
    }
    return 1.0;
}

} // namespace ningmail
