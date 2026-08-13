#pragma once

#include "../core/ScreenLayout.h"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>

namespace zb {

// A Windows-Settings-style display arrangement canvas. The local machine is
// shown as a fixed rectangle in the center; the peer device can be dragged
// to any side (left / right / above / below) to define the relative layout.
// On release the peer snaps to the nearest edge and layoutChanged() is emitted.
class DeviceLayoutWidget : public QWidget {
    Q_OBJECT
public:
    explicit DeviceLayoutWidget(QWidget* parent = nullptr);

    void setLayout(ScreenLayout layout);
    ScreenLayout layout() const { return layout_; }

    void setConnected(bool connected);
    void setPeerName(const QString& name);
    void setLocalResolution(uint32_t w, uint32_t h);
    void setPeerResolution(uint32_t w, uint32_t h);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void layoutChanged(ScreenLayout layout);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Recalculate rectangles after a resize or resolution change.
    void recalculate();

    // Position the client rect for a given layout relative to the host.
    QRect positionForLayout(ScreenLayout layout) const;

    // Determine which edge layout a free client rect position corresponds to.
    ScreenLayout inferLayout(const QRect& clientRect) const;

    // Snap a free position to the nearest edge-aligned position.
    QRect snapToEdge(const QRect& freeRect) const;

    QRect hostRect_;
    QRect clientRect_;
    QRect clientBaseRect_;  // logical (pre-drag) position for snap

    bool connected_ = false;
    QString peerName_;
    uint32_t localW_ = 1920;
    uint32_t localH_ = 1080;
    uint32_t peerW_ = 1920;
    uint32_t peerH_ = 1080;

    ScreenLayout layout_ = ScreenLayout::LeftOf;

    bool dragging_ = false;
    QPoint dragOffset_;       // mouse pos - clientRect.topLeft() at press
    QRect dragStartRect_;     // client rect before drag started
    bool hoverSnapped_ = false;

    // Canvas pan: drag the background to reposition both monitors.
    bool panning_ = false;
    QPoint panStart_;
    QPoint panOffset_;        // accumulated pan offset

    // Visual scale: pixels per logical "screen inch" used to proportionally
    // size the rectangles based on resolution.
    double scale_ = 0.1;
};

} // namespace zb
