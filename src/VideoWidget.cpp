#include "VideoWidget.h"

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent) {
    // 设置一些基本属性，比如背景色
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setStyleSheet("background-color: black;");
}

void VideoWidget::updateFrame(const QImage *frame) {
    // 拷贝图像，因为发送者可能在发出信号后就销毁了原图像
    m_currentFrame = frame->copy();
    // 触发重绘事件，但不会立即执行，而是由Qt的事件循环在适当的时候调用paintEvent
    update();
}

void VideoWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    if (m_currentFrame.isNull()) {
        return;
    }

    QPainter painter(this);

    // 将图像绘制到Widget上，并根据Widget的大小进行缩放
    // Qt::SmoothTransformation 提供了更高质量的缩放
    painter.drawImage(this->rect(), m_currentFrame, m_currentFrame.rect(), Qt::AutoColor);
}
