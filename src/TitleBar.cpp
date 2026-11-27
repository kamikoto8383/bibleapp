#include "TitleBar.h"
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QApplication>
#include <QStyle>

TitleBar::TitleBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(38);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_titleLabel = new QLabel(QString::fromUtf8("ModulariuM"), this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet("font-weight: 600; font-size: 13px;");

    m_minimizeBtn = new QPushButton(this);
    m_maximizeBtn = new QPushButton(this);
    m_closeBtn = new QPushButton(this);

    m_minimizeBtn->setFixedSize(46, 34);
    m_maximizeBtn->setFixedSize(46, 34);
    m_closeBtn->setFixedSize(46, 34);

    m_minimizeBtn->setToolTip(QString::fromUtf8("Свернуть"));
    m_maximizeBtn->setToolTip(QString::fromUtf8("Развернуть"));
    m_closeBtn->setToolTip(QString::fromUtf8("Закрыть"));

    m_minimizeBtn->setText("–");
    m_maximizeBtn->setText("□");
    m_closeBtn->setText("×");
    m_minimizeBtn->setFlat(true);
    m_maximizeBtn->setFlat(true);
    m_closeBtn->setFlat(true);

    connect(m_minimizeBtn, &QPushButton::clicked, this, &TitleBar::minimizeClicked);
    connect(m_maximizeBtn, &QPushButton::clicked, this, &TitleBar::maximizeClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &TitleBar::closeClicked);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(6, 0, 6, 0);
    m_layout->setSpacing(0);

    rebuildLayout();
    updateSystemStyle();
}

void TitleBar::updateSystemStyle()
{
    // Наследуем цвета системы
    setAutoFillBackground(true);
    QPalette pal = QApplication::palette();
    setPalette(pal);
    m_titleLabel->setPalette(pal);
    rebuildLayout();
}

void TitleBar::setControlsPosition(ControlsPosition pos)
{
    if (m_position == pos)
        return;
    m_position = pos;
    rebuildLayout();
}

void TitleBar::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void TitleBar::setMaximized(bool maximized)
{
    m_isMaximized = maximized;
    m_maximizeBtn->setText(maximized ? "❐" : "□");
    m_maximizeBtn->setToolTip(maximized ? QString::fromUtf8("Восстановить")
                                        : QString::fromUtf8("Развернуть"));
}

void TitleBar::rebuildLayout()
{
    QLayoutItem *item;
    while ((item = m_layout->takeAt(0)) != nullptr) {
        // не удаляем виджеты
    }

    if (m_position == Left) {
        m_layout->addWidget(m_closeBtn);
        m_layout->addWidget(m_minimizeBtn);
        m_layout->addWidget(m_maximizeBtn);
        m_layout->addStretch();
        m_layout->addWidget(m_titleLabel);
        m_layout->addStretch();
    } else {
        m_layout->addStretch();
        m_layout->addWidget(m_titleLabel);
        m_layout->addStretch();
        m_layout->addWidget(m_minimizeBtn);
        m_layout->addWidget(m_maximizeBtn);
        m_layout->addWidget(m_closeBtn);
    }

    m_minimizeBtn->setStyleSheet(buttonStyle());
    m_maximizeBtn->setStyleSheet(buttonStyle());
    m_closeBtn->setStyleSheet(buttonStyle(true));
}

QString TitleBar::buttonStyle(bool isClose) const
{
    // Используем системную палитру через palette()
    if (isClose) {
        return QString::fromUtf8(
            "QPushButton {"
            "  border: none;"
            "  background: transparent;"
            "  color: palette(window-text);"
            "  font-size: 16px;"
            "  border-radius: 4px;"
            "}"
            "QPushButton:hover {"
            "  background: #e81123;"
            "  color: white;"
            "}"
            "QPushButton:pressed {"
            "  background: #c50f1f;"
            "  color: white;"
            "}"
        );
    }

    return QString::fromUtf8(
        "QPushButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: palette(window-text);"
        "  font-size: 14px;"
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background: palette(mid);"
        "}"
        "QPushButton:pressed {"
        "  background: palette(dark);"
        "}"
    );
}

void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragPos = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
    }
}

void TitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton && !m_isMaximized) {
        window()->move(event->globalPosition().toPoint() - m_dragPos);
        event->accept();
    }
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit maximizeClicked();
}

void TitleBar::paintEvent(QPaintEvent *)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    // Рисуем фон как у системного окна
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void TitleBar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        updateSystemStyle();
    }
    QWidget::changeEvent(event);
}
