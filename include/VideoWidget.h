#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QWidget>
#include <QImage>
#include <QPainter>

class VideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget *parent = nullptr);

public slots:
    // 提供一个公共槽，用于接收解码和转换后的QImage
    void updateFrame(const QImage* frame);

protected:
    // 重写paintEvent来实现自定义绘制
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_currentFrame;
};

#endif // VIDEOWIDGET_H
