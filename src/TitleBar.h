#ifndef TITLEBAR_H
#define TITLEBAR_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

class TitleBar : public QWidget
{
    Q_OBJECT

public:
    enum ControlsPosition { Left, Right };

    explicit TitleBar(QWidget *parent = nullptr);

    void setControlsPosition(ControlsPosition pos);
    ControlsPosition controlsPosition() const { return m_position; }

    void setTitle(const QString &title);
    void setMaximized(bool maximized);
    void updateSystemStyle();

signals:
    void minimizeClicked();
    void maximizeClicked();
    void closeClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void rebuildLayout();
    QString buttonStyle(bool isClose = false) const;

    QLabel *m_titleLabel;
    QPushButton *m_minimizeBtn;
    QPushButton *m_maximizeBtn;
    QPushButton *m_closeBtn;
    QHBoxLayout *m_layout;

    ControlsPosition m_position = Right;
    QPoint m_dragPos;
    bool m_isMaximized = false;
};

#endif
