#include "DeviceLayoutWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QtMath>

#include <algorithm>

namespace zb {

namespace {
    constexpr int kCanvasPadding = 24;
    constexpr int kGap = 0;
    constexpr int kMinRectW = 90;
    constexpr int kMinRectH = 60;
    constexpr int kCornerRadius = 6;
    constexpr int kNumberBadgeSize = 26;
}

DeviceLayoutWidget::DeviceLayoutWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(380, 240);
    setMouseTracking(true);
    recalculate();
}

QSize DeviceLayoutWidget::sizeHint() const {
    return {460, 280};
}

QSize DeviceLayoutWidget::minimumSizeHint() const {
    return {380, 220};
}

void DeviceLayoutWidget::setLayout(ScreenLayout layout) {
    if (layout_ == layout && !dragging_) return;
    layout_ = layout;
    clientRect_ = positionForLayout(layout_);
    update();
}

void DeviceLayoutWidget::setConnected(bool connected) {
    connected_ = connected;
    update();
}

void DeviceLayoutWidget::setPeerName(const QString& name) {
    peerName_ = name;
    update();
}

void DeviceLayoutWidget::setLocalResolution(uint32_t w, uint32_t h) {
    if (w && h) {
        localW_ = w;
        localH_ = h;
    }
    recalculate();
}

void DeviceLayoutWidget::setPeerResolution(uint32_t w, uint32_t h) {
    if (w && h) {
        peerW_ = w;
        peerH_ = h;
    }
    recalculate();
}

void DeviceLayoutWidget::recalculate() {
    // Proportional sizing based on resolution, scaled to fit the widget.
    double refW = static_cast<double>(std::max(localW_, peerW_));
    double refH = static_cast<double>(std::max(localH_, peerH_));

    int availW = width() - kCanvasPadding * 2;
    int availH = height() - kCanvasPadding * 2;

    // Reserve space for both rectangles side by side with a gap.
    double sx = static_cast<double>(availW - kGap) / (refW * 2);
    double sy = static_cast<double>(availH - kGap) / (refH * 2);
    scale_ = std::min({sx, sy, 0.18});
    if (scale_ <= 0) scale_ = 0.08;

    int hostW = std::max(kMinRectW, static_cast<int>(localW_ * scale_));
    int hostH = std::max(kMinRectH, static_cast<int>(localH_ * scale_));
    int clientW = std::max(kMinRectW, static_cast<int>(peerW_ * scale_));
    int clientH = std::max(kMinRectH, static_cast<int>(peerH_ * scale_));

    // Host centered.
    int hx = (width() - hostW) / 2;
    int hy = (height() - hostH) / 2;
    hostRect_ = QRect(hx, hy, hostW, hostH);

    clientRect_ = positionForLayout(layout_);
    update();
}

QRect DeviceLayoutWidget::positionForLayout(ScreenLayout layout) const {
    int hw = hostRect_.width();
    int hh = hostRect_.height();
    int cw = std::max(kMinRectW, static_cast<int>(peerW_ * scale_));
    int ch = std::max(kMinRectH, static_cast<int>(peerH_ * scale_));

    int hx = hostRect_.x();
    int hy = hostRect_.y();

    // Align centers along the cross axis, and place adjacent on the main axis.
    switch (layout) {
        case ScreenLayout::RightOf:
            return QRect(hx + hw + kGap, hy + (hh - ch) / 2, cw, ch);
        case ScreenLayout::LeftOf:
            return QRect(hx - kGap - cw, hy + (hh - ch) / 2, cw, ch);
        case ScreenLayout::Above:
            return QRect(hx + (hw - cw) / 2, hy - kGap - ch, cw, ch);
        case ScreenLayout::Below:
            return QRect(hx + (hw - cw) / 2, hy + hh + kGap, cw, ch);
    }
    return hostRect_;
}

ScreenLayout DeviceLayoutWidget::inferLayout(const QRect& r) const {
    // Compute separation on each axis. A positive separation means the
    // client is on that side without overlapping the host.
    int sepRight = r.left() - hostRect_.right();    // >0: client to the right
    int sepLeft  = hostRect_.left() - r.right();    // >0: client to the left
    int sepBelow = r.top() - hostRect_.bottom();    // >0: client below
    int sepAbove = hostRect_.top() - r.bottom();    // >0: client above

    // Horizontal separation is the amount of clear space to the side where
    // the client actually is (negative means overlapping on that axis).
    int horizGap = std::max(sepRight, sepLeft);
    int vertGap  = std::max(sepBelow, sepAbove);

    // If the client is primarily separated horizontally, pick left/right;
    // otherwise pick above/below. When overlapping, use the axis with the
    // larger center-to-center distance.
    if (horizGap >= vertGap) {
        return (r.center().x() >= hostRect_.center().x())
                   ? ScreenLayout::RightOf
                   : ScreenLayout::LeftOf;
    } else {
        return (r.center().y() >= hostRect_.center().y())
                   ? ScreenLayout::Below
                   : ScreenLayout::Above;
    }
}

QRect DeviceLayoutWidget::snapToEdge(const QRect& freeRect) const {
    ScreenLayout layout = inferLayout(freeRect);
    int cw = freeRect.width();
    int ch = freeRect.height();

    int hx = hostRect_.x();
    int hy = hostRect_.y();
    int hw = hostRect_.width();
    int hh = hostRect_.height();

    int nx = freeRect.x();
    int ny = freeRect.y();

    switch (layout) {
        case ScreenLayout::RightOf:
            nx = hx + hw + kGap;
            ny = std::clamp(ny, kCanvasPadding, height() - ch - kCanvasPadding);
            break;
        case ScreenLayout::LeftOf:
            nx = hx - kGap - cw;
            ny = std::clamp(ny, kCanvasPadding, height() - ch - kCanvasPadding);
            break;
        case ScreenLayout::Above:
            nx = std::clamp(nx, kCanvasPadding, width() - cw - kCanvasPadding);
            ny = hy - kGap - ch;
            break;
        case ScreenLayout::Below:
            nx = std::clamp(nx, kCanvasPadding, width() - cw - kCanvasPadding);
            ny = hy + hh + kGap;
            break;
    }
    return QRect(nx, ny, cw, ch);
}

void DeviceLayoutWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Canvas background — light surface matching Word 2019 theme.
    p.fillRect(rect(), QColor(0xF3, 0xF2, 0xF1));

    // Subtle dot grid.
    p.setPen(QPen(QColor(0xE1, 0xDF, 0xDD), 1));
    for (int x = kCanvasPadding; x < width(); x += 24) {
        p.drawLine(x, kCanvasPadding, x, height() - kCanvasPadding);
    }
    for (int y = kCanvasPadding; y < height(); y += 24) {
        p.drawLine(kCanvasPadding, y, width() - kCanvasPadding, y);
    }

    // Apply pan offset to all monitor drawings.
    p.save();
    p.translate(panOffset_);

    auto drawMonitor = [&](const QRect& r, const QString& number,
                           const QString& title, const QString& subtitle,
                           bool isHost, bool isDragging) {
        // Shadow.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 38));
        p.drawRoundedRect(r.translated(0, 2), kCornerRadius, kCornerRadius);

        // Body.
        QColor fill = isHost ? QColor(0x00, 0x78, 0xD4)
                             : (isDragging ? QColor(0x40, 0x90, 0xE0)
                                           : QColor(0x2B, 0x6B, 0xB5));
        if (!isHost && !connected_) {
            fill = QColor(0xA1, 0x9F, 0x9D);
        }
        p.setBrush(fill);
        p.setPen(QPen(isDragging ? QColor(0xFF, 0xFF, 0xFF, 200)
                                 : QColor(0x60, 0x5E, 0x5C, 160), 1));
        p.drawRoundedRect(r, kCornerRadius, kCornerRadius);

        // Inner highlight border.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), kCornerRadius - 1, kCornerRadius - 1);

        // Number badge.
        QRect badge(r.x() + 8, r.y() + 8, kNumberBadgeSize, kNumberBadgeSize);
        p.setBrush(QColor(255, 255, 255, 230));
        p.setPen(Qt::NoPen);
        p.drawEllipse(badge);
        p.setPen(QColor(0x1A, 0x1A, 0x1A));
        QFont nf = p.font();
        nf.setBold(true);
        nf.setPointSize(10);
        p.setFont(nf);
        p.drawText(badge, Qt::AlignCenter, number);

        // Title & subtitle centered.
        p.setPen(QColor(255, 255, 255, 235));
        QFont tf = p.font();
        tf.setBold(true);
        tf.setPointSize(9);
        p.setFont(tf);

        QRect textRect = r.adjusted(8, kNumberBadgeSize + 12, -8, -8);
        QRect titleRect;
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   title, &titleRect);

        if (!subtitle.isEmpty()) {
            QFont sf = p.font();
            sf.setBold(false);
            sf.setPointSize(8);
            p.setFont(sf);
            p.setPen(QColor(255, 255, 255, 180));
            QRect subArea = textRect;
            subArea.setTop(titleRect.bottom() + 2);
            QRect subRect;
            p.drawText(subArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       subtitle, &subRect);
        }
    };

    // Draw host.
    QString hostTitle = QStringLiteral("控制端");
    QString hostSub = QString("%1×%2").arg(localW_).arg(localH_);
    drawMonitor(hostRect_, "1", hostTitle, hostSub, true, false);

    // Draw client.
    QString peerTitle = peerName_.isEmpty()
        ? (connected_ ? QStringLiteral("客户端") : QStringLiteral("客户端（未连接）"))
        : peerName_;
    QString peerSub = connected_
        ? QString("%1×%2").arg(peerW_).arg(peerH_)
        : QString();
    drawMonitor(clientRect_, "2", peerTitle, peerSub, false, dragging_);

    p.restore();

    // Hint text at bottom (not panned).
    p.setPen(QColor(0x60, 0x5E, 0x5C, 180));
    QFont hf = p.font();
    hf.setPointSize(8);
    hf.setBold(false);
    p.setFont(hf);
    p.drawText(rect().adjusted(0, 0, 0, -6),
               Qt::AlignHCenter | Qt::AlignBottom,
               QStringLiteral("拖动矩形设置位置，拖动空白处平移画布"));
}

void DeviceLayoutWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Check if click is on the client rect (translated by pan offset).
        QRect panClientRect = clientRect_.translated(panOffset_);
        if (panClientRect.contains(event->pos())) {
            dragging_ = true;
            dragOffset_ = event->pos() - panClientRect.topLeft();
            dragStartRect_ = clientRect_;
            setCursor(Qt::ClosedHandCursor);
            update();
            event->accept();
            return;
        }

        // Otherwise, start panning the canvas.
        panning_ = true;
        panStart_ = event->pos();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DeviceLayoutWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        // Drag the client rect relative to the pan-adjusted position.
        QPoint topLeft = event->pos() - dragOffset_ - panOffset_;
        int cw = clientRect_.width();
        int ch = clientRect_.height();

        int nx = std::clamp(topLeft.x(), kCanvasPadding - panOffset_.x(),
                            width() - cw - kCanvasPadding - panOffset_.x());
        int ny = std::clamp(topLeft.y(), kCanvasPadding - panOffset_.y(),
                            height() - ch - kCanvasPadding - panOffset_.y());
        clientRect_ = QRect(nx, ny, cw, ch);
        update();
        event->accept();
        return;
    }

    if (panning_ && (event->buttons() & Qt::LeftButton)) {
        panOffset_ += event->pos() - panStart_;
        panStart_ = event->pos();
        update();
        event->accept();
        return;
    }

    // Hover cursor.
    QRect panClientRect = clientRect_.translated(panOffset_);
    if (panClientRect.contains(event->pos())) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
    QWidget::mouseMoveEvent(event);
}

void DeviceLayoutWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;

        // Snap to nearest edge and determine final layout.
        QRect snapped = snapToEdge(clientRect_);
        ScreenLayout newLayout = inferLayout(snapped);

        bool changed = (newLayout != layout_);
        layout_ = newLayout;
        clientRect_ = snapped;
        update();

        QRect panClientRect = clientRect_.translated(panOffset_);
        setCursor(panClientRect.contains(event->pos()) ? Qt::OpenHandCursor
                                                       : Qt::ArrowCursor);
        if (changed) {
            emit layoutChanged(layout_);
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && panning_) {
        panning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DeviceLayoutWidget::resizeEvent(QResizeEvent*) {
    recalculate();
}

} // namespace zb
