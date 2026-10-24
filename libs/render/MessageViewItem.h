#pragma once

#include <QFutureWatcher>
#include <QImage>
#include <QString>

#include <QtQuick/QQuickPaintedItem>

#include <QtQmlIntegration/qqmlintegration.h>

#include "LiteHtmlRenderer.h"

namespace ningmail {

/// One background render request (R73). The epoch travels with the result so
/// the finished handler can implement latest-wins: a result whose epoch no
/// longer matches the item's m_epoch is dropped without an update().
///
/// NOTE: the brief sketched QFutureWatcher<QImage>; the result type here
/// additionally carries the epoch (RenderJob) because latest-wins requires
/// matching results to their request. The watcher mechanism itself is exactly
/// the one from the brief.
struct RenderJob {
    quint64 epoch = 0;
    qreal scale = 1.0;
    QImage image;
};

/// QQuickPaintedItem that renders sanitized email HTML through
/// LiteHtmlRenderer on the QtConcurrent pool. GUI-thread discipline:
/// setHtml/geometryChange only copy state and enqueue a job; paint() only
/// blits the cached QImage; the finished handler (delivered on the GUI thread)
/// applies latest-wins and calls update().
class MessageViewItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString html READ html WRITE setHtml NOTIFY htmlChanged)
    Q_PROPERTY(int contentHeight READ contentHeight NOTIFY contentHeightChanged)
  public:
    explicit MessageViewItem(QQuickItem *parent = nullptr);

    QString html() const {
        return m_html;
    }
    void setHtml(const QString &html);

    int contentHeight() const {
        return m_contentHeight;
    }

    void paint(QPainter *painter) override;

  signals:
    void htmlChanged();
    void contentHeightChanged();

  protected:
    // Width change → re-render (ruling 3).
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

  private:
    void requestRender();
    qreal renderScale() const;

    QString m_html;
    // Latest request generation; also invalidates when html is reset to "".
    quint64 m_epoch = 0;
    int m_contentHeight = 0;
    QImage m_cache;
    QFutureWatcher<RenderJob> m_watcher;
};

} // namespace ningmail
