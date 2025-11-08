#include "VideoWidget.h"

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent) {
    // 设置一些基本属性，比如背景色
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setStyleSheet("background-color: black;");
}

void VideoWidget::updateFrame(const QImage *frame) {
    // 安全检查：frame 可能为 nullptr
    if (!frame || frame->isNull()) {
        m_currentFrame = QImage();
        update();
        return;
    }
    git
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

    // 计算保持长宽比的目标矩形并居中显示
    QSize widgetSize = this->size();
    QSize targetSize = m_currentFrame.size().scaled(widgetSize, Qt::KeepAspectRatio);
    int x = (widgetSize.width() - targetSize.width()) / 2;
    int y = (widgetSize.height() - targetSize.height()) / 2;
    QRect targetRect(x, y, targetSize.width(), targetSize.height());

    // 将图像绘制到Widget上，保持长宽比
    painter.drawImage(targetRect, m_currentFrame);
}
