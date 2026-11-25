#include <QUrl>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QHash>
#include <QUrlQuery>
#include <algorithm>
#include "BibleView.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QListView>
#include <QStandardItem>
#include <QFrame>
#include <QAbstractItemView>
#include <QFontComboBox>
#include <QStyle>
#include <QPainter>
#include <QPaintEvent>
#include <QTabWidget>
#include <QFontDatabase>
#include <QTextBrowser>
#include <QToolBar>
#include <QMainWindow>
#include <QEasingCurve>
#include <QWheelEvent>
#include <QPropertyAnimation>
#include <QAbstractAnimation>
#include <QScroller>
#include <QScrollerProperties>
#include <QLabel>
#include <QSizePolicy>
#include <QPushButton>
#include <QLineEdit>
#include <QEvent>
#include <QResizeEvent>
#include <QPalette>
#include <QColor>
#include <QListWidget>
#include <QSet>
#include <QSplitter>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include <QTextCursor>
#include <QScrollBar>
#include <QTextEdit>
#include <QColor>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>

static QSettings &appSettings()
{
    static QSettings *s = nullptr;
    if (!s) {
        const QString dir = QDir::homePath() + QStringLiteral("/.config/bibleapp");
        QDir().mkpath(dir);
        s = new QSettings(dir + QStringLiteral("/ui.ini"), QSettings::IniFormat);
        s->setFallbacksEnabled(false);
    }
    return *s;
}

static void savePanelState(BibleView *view);

static void savePanelState(BibleView *view)
{
    if (!view || view->settingsSaveBlocked())
        return;
    QString key = QStringLiteral("panel/left");
    if (view->objectName() == QLatin1String("panelRight"))
        key = QStringLiteral("panel/right");
    QSettings &st = appSettings();
    st.setValue(key + QStringLiteral("/fontSize"), view->fontSize());
    st.setValue(key + QStringLiteral("/fontFamily"), view->fontFamily());
    if (view->module()) {
        st.setValue(key + QStringLiteral("/module"), view->module()->name());
        st.setValue(key + QStringLiteral("/book"), view->currentBook());
        st.setValue(key + QStringLiteral("/chapter"), view->currentChapter());
    }
    if (view->contentSplitter()) {
        const QList<int> sizes = view->contentSplitter()->sizes();
        if (sizes.size() >= 2 && sizes[0] + sizes[1] > 100) {
            st.setValue(key + QStringLiteral("/splitText"), sizes[0]);
            st.setValue(key + QStringLiteral("/splitSide"), sizes[1]);
        }
    }
    st.sync();
}

#include <QKeyEvent>
#include <QTextCursor>
#include <QScrollBar>
#include <QTextEdit>
#include <QColor>
#include <QMessageBox>

static QStringList parallelRefs(int book, int chapter, int verse)
{
    QString key = QString("%1:%2:%3").arg(book).arg(chapter).arg(verse);
    static QHash<QString, QStringList> map;
    if (map.isEmpty()) {
        map["1:1:1"] = {QString::fromUtf8("Ин 1:1–3"), QString::fromUtf8("Евр 11:3")};
        map["1:1:3"] = {QString::fromUtf8("2 Кор 4:6")};
        map["43:1:1"] = {QString::fromUtf8("Быт 1:1"), QString::fromUtf8("1 Ин 1:1–2")};
        map["43:1:12"] = {QString::fromUtf8("Рим 8:14–16"), QString::fromUtf8("Гал 3:26")};
        map["19:1:1"] = {QString::fromUtf8("Иер 17:7–8")};
        map["19:1:3"] = {QString::fromUtf8("Иер 17:8")};
    }
    return map.value(key);
}


/** QComboBox, который всегда открывает список прямо под собой */
class AnchoredComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void showPopup() override
    {
        QComboBox::showPopup();
        QWidget *popup = this->findChild<QFrame *>();
        if (!popup) {
            // Qt6: контейнер попапа — top-level окно view
            QAbstractItemView *v = view();
            if (v)
                popup = v->window();
        }
        if (!popup)
            return;
        const QPoint below = mapToGlobal(QPoint(0, height()));
        popup->move(below);
        // ширина не меньше комбобокса
        if (popup->width() < width())
            popup->resize(width(), popup->height());
    }
};


// Плавная прокрутка колёсиком мыши для QTextBrowser

/** Отдельное окно перевода / сноски — системная рамка (свернуть/развернуть/закрыть). */
class WordInfoWindow : public QMainWindow
{
public:
    explicit WordInfoWindow(QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(QString::fromUtf8("Справка"));
        setAttribute(Qt::WA_DeleteOnClose, false);
        setWindowFlags(Qt::Window
                       | Qt::WindowTitleHint
                       | Qt::WindowSystemMenuHint
                       | Qt::WindowMinMaxButtonsHint
                       | Qt::WindowCloseButtonHint);
        resize(460, 400);
        setMinimumSize(300, 200);

        m_browser = new QTextBrowser(this);
        m_browser->setOpenLinks(false);
        m_browser->setOpenExternalLinks(false);
        m_browser->setReadOnly(true);
        setCentralWidget(m_browser);

        applyPaperStyle();
        m_browser->setHtml(wrapHtml(QString::fromUtf8(
            "<p class='hint'>Нажмите на греческое слово, номер Стронга или сноску в тексте — "
            "здесь появится информация.</p>")));
    }

    static QString documentCss()
    {
        return QStringLiteral(
            "body {"
            "  background-color: #f7f0e0;"
            "  color: #1a120a;"
            "  font-family: 'Noto Serif', 'DejaVu Serif', serif;"
            "  font-size: 14px;"
            "  line-height: 1.55;"
            "  margin: 0;"
            "  padding: 12px 16px 16px 16px;"
            "}"
            "h1, h2, h3, h4 {"
            "  color: #2c1810;"
            "  font-weight: 700;"
            "  margin: 0.4em 0 0.5em 0;"
            "  line-height: 1.3;"
            "}"
            "h1 { font-size: 1.25em; }"
            "h2 { font-size: 1.12em; }"
            "p { margin: 0.45em 0; }"
            "b, strong { color: #1a120a; font-weight: 700; }"
            "i, em { color: #3a2a1a; }"
            "a {"
            "  color: #6b3a1a;"
            "  text-decoration: underline;"
            "  text-underline-offset: 2px;"
            "}"
            "a:hover { color: #8a4a20; }"
            "mark, .highlight {"
            "  background-color: #ffe28a;"
            "  color: #1a1000;"
            "  padding: 0 2px;"
            "  border-radius: 2px;"
            "}"
            "hr {"
            "  border: none;"
            "  border-top: 1px solid #d4c4a8;"
            "  margin: 10px 0;"
            "}"
            "ul, ol { margin: 0.4em 0 0.4em 1.2em; padding: 0; }"
            "li { margin: 0.2em 0; }"
            "blockquote {"
            "  margin: 0.6em 0;"
            "  padding: 8px 12px;"
            "  background: #efe6d2;"
            "  border-left: 3px solid #c4a574;"
            "  color: #2c1810;"
            "}"
            "table { border-collapse: collapse; width: 100%; margin: 0.5em 0; }"
            "td, th {"
            "  border: 1px solid #d4c4a8;"
            "  padding: 4px 8px;"
            "  vertical-align: top;"
            "}"
            "th { background: #efe6d2; font-weight: 700; }"
            ".ref-title {"
            "  font-size: 1.2em;"
            "  font-weight: 700;"
            "  color: #2c1810;"
            "  margin: 0 0 10px 0;"
            "  padding-bottom: 6px;"
            "  border-bottom: 1px solid #d4c4a8;"
            "}"
            ".verse-text { line-height: 1.55; color: #1a120a; }"
            ".hint { color: #5a4a3a; font-size: 0.95em; }"
            ".strong-num {"
            "  display: inline-block;"
            "  background: #e8d4a8;"
            "  color: #2c1810;"
            "  font-weight: 700;"
            "  padding: 1px 6px;"
            "  border-radius: 3px;"
            "  font-size: 0.9em;"
            "}"
        );
    }

    void applyPaperStyle()
    {
        const QString css = QStringLiteral(
            "QMainWindow { background-color: #f7f0e0; }"
            "QTextBrowser {"
            "  background-color: #f7f0e0;"
            "  color: #1a120a;"
            "  border: none;"
            "  padding: 0;"
            "  selection-background-color: #e0d0a0;"
            "  selection-color: #000000;"
            "}"
            "QTextBrowser QScrollBar:vertical {"
            "  background: #f0e6d0; width: 12px; margin: 0; border-radius: 6px;"
            "}"
            "QTextBrowser QScrollBar::handle:vertical {"
            "  background: #c4b090; min-height: 28px; border-radius: 6px;"
            "  border: 1px solid #a89070;"
            "}"
            "QTextBrowser QScrollBar::handle:vertical:hover { background: #b09a78; }"
            "QTextBrowser QScrollBar::add-line:vertical,"
            "QTextBrowser QScrollBar::sub-line:vertical { height: 0; background: none; }"
            "QTextBrowser QScrollBar::add-page:vertical,"
            "QTextBrowser QScrollBar::sub-page:vertical { background: #f0e6d0; }"
            "QTextBrowser QScrollBar:horizontal {"
            "  background: #f0e6d0; height: 12px; margin: 0; border-radius: 6px;"
            "}"
            "QTextBrowser QScrollBar::handle:horizontal {"
            "  background: #c4b090; min-width: 28px; border-radius: 6px;"
            "  border: 1px solid #a89070;"
            "}"
            "QTextBrowser QScrollBar::add-line:horizontal,"
            "QTextBrowser QScrollBar::sub-line:horizontal { width: 0; background: none; }"
        );
        setStyleSheet(css);
        if (m_browser)
            m_browser->setStyleSheet(css);
    }

    static QString wrapHtml(const QString &html)
    {
        return QStringLiteral(
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<style>%1</style></head><body>%2</body></html>"
        ).arg(documentCss(), html);
    }

    void setHtml(const QString &html)
    {
        if (!m_browser)
            return;
        applyPaperStyle();
        m_browser->setHtml(wrapHtml(html));
        if (!isVisible())
            show();
        raise();
        activateWindow();
    }

private:
    QTextBrowser *m_browser = nullptr;
};

class SmoothWheelFilter : public QObject
{
public:
    explicit SmoothWheelFilter(QScrollBar *bar, QObject *parent = nullptr)
        : QObject(parent)
        , m_bar(bar)
    {
        m_anim = new QPropertyAnimation(bar, "value", this);
        m_anim->setDuration(180);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (!m_bar || event->type() != QEvent::Wheel)
            return QObject::eventFilter(obj, event);

        auto *we = static_cast<QWheelEvent *>(event);
        // Тачпад / горизонталь — не трогаем
        if (we->angleDelta().y() == 0)
            return QObject::eventFilter(obj, event);

        const int delta = we->angleDelta().y();
        // Один «щелчок» ≈ 120; крутим примерно на 3 строки
        const int step = m_bar->singleStep() > 0 ? m_bar->singleStep() * 3 : 60;
        int target;
        if (m_anim->state() == QAbstractAnimation::Running) {
            target = m_anim->endValue().toInt();
            m_anim->stop();
        } else {
            target = m_bar->value();
        }
        target -= (delta / 120.0) * step;
        target = qBound(m_bar->minimum(), target, m_bar->maximum());

        m_anim->setStartValue(m_bar->value());
        m_anim->setEndValue(target);
        m_anim->start();
        return true; // съели событие
    }

private:
    QScrollBar *m_bar = nullptr;
    QPropertyAnimation *m_anim = nullptr;
};

BibleView::BibleView(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(4);

    auto *topLayout = new QHBoxLayout;
    m_moduleLabel = new QLabel(QString::fromUtf8("Модуль не выбран"), this);
    m_moduleLabel->setStyleSheet("font-weight: 600;");
    topLayout->addWidget(m_moduleLabel, 1);
    m_loadBtn = new QPushButton(QString::fromUtf8("Выбрать книгу…"), this);
    topLayout->addWidget(m_loadBtn);
    connect(m_loadBtn, &QPushButton::clicked, this, &BibleView::requestLoadModule);
    mainLayout->addLayout(topLayout);

    // Навигация
    auto *navLayout = new QHBoxLayout;
    navLayout->setSpacing(4);

    auto makeNavLabel = [this](const QString &text) {
        auto *lb = new QLabel(text, this);
        lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        lb->setMinimumWidth(lb->fontMetrics().horizontalAdvance(text) + 4);
        return lb;
    };

    navLayout->addWidget(makeNavLabel(QString::fromUtf8("Книга:")));
    m_bookCombo = new AnchoredComboBox(this);
    m_bookCombo->setMinimumWidth(100);
    m_bookCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    navLayout->addWidget(m_bookCombo);

    navLayout->addWidget(makeNavLabel(QString::fromUtf8("Глава:")));
    m_chapterCombo = new AnchoredComboBox(this);
    m_chapterCombo->setMinimumWidth(56);
    m_chapterCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_chapterCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    navLayout->addWidget(m_chapterCombo);

    m_prevBtn = new QPushButton(QString::fromUtf8("◀"), this);
    m_prevBtn->setToolTip(QString::fromUtf8("Предыдущая глава (←)"));
    m_prevBtn->setFixedSize(28, 28);
    m_prevBtn->setMaximumWidth(28);
    navLayout->addWidget(m_prevBtn);

    m_nextBtn = new QPushButton(QString::fromUtf8("▶"), this);
    m_nextBtn->setToolTip(QString::fromUtf8("Следующая глава (→)"));
    m_nextBtn->setFixedSize(28, 28);
    m_nextBtn->setMaximumWidth(28);
    navLayout->addWidget(m_nextBtn);

    m_chapterLabel = new QLabel(this);
    m_chapterLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_chapterLabel->setMinimumWidth(36);
    navLayout->addWidget(m_chapterLabel);

    navLayout->addSpacing(6);
    navLayout->addWidget(makeNavLabel(QString::fromUtf8("Стих:")));
    m_verseCombo = new AnchoredComboBox(this);
    m_verseCombo->setMinimumWidth(52);
    m_verseCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_verseCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_verseCombo->setToolTip(QString::fromUtf8("Выбор стиха"));
    navLayout->addWidget(m_verseCombo);

    navLayout->addSpacing(6);
    navLayout->addWidget(makeNavLabel(QString::fromUtf8("Шрифт:")));
    auto *fontMinus = new QPushButton("A−", this);
    fontMinus->setFixedWidth(36);
    auto *fontPlus = new QPushButton("A+", this);
    fontPlus->setFixedWidth(36);
    m_fontLabel = new QLabel("13", this);
    navLayout->addWidget(fontMinus);
    navLayout->addWidget(m_fontLabel);
    navLayout->addWidget(fontPlus);
    connect(fontMinus, &QPushButton::clicked, this, &BibleView::onFontSmaller);
    connect(fontPlus, &QPushButton::clicked, this, &BibleView::onFontBigger);

    navLayout->addStretch();
    mainLayout->addLayout(navLayout);

    connect(m_prevBtn, &QPushButton::clicked, this, &BibleView::onPrevChapter);
    connect(m_nextBtn, &QPushButton::clicked, this, &BibleView::onNextChapter);
    connect(m_verseCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BibleView::onVerseChanged);


    // Поиск
    auto *searchLayout = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    // Системный placeholder часто полупрозрачный — рисуем свою яркую подсказку
    m_searchEdit->setPlaceholderText(QString());
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  color: #ffffff;"
        "  background-color: rgba(0,0,0,40%);"
        "  border: 1px solid #666666;"
        "  border-radius: 3px;"
        "  padding: 4px 8px;"
        "  selection-background-color: #5a7aaa;"
        "  selection-color: #ffffff;"
        "}"
    ));
    searchLayout->addWidget(m_searchEdit, 1);

    // Белая непрозрачная подсказка поверх поля (пока пусто)
    {
        auto *hint = new QLabel(QString::fromUtf8("Поиск"), m_searchEdit);
        hint->setObjectName(QStringLiteral("searchHint"));
        hint->setStyleSheet(QStringLiteral(
            "QLabel#searchHint {"
            "  color: #ffffff;"
            "  background: transparent;"
            "  padding-left: 8px;"
            "  font-size: 13px;"
            "}"
        ));
        hint->setAttribute(Qt::WA_TransparentForMouseEvents);
        hint->setGeometry(0, 0, m_searchEdit->width(), m_searchEdit->height());
        auto reposition = [hint](QLineEdit *edit) {
            if (!hint || !edit) return;
            hint->setGeometry(2, 0, qMax(10, edit->width() - 4), edit->height());
            hint->setVisible(edit->text().isEmpty());
        };
        m_searchEdit->installEventFilter(this);
        QObject::connect(m_searchEdit, &QLineEdit::textChanged, this, [hint](const QString &t) {
            if (hint) hint->setVisible(t.isEmpty());
        });
        QTimer::singleShot(0, m_searchEdit, [m_searchEdit = m_searchEdit, hint, reposition]() {
            reposition(m_searchEdit);
        });
        m_searchEdit->setProperty("hasSearchHint", true);
        hint->raise();
    }

    // Маленькая кнопка очистки поля поиска
    auto *clearFieldBtn = new QPushButton(QString::fromUtf8("✕"), this);
    clearFieldBtn->setFixedSize(28, 28);
    clearFieldBtn->setToolTip(QString::fromUtf8("Очистить поле поиска"));
    clearFieldBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  font-size: 12px;"
        "  padding: 0;"
        "  min-width: 28px;"
        "}"
    ));
    searchLayout->addWidget(clearFieldBtn);
    connect(clearFieldBtn, &QPushButton::clicked, this, [this]() {
        if (m_searchEdit)
            m_searchEdit->clear();
        onClearSearchResults();
    });

    auto *searchBtn = new QPushButton(QString::fromUtf8("Найти"), this);
    searchLayout->addWidget(searchBtn);
    connect(searchBtn, &QPushButton::clicked, this, &BibleView::onSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &BibleView::onSearch);
    mainLayout->addLayout(searchLayout);



    m_contentSplitter = new QSplitter(Qt::Horizontal, this);

    m_textView = new QTextBrowser(this);
    m_textView->setOpenLinks(false);
    m_textView->setOpenExternalLinks(false);
    connect(m_textView, &QTextBrowser::anchorClicked, this, &BibleView::onAnchorClicked);
    m_textView->setReadOnly(true);
    m_textView->setContextMenuPolicy(Qt::CustomContextMenu);
    // Плавная прокрутка
    {
        QScrollBar *vsb = m_textView->verticalScrollBar();
        vsb->setSingleStep(24); // мельче шаг — мягче ощущения
        auto *smooth = new SmoothWheelFilter(vsb, m_textView);
        m_textView->viewport()->installEventFilter(smooth);
        // Кинетический скролл (тачпад / палец)
        QScroller::grabGesture(m_textView->viewport(), QScroller::TouchGesture);
        QScroller *sc = QScroller::scroller(m_textView->viewport());
        if (sc) {
            QScrollerProperties prop = sc->scrollerProperties();
            prop.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                                 QVariant::fromValue(QScrollerProperties::OvershootWhenScrollable));
            prop.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.12);
            prop.setScrollMetric(QScrollerProperties::DragVelocitySmoothingFactor, 0.8);
            prop.setScrollMetric(QScrollerProperties::FrameRate,
                                 QVariant::fromValue(QScrollerProperties::Fps60));
            sc->setScrollerProperties(prop);
        }
    }
    // Синхронный скролл
    connect(m_textView->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        emit scrollRatioChanged(scrollRatio());
    });
    connect(m_textView, &QWidget::customContextMenuRequested,
            this, &BibleView::onTextContextMenu);
    m_contentSplitter->addWidget(m_textView);

    auto *side = new QWidget(this);
    auto *sideOuter = new QVBoxLayout(side);
    sideOuter->setContentsMargins(0, 0, 0, 0);
    m_sideTabs = new QTabWidget(side);

    // --- Вкладка: Закладки / поиск / параллели ---
    auto *sideTools = new QWidget(m_sideTabs);
    auto *sideLayout = new QVBoxLayout(sideTools);
    sideLayout->setContentsMargins(4, 4, 4, 4);

    sideLayout->addWidget(new QLabel(QString::fromUtf8("Закладки:"), sideTools));
    m_bookmarkList = new QListWidget(sideTools);
    m_bookmarkList->setMaximumHeight(140);
    sideLayout->addWidget(m_bookmarkList);
    connect(m_bookmarkList, &QListWidget::itemDoubleClicked, this, &BibleView::onBookmarkClicked);

    auto *bmBtnRow = new QHBoxLayout;
    auto *gotoBm = new QPushButton(QString::fromUtf8("Перейти"), sideTools);
    auto *delBm = new QPushButton(QString::fromUtf8("Удалить"), sideTools);
    delBm->setToolTip(QString::fromUtf8("Удалить выделенную закладку"));
    auto *clearBm = new QPushButton(QString::fromUtf8("Очистить"), sideTools);
    clearBm->setToolTip(QString::fromUtf8("Удалить все закладки этого модуля"));
    bmBtnRow->addWidget(gotoBm);
    bmBtnRow->addWidget(delBm);
    bmBtnRow->addWidget(clearBm);
    sideLayout->addLayout(bmBtnRow);
    connect(gotoBm, &QPushButton::clicked, this, &BibleView::onBookmarkClicked);
    connect(delBm, &QPushButton::clicked, this, &BibleView::onRemoveBookmark);
    connect(clearBm, &QPushButton::clicked, this, &BibleView::onClearBookmarks);

    {
        auto *srRow = new QHBoxLayout;
        srRow->addWidget(new QLabel(QString::fromUtf8("Результаты поиска:"), sideTools));
        srRow->addStretch();
        auto *clearSearchBtn = new QPushButton(QString::fromUtf8("Очистить"), sideTools);
        clearSearchBtn->setToolTip(QString::fromUtf8("Очистить результаты поиска и подсветку"));
        srRow->addWidget(clearSearchBtn);
        sideLayout->addLayout(srRow);
        connect(clearSearchBtn, &QPushButton::clicked, this, &BibleView::onClearSearchResults);
    }
    m_searchResults = new QListWidget(sideTools);
    sideLayout->addWidget(m_searchResults, 1);
    connect(m_searchResults, &QListWidget::itemClicked, this, &BibleView::onSearchResultClicked);

    sideLayout->addWidget(new QLabel(QString::fromUtf8("Параллельные места:"), sideTools));
    m_parallelList = new QListWidget(sideTools);
    m_parallelList->setMaximumHeight(120);
    sideLayout->addWidget(m_parallelList);
    connect(m_parallelList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item || !item->data(Qt::UserRole).isValid())
            return;
        const int b = item->data(Qt::UserRole).toInt();
        const int c = item->data(Qt::UserRole + 1).toInt();
        const int v = item->data(Qt::UserRole + 2).toInt();
        m_lastSearchQuery.clear();
        goTo(b, c, v);
        QTimer::singleShot(80, this, [this, b, c, v]() { revealParallelVerse(b, c, v); });
        QTimer::singleShot(250, this, [this, b, c, v]() { revealParallelVerse(b, c, v); });
    });

    m_sideTabs->addTab(sideTools, QString::fromUtf8("Закладки"));

    // --- Вкладка: Шрифт ---
    auto *fontPage = new QWidget(m_sideTabs);
    auto *fontLay = new QVBoxLayout(fontPage);
    fontLay->setContentsMargins(8, 8, 8, 8);
    fontLay->addWidget(new QLabel(QString::fromUtf8(
        "Шрифт текста модуля.\n"
        "Для греческого и древнееврейского\n"
        "рекомендуются встроенные шрифты."), fontPage));

    fontLay->addWidget(new QLabel(QString::fromUtf8("Семейство:"), fontPage));
    m_fontCombo = new QFontComboBox(fontPage);
    m_fontCombo->setWritingSystem(QFontDatabase::Any);
    fontLay->addWidget(m_fontCombo);

    auto *prefLabel = new QLabel(QString::fromUtf8(
        "Встроенные:\n"
        "• Noto Serif — греческий, латиница, кириллица\n"
        "• Gentium Plus — древнегреческий\n"
        "• Noto Serif Hebrew — древнееврейский"), fontPage);
    prefLabel->setWordWrap(true);
    prefLabel->setStyleSheet("color:#555; font-size:11px;");
    fontLay->addWidget(prefLabel);

    auto *quickRow = new QHBoxLayout;
    auto *btnNoto = new QPushButton(QString::fromUtf8("Noto Serif"), fontPage);
    auto *btnGent = new QPushButton(QString::fromUtf8("Gentium"), fontPage);
    auto *btnHeb = new QPushButton(QString::fromUtf8("Hebrew"), fontPage);
    quickRow->addWidget(btnNoto);
    quickRow->addWidget(btnGent);
    quickRow->addWidget(btnHeb);
    fontLay->addLayout(quickRow);
    connect(btnNoto, &QPushButton::clicked, this, [this]() { setFontFamily(QStringLiteral("Noto Serif")); });
    connect(btnGent, &QPushButton::clicked, this, [this]() { setFontFamily(QStringLiteral("Gentium Plus")); });
    connect(btnHeb, &QPushButton::clicked, this, [this]() { setFontFamily(QStringLiteral("Noto Serif Hebrew")); });

    fontLay->addWidget(new QLabel(QString::fromUtf8("Размер:"), fontPage));
    auto *sizeRow = new QHBoxLayout;
    auto *fontMinus2 = new QPushButton(QStringLiteral("A−"), fontPage);
    auto *fontPlus2 = new QPushButton(QStringLiteral("A+"), fontPage);
    m_fontLabel = m_fontLabel; // already created in nav — keep nav one
    auto *sizeLbl = new QLabel(QString::number(m_fontSize), fontPage);
    sizeLbl->setObjectName(QStringLiteral("fontTabSizeLabel"));
    sizeRow->addWidget(fontMinus2);
    sizeRow->addWidget(sizeLbl, 1, Qt::AlignCenter);
    sizeRow->addWidget(fontPlus2);
    fontLay->addLayout(sizeRow);
    connect(fontMinus2, &QPushButton::clicked, this, &BibleView::onFontSmaller);
    connect(fontPlus2, &QPushButton::clicked, this, &BibleView::onFontBigger);

    fontLay->addStretch(1);

    m_sideTabs->addTab(fontPage, QString::fromUtf8("Шрифт"));

    // --- Вкладка: История ---
    auto *histPage = new QWidget(m_sideTabs);
    auto *histLay = new QVBoxLayout(histPage);
    histLay->setContentsMargins(4, 4, 4, 4);
    histLay->addWidget(new QLabel(QString::fromUtf8(
        "Места чтения (модуль, книга, глава:стих):"), histPage));
    m_historyList = new QListWidget(histPage);
    histLay->addWidget(m_historyList, 1);
    connect(m_historyList, &QListWidget::itemDoubleClicked, this, &BibleView::onHistoryClicked);
    connect(m_historyList, &QListWidget::itemClicked, this, &BibleView::onHistoryClicked);

    auto *histBtnRow = new QHBoxLayout;
    auto *gotoHist = new QPushButton(QString::fromUtf8("Перейти"), histPage);
    auto *clearHist = new QPushButton(QString::fromUtf8("Очистить"), histPage);
    clearHist->setToolTip(QString::fromUtf8("Очистить всю историю чтения"));
    histBtnRow->addWidget(gotoHist);
    histBtnRow->addWidget(clearHist);
    histLay->addLayout(histBtnRow);
    connect(gotoHist, &QPushButton::clicked, this, &BibleView::onHistoryClicked);
    connect(clearHist, &QPushButton::clicked, this, &BibleView::onClearHistory);

    m_sideTabs->addTab(histPage, QString::fromUtf8("История"));


    sideOuter->addWidget(m_sideTabs);
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
        if (m_blockSettingsSave)
            return;
        setFontFamily(f.family());
    });

    m_contentSplitter->addWidget(side);
    m_contentSplitter->setChildrenCollapsible(false);
    m_contentSplitter->setStretchFactor(0, 0);
    m_contentSplitter->setStretchFactor(1, 0);
    mainLayout->addWidget(m_contentSplitter, 1);
    connect(m_contentSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        savePanelState(this);
    });
    // восстановить ширину текст/боковая панель после показа
    QTimer::singleShot(100, this, [this]() { restoreContentSplitter(); });
    QTimer::singleShot(400, this, [this]() { restoreContentSplitter(); });

    connect(m_bookCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BibleView::onBookChanged);
    connect(m_chapterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!m_chapterCombo || m_chapterCombo->currentData().isNull())
                    return;
                onChapterChanged(m_chapterCombo->currentData().toInt());
            });

    applyTextStyle();
    loadHistory();
}


bool BibleView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_searchEdit && m_searchEdit) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            if (QLabel *hint = m_searchEdit->findChild<QLabel *>(QStringLiteral("searchHint"))) {
                hint->setGeometry(2, 0, qMax(10, m_searchEdit->width() - 4), m_searchEdit->height());
                hint->setVisible(m_searchEdit->text().isEmpty());
                hint->raise();
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void BibleView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Left) {
        onPrevChapter();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        onNextChapter();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void BibleView::fillChapterCombo(int book, int selectChapter)
{
    if (!m_chapterCombo)
        return;

    m_chapterCombo->blockSignals(true);
    m_chapterCombo->clear();

    QVector<int> chapters;
    if (m_module)
        chapters = m_module->chapterNumbers(book);
    if (chapters.isEmpty())
        chapters << 1;

    for (int c : chapters) {
        const QString label = (c == 0) ? QString::fromUtf8("Введение")
                                       : QString::number(c);
        m_chapterCombo->addItem(label, c);
    }

    int ix = m_chapterCombo->findData(selectChapter);
    if (ix < 0)
        ix = 0;
    m_chapterCombo->setCurrentIndex(ix);
    m_chapterCombo->blockSignals(false);

    if (m_chapterLabel)
        m_chapterLabel->setText(QString::fromUtf8("из %1").arg(chapters.size()));
}

void BibleView::fillVerseCombo(int book, int chapter, int selectVerse)
{
    if (!m_verseCombo)
        return;

    m_verseCombo->blockSignals(true);
    m_verseCombo->clear();

    QVector<int> verses;
    if (m_module)
        verses = m_module->verseNumbers(book, chapter);
    if (verses.isEmpty())
        verses << 1;

    for (int v : verses) {
        const QString label = (v == 0) ? QString::fromUtf8("Вступление")
                                       : QString::number(v);
        m_verseCombo->addItem(label, v);
    }
    m_verseCombo->setToolTip(QString::fromUtf8("Выбор стиха"));

    int ix = m_verseCombo->findData(selectVerse);
    if (ix < 0)
        ix = 0;
    m_verseCombo->setCurrentIndex(ix);
    m_verseCombo->blockSignals(false);
    m_currentVerse = m_verseCombo->currentData().toInt();
}


void BibleView::onPrevChapter()
{
    if (!m_module || m_bookCombo->count() == 0)
        return;

    const int idx = m_chapterCombo->currentIndex();
    if (idx > 0) {
        m_chapterCombo->blockSignals(true);
        m_chapterCombo->setCurrentIndex(idx - 1);
        m_chapterCombo->blockSignals(false);
        m_currentChapter = m_chapterCombo->currentData().toInt();
        m_currentVerse = 1;
        loadChapter();
        refillVerseCombo(1);
        return;
    }

    int bookIdx = m_bookCombo->currentIndex() - 1;
    while (bookIdx >= 0 && m_bookCombo->itemData(bookIdx).toInt() <= 0)
        --bookIdx;
    if (bookIdx >= 0) {
        m_bookCombo->blockSignals(true);
        m_bookCombo->setCurrentIndex(bookIdx);
        m_bookCombo->blockSignals(false);
        m_currentBook = m_bookCombo->itemData(bookIdx).toInt();
        const QVector<int> chapters = m_module->chapterNumbers(m_currentBook);
        const int last = chapters.isEmpty() ? 1 : chapters.last();
        fillChapterCombo(m_currentBook, last);
        m_currentChapter = m_chapterCombo->currentData().toInt();
        m_currentVerse = 1;
        loadChapter();
        refillVerseCombo(1);
    }
}

void BibleView::onNextChapter()
{
    if (!m_module || m_bookCombo->count() == 0)
        return;

    const int idx = m_chapterCombo->currentIndex();
    if (idx >= 0 && idx + 1 < m_chapterCombo->count()) {
        m_chapterCombo->blockSignals(true);
        m_chapterCombo->setCurrentIndex(idx + 1);
        m_chapterCombo->blockSignals(false);
        m_currentChapter = m_chapterCombo->currentData().toInt();
        m_currentVerse = 1;
        loadChapter();
        refillVerseCombo(1);
        return;
    }

    int bookIdx = m_bookCombo->currentIndex() + 1;
    while (bookIdx < m_bookCombo->count() && m_bookCombo->itemData(bookIdx).toInt() <= 0)
        ++bookIdx;
    if (bookIdx < m_bookCombo->count()) {
        m_bookCombo->blockSignals(true);
        m_bookCombo->setCurrentIndex(bookIdx);
        m_bookCombo->blockSignals(false);
        m_currentBook = m_bookCombo->itemData(bookIdx).toInt();
        fillChapterCombo(m_currentBook, 1);
        m_currentChapter = m_chapterCombo->currentData().toInt();
        m_currentVerse = 1;
        loadChapter();
        refillVerseCombo(1);
    }
}

QString BibleView::bookmarkKey() const
{
    if (!m_module) return "none";
    return m_module->name();
}

void BibleView::loadBookmarks()
{
    refreshBookmarkList();
}

void BibleView::saveBookmarks()
{
    // список уже в QSettings через refresh при add/remove
}

void BibleView::refreshBookmarkList()
{
    m_bookmarkList->clear();
    if (!m_module) return;

    QSettings &s = appSettings();
    QStringList list = s.value("bookmarks/" + bookmarkKey()).toStringList();
    for (const QString &entry : list) {
        // format: bookId|chapter|label|snippet
        QStringList parts = entry.split('|');
        if (parts.size() >= 3) {
            auto *item = new QListWidgetItem(parts[2], m_bookmarkList);
            item->setData(Qt::UserRole, parts[0].toInt());
            item->setData(Qt::UserRole + 1, parts[1].toInt());
            if (parts.size() >= 4)
                item->setData(Qt::UserRole + 2, parts[3]);
            else
                item->setData(Qt::UserRole + 2, QString());
        }
    }
}

void BibleView::addBookmarkWithLabel(const QString &label, const QString &snippet)
{
    if (!m_module || m_bookCombo->currentIndex() < 0)
        return;

    int book = m_bookCombo->currentData().toInt();
    int chapter = m_chapterCombo->currentData().toInt();
    QString safeLabel = label;
    safeLabel.replace(QLatin1Char('|'), QLatin1Char('/'));
    QString safeSnippet = snippet.simplified();
    safeSnippet.replace(QLatin1Char('|'), QLatin1Char('/'));
    if (safeSnippet.length() > 200)
        safeSnippet = safeSnippet.left(200);

    QString entry = QString("%1|%2|%3|%4").arg(book).arg(chapter).arg(safeLabel).arg(safeSnippet);

    QSettings &s = appSettings();
    QString key = "bookmarks/" + bookmarkKey();
    QStringList list = s.value(key).toStringList();
    if (!list.contains(entry)) {
        list.append(entry);
        s.setValue(key, list);
        s.sync();
    }
    refreshBookmarkList();
}

void BibleView::onAddBookmark()
{
    if (!m_module || m_bookCombo->currentIndex() < 0) return;
    QString label = QString("%1 %2").arg(m_bookCombo->currentText()).arg(m_chapterCombo->currentData().toInt());
    addBookmarkWithLabel(label);
}

void BibleView::onTextContextMenu(const QPoint &pos)
{
    if (!m_textView)
        return;

    QString selected = m_textView->textCursor().selectedText();
    // QTextDocument may use U+2029 as paragraph separator
    selected.replace(QChar(0x2029), QChar(' '));
    selected = selected.trimmed();
    selected = selected.simplified();

    QMenu menu(m_textView);

    QAction *actBookmark = menu.addAction(QString::fromUtf8("В закладки"));
    if (!m_module) {
        actBookmark->setEnabled(false);
    } else if (selected.isEmpty()) {
        actBookmark->setText(QString::fromUtf8("В закладки (текущая глава)"));
    }

    QAction *actCopy = menu.addAction(QString::fromUtf8("Копировать"));
    actCopy->setEnabled(!selected.isEmpty());

    menu.addSeparator();
    QAction *actSelectAll = menu.addAction(QString::fromUtf8("Выделить всё"));

    QAction *chosen = menu.exec(m_textView->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == actBookmark) {
        if (!selected.isEmpty()) {
            QString snippet = selected;
            if (snippet.length() > 60)
                snippet = snippet.left(57) + QString::fromUtf8("…");
            QString label = QString::fromUtf8("%1 %2 — «%3»")
                                .arg(m_bookCombo->currentText())
                                .arg(m_chapterCombo->currentData().toInt())
                                .arg(snippet);
            addBookmarkWithLabel(label, selected);
        } else {
            onAddBookmark();
        }
    } else if (chosen == actCopy) {
        m_textView->copy();
    } else if (chosen == actSelectAll) {
        m_textView->selectAll();
    }
}

void BibleView::revealTextSnippet(const QString &snippet)
{
    if (!m_textView || snippet.trimmed().isEmpty())
        return;

    // Только тот текст, который пользователь сохранил в закладку
    const QString needle = snippet.trimmed().simplified();
    QTextDocument *doc = m_textView->document();
    if (!doc)
        return;

    QTextCursor found = doc->find(needle, 0);
    if (found.isNull() && needle.length() > 30) {
        // допуск: первые 30 символов фразы, если в тексте чуть другая пунктуация
        found = doc->find(needle.left(30), 0);
        if (!found.isNull()) {
            // расширить выделение до длины needle по возможности
            int startPos = found.selectionStart();
            QTextCursor ext(doc);
            ext.setPosition(startPos);
            int endPos = qMin(startPos + needle.length(), doc->characterCount() - 1);
            ext.setPosition(endPos, QTextCursor::KeepAnchor);
            found = ext;
        }
    }
    if (found.isNull())
        return;

    // Жёлтая подсветка только этого фрагмента
    QTextEdit::ExtraSelection sel;
    sel.cursor = found;
    sel.format.setBackground(QColor(255, 226, 138));
    sel.format.setForeground(QColor(26, 16, 0));
    m_textView->setExtraSelections({sel});

    m_textView->setTextCursor(found);
    m_textView->ensureCursorVisible();

    QScrollBar *sb = m_textView->verticalScrollBar();
    if (sb) {
        const QRect r = m_textView->cursorRect(found);
        const int viewH = m_textView->viewport()->height();
        if (r.top() < 20 || r.bottom() > viewH - 20) {
            const int delta = r.center().y() - viewH / 2;
            sb->setValue(sb->value() + delta);
        }
    }
}


void BibleView::revealParallelVerse(int book, int chapter, int verse)
{
    if (!m_textView || !m_module || verse < 1)
        return;

    m_textView->scrollToAnchor(QStringLiteral("v%1").arg(verse));

    QString verseText = m_module->getVerseText(book, chapter, verse);
    verseText = verseText.trimmed().simplified();
    {
        const QString lead = QString::number(verse);
        if (verseText.startsWith(lead)) {
            verseText = verseText.mid(lead.size());
            if (verseText.startsWith(QLatin1Char('.')) || verseText.startsWith(QLatin1Char(')'))
                || verseText.startsWith(QLatin1Char(':')))
                verseText = verseText.mid(1);
            verseText = verseText.trimmed();
        }
    }
    if (verseText.isEmpty())
        return;

    QString needle = verseText;
    if (needle.length() > 120)
        needle = needle.left(120);

    QTextDocument *doc = m_textView->document();
    if (!doc)
        return;

    QTextCursor found = doc->find(needle, 0);
    if (found.isNull() && needle.length() > 40)
        found = doc->find(needle.left(40), 0);
    if (found.isNull())
        return;

    QTextEdit::ExtraSelection sel;
    sel.cursor = found;
    sel.format.setBackground(QColor(255, 226, 138));
    sel.format.setForeground(QColor(26, 16, 0));
    m_textView->setExtraSelections({sel});
    m_textView->setTextCursor(found);
    m_textView->ensureCursorVisible();

    QScrollBar *sb = m_textView->verticalScrollBar();
    if (sb) {
        const QRect r = m_textView->cursorRect(found);
        const int viewH = m_textView->viewport()->height();
        if (r.top() < 20 || r.bottom() > viewH - 20)
            sb->setValue(sb->value() + (r.center().y() - viewH / 2));
    }
}

void BibleView::onBookmarkClicked()
{
    auto *item = m_bookmarkList->currentItem();
    if (!item)
        return;
    const int book = item->data(Qt::UserRole).toInt();
    const int chapter = item->data(Qt::UserRole + 1).toInt();
    const QString snippet = item->data(Qt::UserRole + 2).toString().trimmed();

    m_pendingSnippet = snippet;
    // Не используем поиск по словам — только точная фраза закладки
    m_lastSearchQuery.clear();

    goTo(book, chapter, 1);

    const QString sn = snippet;
    QTimer::singleShot(80, this, [this, sn]() { revealTextSnippet(sn); });
    QTimer::singleShot(250, this, [this, sn]() { revealTextSnippet(sn); });
}

void BibleView::onRemoveBookmark()
{
    auto *item = m_bookmarkList->currentItem();
    if (!item || !m_module)
        return;

    const int book = item->data(Qt::UserRole).toInt();
    const int chapter = item->data(Qt::UserRole + 1).toInt();
    const QString label = item->text();
    const QString snippet = item->data(Qt::UserRole + 2).toString();

    // Формат: book|chapter|label|snippet
    QString entry = QString("%1|%2|%3|%4").arg(book).arg(chapter).arg(label).arg(snippet);
    QString entryOld = QString("%1|%2|%3").arg(book).arg(chapter).arg(label);

    QSettings &s = appSettings();
    const QString key = "bookmarks/" + bookmarkKey();
    QStringList list = s.value(key).toStringList();

    // Удаляем только выбранную запись
    bool removed = false;
    for (int i = list.size() - 1; i >= 0; --i) {
        const QString &e = list.at(i);
        if (e == entry || e == entryOld) {
            list.removeAt(i);
            removed = true;
            break; // только одну — выделенную
        }
        // совпадение по book|chapter|label (если snippet чуть отличается)
        QStringList parts = e.split('|');
        if (parts.size() >= 3
            && parts[0].toInt() == book
            && parts[1].toInt() == chapter
            && parts[2] == label) {
            list.removeAt(i);
            removed = true;
            break;
        }
    }
    if (removed) {
        s.setValue(key, list);
        s.sync();
        refreshBookmarkList();
        if (m_textView)
            m_textView->setExtraSelections({});
    }
}


void BibleView::onClearBookmarks()
{
    if (!m_module)
        return;
    const int ret = QMessageBox::question(
        this,
        QString::fromUtf8("Очистить закладки"),
        QString::fromUtf8("Удалить все закладки для модуля «%1»?").arg(m_module->name()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;
    QSettings &s = appSettings();
    s.remove("bookmarks/" + bookmarkKey());
    s.sync();
    refreshBookmarkList();
    if (m_textView)
        m_textView->setExtraSelections({});
}


void BibleView::restoreContentSplitter()
{
    if (!m_contentSplitter)
        return;
    QString key = QStringLiteral("panel/left");
    if (objectName() == QLatin1String("panelRight"))
        key = QStringLiteral("panel/right");
    QSettings &st = appSettings();
    const int a = st.value(key + QStringLiteral("/splitText"), -1).toInt();
    const int b = st.value(key + QStringLiteral("/splitSide"), -1).toInt();
    if (a > 50 && b > 20)
        m_contentSplitter->setSizes(QList<int>() << a << b);
}




double BibleView::scrollRatio() const
{
    if (!m_textView)
        return 0.0;
    QScrollBar *sb = m_textView->verticalScrollBar();
    if (!sb)
        return 0.0;
    const int maxv = sb->maximum();
    if (maxv <= 0)
        return 0.0;
    return double(sb->value()) / double(maxv);
}

void BibleView::setScrollRatio(double ratio)
{
    if (!m_textView)
        return;
    QScrollBar *sb = m_textView->verticalScrollBar();
    if (!sb)
        return;
    const int maxv = sb->maximum();
    if (maxv <= 0)
        return;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    const int target = int(ratio * maxv + 0.5);
    if (sb->value() != target)
        sb->setValue(target);
}

void BibleView::setActiveHighlight(bool active)
{
    m_activeHighlight = active;
    // Никакого setStyleSheet на панель — иначе QComboBox открывается «в космос»
    setStyleSheet(QString());
    if (m_activeBanner) {
        if (active) {
            m_activeBanner->setText(QString::fromUtf8("● Активное окно"));
            m_activeBanner->setStyleSheet(QStringLiteral(
                "QLabel {"
                "  background-color: rgba(255, 255, 255, 0.92);"
                "  color: #444444;"
                "  font-weight: 600;"
                "  font-size: 11px;"
                "  border: 1px solid rgba(255, 255, 255, 0.5);"
                "  border-radius: 4px;"
                "  padding: 2px 8px;"
                "}"
            ));
            m_activeBanner->setVisible(true);
        } else {
            m_activeBanner->clear();
            m_activeBanner->setVisible(false);
        }
    }
    update();
}

void BibleView::setFontSize(int points)
{
    m_fontSize = qBound(10, points, 28);
    if (m_fontLabel)
        m_fontLabel->setText(QString::number(m_fontSize));
    if (auto *lbl = findChild<QLabel*>(QStringLiteral("fontTabSizeLabel")))
        lbl->setText(QString::number(m_fontSize));
    applyTextStyle();
    loadChapter();
    savePanelState(this);
}

void BibleView::setFontFamily(const QString &family)
{
    if (family.trimmed().isEmpty())
        return;
    m_fontFamily = family.trimmed();
    if (m_fontCombo) {
        m_fontCombo->blockSignals(true);
        m_fontCombo->setCurrentFont(QFont(m_fontFamily));
        m_fontCombo->blockSignals(false);
    }
    applyTextStyle();
    loadChapter();
    savePanelState(this);
}

void BibleView::populateFontCombo()
{
    if (!m_fontCombo)
        return;
    // предпочитаемые в начале списка — через setCurrentFont
    m_fontCombo->setCurrentFont(QFont(m_fontFamily));
}

void BibleView::onFontBigger() { setFontSize(m_fontSize + 1); }
void BibleView::onFontSmaller() { setFontSize(m_fontSize - 1); }

void BibleView::applyTextStyle()
{
    if (!m_textView)
        return;
    const QString family = m_fontFamily.isEmpty() ? QStringLiteral("Noto Serif") : m_fontFamily;
    m_textView->setStyleSheet(
        QStringLiteral(
            "QTextBrowser {"
            "  background-color: #f7f0e0;"
            "  color: #2c1810;"
            "  border: 1px solid #d4c4a8;"
            "  border-radius: 4px;"
            "  padding: 14px;"
            "  font-family: \"%1\", \"Noto Serif\", \"Gentium Plus\", \"Noto Serif Hebrew\", \"FreeSerif\", serif;"
            "  font-size: %2pt;"
            "  selection-background-color: #e0d0a0;"
            "}"
            "QTextBrowser QScrollBar:vertical {"
            "  background: #f0e6d0; width: 14px; margin: 0; border-radius: 7px;"
            "}"
            "QTextBrowser QScrollBar::handle:vertical {"
            "  background: #c4b090; min-height: 32px; border-radius: 7px; border: 1px solid #a89070;"
            "}"
            "QTextBrowser QScrollBar::handle:vertical:hover { background: #b09a78; }"
            "QTextBrowser QScrollBar::add-line:vertical,"
            "QTextBrowser QScrollBar::sub-line:vertical { height: 0; background: none; }"
            "QTextBrowser QScrollBar::add-page:vertical,"
            "QTextBrowser QScrollBar::sub-page:vertical { background: #f0e6d0; }"
            "QTextBrowser QScrollBar:horizontal {"
            "  background: #f0e6d0; height: 14px; margin: 0; border-radius: 7px;"
            "}"
            "QTextBrowser QScrollBar::handle:horizontal {"
            "  background: #c4b090; min-width: 32px; border-radius: 7px; border: 1px solid #a89070;"
            "}"
            "QTextBrowser QScrollBar::handle:horizontal:hover { background: #b09a78; }"
            "QTextBrowser QScrollBar::add-line:horizontal,"
            "QTextBrowser QScrollBar::sub-line:horizontal { width: 0; background: none; }"
            "QTextBrowser QScrollBar::add-page:horizontal,"
            "QTextBrowser QScrollBar::sub-page:horizontal { background: #f0e6d0; }"
        ).arg(family, QString::number(m_fontSize))
    );
}

namespace {

bool isNewTestamentBookName(const QString &fullName)
{
    const QString n = fullName.trimmed().toLower()
                          .replace(QString::fromUtf8("ё"), QString::fromUtf8("е"));
    // Новый Завет начинается с Евангелий
    if (n.contains(QString::fromUtf8("иудиф")))
        return false;
    if (n.contains(QString::fromUtf8("от матфе")) || n.startsWith(QString::fromUtf8("матфе"))
        || n.contains(QString::fromUtf8("ев. от матф")))
        return true;
    if (n.contains(QString::fromUtf8("от марк")) || n.startsWith(QString::fromUtf8("марк")))
        return true;
    if (n.contains(QString::fromUtf8("от лук")) || n.startsWith(QString::fromUtf8("лук")))
        return true;
    if (n.contains(QString::fromUtf8("от иоанн")) || n.contains(QString::fromUtf8("евангелие от иоанн")))
        return true;
    if (n.startsWith(QString::fromUtf8("деяния")) || n.contains(QString::fromUtf8("деяния апостол")))
        return true;
    if (n.contains(QString::fromUtf8("римлян")))
        return true;
    if (n.contains(QString::fromUtf8("коринф")))
        return true;
    if (n.contains(QString::fromUtf8("галат")))
        return true;
    if (n.contains(QString::fromUtf8("ефес")))
        return true;
    if (n.contains(QString::fromUtf8("филиппи")))
        return true;
    if (n.contains(QString::fromUtf8("колосс")))
        return true;
    if (n.contains(QString::fromUtf8("фессалон")))
        return true;
    if (n.contains(QString::fromUtf8("тимоф")))
        return true;
    if (n.startsWith(QString::fromUtf8("титу")) || n.contains(QString::fromUtf8("к титу")))
        return true;
    if (n.contains(QString::fromUtf8("филимон")))
        return true;
    if (n.contains(QString::fromUtf8("евреям")))
        return true;
    if (n.contains(QString::fromUtf8("иаков")))
        return true;
    if (n.contains(QString::fromUtf8("петра")))
        return true;
    if (n.contains(QString::fromUtf8("иоанна")) && (n.contains(QLatin1Char('1')) || n.contains(QLatin1Char('2'))
            || n.contains(QLatin1Char('3')) || n.contains(QString::fromUtf8("1-"))
            || n.contains(QString::fromUtf8("2-")) || n.contains(QString::fromUtf8("3-"))))
        return true;
    if (n.contains(QString::fromUtf8("иуды")) || n.startsWith(QString::fromUtf8("иуда"))
        || n.contains(QString::fromUtf8("послание иуды")))
        return true;
    if (n.contains(QString::fromUtf8("откровен")) || n.contains(QString::fromUtf8("апокалипс")))
        return true;
    // MyBible / числовые id часто: >= 470 Матфей
    return false;
}

/** Делегат: заголовки ВЗ/НЗ по центру выпадающего списка */
class BookComboDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const int bookId = index.data(Qt::UserRole).toInt();
        if (bookId <= 0) {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            opt.displayAlignment = Qt::AlignCenter;
            // Яркий цвет заголовка
            opt.palette.setColor(QPalette::Text, QColor(0xe6, 0x7e, 0x00));
            opt.palette.setColor(QPalette::WindowText, QColor(0xe6, 0x7e, 0x00));
            opt.palette.setColor(QPalette::HighlightedText, QColor(0xe6, 0x7e, 0x00));
            QFont f = opt.font;
            f.setBold(true);
            opt.font = f;
            opt.fontMetrics = QFontMetrics(f);

            painter->save();
            // Фон как у остальных пунктов
            if (opt.state & QStyle::State_Selected)
                painter->fillRect(opt.rect, opt.palette.highlight());
            else
                painter->fillRect(opt.rect, opt.palette.base());

            painter->setPen(QColor(0xe6, 0x7e, 0x00));
            painter->setFont(f);
            painter->drawText(opt.rect, Qt::AlignCenter, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
};

void ensureBookComboDelegate(QComboBox *combo)
{
    if (!combo || !combo->view())
        return;
    if (combo->view()->itemDelegate() && combo->view()->itemDelegate()->property("bookComboDelegate").toBool())
        return;
    auto *del = new BookComboDelegate(combo);
    del->setProperty("bookComboDelegate", true);
    combo->setItemDelegate(del);
    if (auto *lv = qobject_cast<QListView *>(combo->view()))
        lv->setTextElideMode(Qt::ElideNone);
}

void addBookComboHeader(QComboBox *combo, const QString &title)
{
    if (!combo)
        return;
    ensureBookComboDelegate(combo);
    const int row = combo->count();
    combo->addItem(title);
    combo->setItemData(row, -1); // не книга
    combo->setItemData(row, int(Qt::AlignCenter), Qt::TextAlignmentRole);
    if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
        if (QStandardItem *it = model->item(row)) {
            it->setFlags(Qt::ItemIsEnabled); // виден, но ниже отключим selectable через id
            it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
            it->setForeground(QBrush(QColor(0xe6, 0x7e, 0x00)));
            it->setTextAlignment(Qt::AlignCenter);
            it->setData(int(Qt::AlignCenter), Qt::TextAlignmentRole);
        }
    }
}

} // namespace

void BibleView::setModule(Module *module)
{
    const bool prevBlock = m_blockSettingsSave;
    m_blockSettingsSave = true;

    m_module = module;
    m_bookCombo->clear();
    ensureBookComboDelegate(m_bookCombo);
    m_searchResults->clear();
    m_parallelList->clear();

    if (!m_module || !m_module->isValid()) {
        m_moduleLabel->setText(QString::fromUtf8("Модуль не выбран"));
        m_textView->setHtml(QString::fromUtf8("<p style='color:#666'>Выберите модуль.</p>"));
        m_bookmarkList->clear();
        m_blockSettingsSave = prevBlock;
        return;
    }

    m_moduleLabel->setText(m_module->name());

    const QVector<BookInfo> books = m_module->books();
    const bool showTestaments = m_module->isBibleText() && !m_module->isDictionary()
                                && books.size() >= 10;
    bool otHeader = false;
    bool ntHeader = false;
    for (const auto &book : books) {
        if (showTestaments) {
            const bool isNT = isNewTestamentBookName(book.fullName)
                              || book.id >= 470; // MyBible: Матфей = 470
            // Порядковый канон 1..66: НЗ с 40
            const bool isNTSeq = (book.id >= 40 && book.id <= 66 && book.id < 100);
            const bool nt = isNT || isNTSeq;
            if (!nt && !otHeader) {
                addBookComboHeader(m_bookCombo, QString::fromUtf8("—— Ветхий Завет ——"));
                otHeader = true;
            }
            if (nt && !ntHeader) {
                addBookComboHeader(m_bookCombo, QString::fromUtf8("—— Новый Завет ——"));
                ntHeader = true;
            }
        }
        m_bookCombo->addItem(book.fullName, book.id);
    }

    loadBookmarks();

    // Первая реальная книга, не заголовок
    int first = 0;
    while (first < m_bookCombo->count() && m_bookCombo->itemData(first).toInt() <= 0)
        ++first;
    if (first < m_bookCombo->count()) {
        m_bookCombo->setCurrentIndex(first);
        onBookChanged(first);
    }
    m_blockSettingsSave = prevBlock;
}

void BibleView::goTo(int book, int chapter, int verse)
{
    if (!m_module) return;
    const bool prevBlock = m_blockSettingsSave;
    m_blockSettingsSave = true;
    for (int i = 0; i < m_bookCombo->count(); ++i) {
        if (m_bookCombo->itemData(i).toInt() == book) {
            m_bookCombo->blockSignals(true);
            m_bookCombo->setCurrentIndex(i);
            m_bookCombo->blockSignals(false);
            break;
        }
    }
    m_currentBook = book;
    fillChapterCombo(book, chapter);
    m_currentChapter = m_chapterCombo->currentData().toInt();
    m_currentVerse = verse;
    loadChapter();
    refillVerseCombo(verse);
    if (verse > 1) {
        QTimer::singleShot(80, this, [this, book, chapter, verse]() {
            revealParallelVerse(book, chapter, verse);
        });
    }
    m_blockSettingsSave = prevBlock;
    addHistoryEntry(book, chapter, verse > 0 ? verse : 1);
    savePanelState(this);
    setFocus();
}

void BibleView::onBookChanged(int index)
{
    if (!m_module || index < 0)
        return;
    const int bookId = m_bookCombo->itemData(index).toInt();
    // Заголовок «Ветхий/Новый Завет» — не выбирать
    if (bookId <= 0) {
        int next = index + 1;
        while (next < m_bookCombo->count() && m_bookCombo->itemData(next).toInt() <= 0)
            ++next;
        if (next < m_bookCombo->count()) {
            m_bookCombo->blockSignals(true);
            m_bookCombo->setCurrentIndex(next);
            m_bookCombo->blockSignals(false);
            index = next;
        } else {
            return;
        }
    }
    m_currentBook = m_bookCombo->itemData(index).toInt();
    fillChapterCombo(m_currentBook, 1);
    m_currentChapter = m_chapterCombo->currentData().toInt();
    m_currentVerse = 1;
    loadChapter();
    refillVerseCombo(1);
    if (!m_blockSettingsSave)
        addHistoryEntry(m_currentBook, m_currentChapter, 1);
    savePanelState(this);
}

void BibleView::onChapterChanged(int chapter)
{
    if (!m_module)
        return;
    m_currentChapter = chapter;
    m_currentVerse = 1;
    loadChapter();
    if (!m_blockSettingsSave)
        addHistoryEntry(m_currentBook, m_currentChapter, 1);
    refillVerseCombo(1);
    savePanelState(this);
}


void BibleView::refillVerseCombo(int selectVerse)
{
    fillVerseCombo(m_currentBook, m_currentChapter, selectVerse);
}

void BibleView::onVerseChanged(int index)
{
    if (index < 0 || !m_verseCombo)
        return;
    const int v = m_verseCombo->itemData(index).toInt();
    if (v < 0)
        return;
    m_currentVerse = v;
    if (v > 0 && m_module && m_currentBook > 0 && m_currentChapter > 0) {
        QTimer::singleShot(30, this, [this, v]() {
            revealParallelVerse(m_currentBook, m_currentChapter, v);
        });
    }
    savePanelState(this);
}

void BibleView::loadChapter()
{
    if (!m_module || !m_textView) return;
    int index = m_bookCombo->currentIndex();
    if (index < 0) {
        m_textView->setHtml(QString::fromUtf8("<p style='color:#666'>Выберите книгу.</p>"));
        return;
    }

    m_currentBook = m_bookCombo->itemData(index).toInt();
    m_currentChapter = m_chapterCombo->currentData().toInt();
    if (m_currentBook <= 0 || m_currentChapter < 0) {
        m_textView->setHtml(QString::fromUtf8("<p style='color:#666'>Некорректная ссылка.</p>"));
        return;
    }

    QString html = m_module->getChapterText(m_currentBook, m_currentChapter, m_lastSearchQuery);
    // Ссылки MyBible B:книга глава:стих → кликабельный mbible:
    {
        static const QRegularExpression re1(
            QStringLiteral("href\\s*=\\s*['\"]B:(\\d+)\\s+(\\d+):(\\d+)['\"]"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression re2(
            QStringLiteral("href\\s*=\\s*['\"]B:(\\d+):(\\d+):(\\d+)['\"]"),
            QRegularExpression::CaseInsensitiveOption);
        html.replace(re1, QStringLiteral("href=\"mbible:\\1/\\2/\\3\""));
        html.replace(re2, QStringLiteral("href=\"mbible:\\1/\\2/\\3\""));
    }

    if (html.isEmpty()) {
        m_textView->setHtml(QString::fromUtf8(
            "<p style='color:#666'>Нет текста для %1, глава %2.</p>")
            .arg(m_bookCombo->currentText()).arg(m_currentChapter));
    } else {
        const QString type = m_module->type().toLower();
        const bool isDictionary = type.contains(QLatin1String("dict"))
                                  || type.contains(QString::fromUtf8("слов"))
                                  || type.contains(QLatin1String("concord"));
        const QString chapterTitle = (m_currentChapter == 0)
                                         ? QString::fromUtf8("Введение")
                                         : QString::number(m_currentChapter);
        const QString headerText = isDictionary
                                       ? m_bookCombo->currentText().toHtmlEscaped()
                                       : QString::fromUtf8("%1 %2")
                                             .arg(m_bookCombo->currentText().toHtmlEscaped(),
                                                  chapterTitle);
        QString header = QString(
            "<div style='margin:0 0 0.55em 0;'>"
            "<div style='font-weight:700; font-size:1.4em; color:#2c1810; margin:0 0 0.4em 0;'>%1</div>"
            "</div>")
                             .arg(headerText);
        m_textView->setHtml(header + html);
        m_textView->moveCursor(QTextCursor::Start);
    }
    showParallelForCurrent();
    emit verseChanged(m_currentBook, m_currentChapter, m_currentVerse);
}


void BibleView::showParallelForCurrent()
{
    showParallelsForVerse(m_currentBook, m_currentChapter, m_currentVerse);
}


void BibleView::scrollToSearchMatch(const QString &query, int verse)
{
    if (!m_textView || query.trimmed().isEmpty())
        return;

    // 1) Сначала к якорю стиха
    if (verse > 0)
        m_textView->scrollToAnchor(QStringLiteral("v%1").arg(verse));

    QTextDocument *doc = m_textView->document();
    if (!doc)
        return;

    const QString phrase = query.trimmed().simplified();
    if (phrase.isEmpty())
        return;

    auto findNeedle = [&](const QString &needle) -> QTextCursor {
        if (needle.isEmpty())
            return QTextCursor();
        QTextCursor c = doc->find(needle, 0);
        if (c.isNull())
            c = doc->find(needle, QTextCursor(doc));
        return c;
    };

    // 2) Ищем целую фразу, затем отдельные слова
    QTextCursor found = findNeedle(phrase);
    if (found.isNull()) {
        // без лишних пробелов / регистр — Qt find по умолчанию case-insensitive
        const QStringList parts = phrase.split(QRegularExpression(QStringLiteral("\\s+")),
                                               Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            // пробуем первые 2–3 слова как подфразу
            found = findNeedle(parts.mid(0, qMin(3, parts.size())).join(QLatin1Char(' ')));
        }
        if (found.isNull() && !parts.isEmpty())
            found = findNeedle(parts.first());
    }
    if (found.isNull())
        return;

    QTextEdit::ExtraSelection sel;
    sel.cursor = found;
    sel.format.setBackground(QColor(255, 226, 138));
    sel.format.setForeground(QColor(26, 16, 0));
    m_textView->setExtraSelections({sel});

    m_textView->setTextCursor(found);
    m_textView->ensureCursorVisible();

    QScrollBar *sb = m_textView->verticalScrollBar();
    if (sb) {
        const QRect r = m_textView->cursorRect(found);
        const int viewH = m_textView->viewport()->height();
        if (r.top() < 40 || r.bottom() > viewH - 40) {
            const int delta = r.center().y() - viewH / 3;
            sb->setValue(sb->value() + delta);
        }
    }
}


void BibleView::onSearch()
{
    m_searchResults->clear();
    if (!m_module)
        return;

    const QString q = m_searchEdit->text().trimmed();
    m_lastSearchQuery = q;
    if (q.isEmpty()) {
        m_highlightVerse = 0;
        loadChapter();
        return;
    }

    // Поиск по ВСЕМУ модулю (все книги), не только текущая глава
    const int booksTotal = m_module->books().size();
    auto results = m_module->search(q, 10000);

    QSet<int> booksHit;
    for (const auto &r : results)
        booksHit.insert(r.book);

    for (const auto &r : results) {
        QString label = QString("%1 %2:%3 — %4")
                            .arg(r.bookName).arg(r.chapter).arg(r.verse).arg(r.snippet);
        auto *item = new QListWidgetItem(label, m_searchResults);
        item->setData(Qt::UserRole, r.book);
        item->setData(Qt::UserRole + 1, r.chapter);
        item->setData(Qt::UserRole + 2, r.verse);
    }
    if (results.isEmpty()) {
        m_searchResults->addItem(QString::fromUtf8(
            "Ничего не найдено по всем %1 книгам модуля").arg(booksTotal));
        m_highlightVerse = 0;
        loadChapter();
        return;
    }

    m_searchResults->insertItem(0, QString::fromUtf8(
        "Найдено: %1 в %2 книгах (из %3) — поиск по всему модулю")
        .arg(results.size()).arg(booksHit.size()).arg(booksTotal));
    m_searchResults->item(0)->setFlags(Qt::ItemIsEnabled);

    // Перейти к первому результату, подсветить, прокрутить к слову
    const auto &first = results.first();
    m_highlightVerse = first.verse;
    goTo(first.book, first.chapter, first.verse);
    m_searchResults->setCurrentRow(1); // строка после заголовка

    const QString qFind = m_lastSearchQuery;
    const int verse = first.verse;
    QTimer::singleShot(60, this, [this, qFind, verse]() {
        scrollToSearchMatch(qFind, verse);
    });
    QTimer::singleShot(220, this, [this, qFind, verse]() {
        scrollToSearchMatch(qFind, verse);
    });
}


void BibleView::onClearSearchResults()
{
    if (m_searchResults)
        m_searchResults->clear();
    if (m_searchEdit)
        m_searchEdit->clear();
    m_lastSearchQuery.clear();
    m_highlightVerse = 0;
    if (m_textView)
        m_textView->setExtraSelections({});
    loadChapter(); // перерисовать без подсветки поиска
}

void BibleView::onSearchResultClicked()
{
    auto *item = m_searchResults->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid()) return;
    m_highlightVerse = item->data(Qt::UserRole + 2).toInt();
    if (!m_searchEdit->text().trimmed().isEmpty())
        m_lastSearchQuery = m_searchEdit->text().trimmed();
    goTo(item->data(Qt::UserRole).toInt(),
         item->data(Qt::UserRole + 1).toInt(),
         m_highlightVerse);

    // После загрузки главы: прокрутить к найденному слову и подсветить
    const QString q = m_lastSearchQuery;
    const int verse = m_highlightVerse;
    QTimer::singleShot(60, this, [this, q, verse]() {
        scrollToSearchMatch(q, verse);
    });
    QTimer::singleShot(200, this, [this, q, verse]() {
        scrollToSearchMatch(q, verse);
    });
}

void BibleView::showParallelsForVerse(int book, int chapter, int verse)
{
    if (!m_parallelList)
        return;
    m_parallelList->clear();
    m_currentVerse = verse;

    const auto refs = Module::lookupCrossRefs(book, chapter, verse);
    if (refs.isEmpty()) {
        m_parallelList->addItem(QString::fromUtf8("Для %1 параллели не найдены")
                                    .arg(Module::formatRef(book, chapter, verse)));
        return;
    }

    auto *header = new QListWidgetItem(
        QString::fromUtf8("Параллели для %1 (%2)")
            .arg(Module::formatRef(book, chapter, verse))
            .arg(refs.size()),
        m_parallelList);
    header->setFlags(Qt::ItemIsEnabled);

    int shown = 0;
    const int maxShow = 200;
    for (const auto &cr : refs) {
        if (shown >= maxShow) {
            auto *more = new QListWidgetItem(
                QString::fromUtf8("… и ещё %1 (показаны первые %2)")
                    .arg(refs.size() - maxShow).arg(maxShow),
                m_parallelList);
            more->setFlags(Qt::ItemIsEnabled);
            break;
        }
        QString label = cr.display;
        if (!cr.note.isEmpty()) {
            QString n = cr.note;
            if (n == QLatin1String("tsk"))
                n = QString::fromUtf8("TSK");
            else if (n == QLatin1String("apocrypha"))
                n = QString::fromUtf8("апокрифы");
            else if (n == QLatin1String("synoptic"))
                n = QString::fromUtf8("синоптики");
            label += QString::fromUtf8("  · %1").arg(n);
        }
        auto *item = new QListWidgetItem(label, m_parallelList);
        item->setData(Qt::UserRole, cr.refBook);
        item->setData(Qt::UserRole + 1, cr.refChapter);
        item->setData(Qt::UserRole + 2, cr.refVerseStart);
        item->setToolTip(QString::fromUtf8("Перейти к %1").arg(cr.display));
        ++shown;
    }
}

void BibleView::onAnchorClicked(const QUrl &url)
{
    // Ссылки симфонии/MyBible: mbible:book/chapter/verse  или  B:book chapter:verse
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("mbible") || scheme == QLatin1String("b")) {
        int book = 0, chapter = 0, verse = 0;
        if (scheme == QLatin1String("mbible")) {
            // mbible:10/1/1  или path /10/1/1
            QString p = url.path();
            if (p.startsWith(QLatin1Char('/')))
                p = p.mid(1);
            if (p.isEmpty())
                p = url.toString(QUrl::RemoveScheme);
            const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.size() >= 3) {
                book = parts[0].toInt();
                chapter = parts[1].toInt();
                verse = parts[2].toInt();
            }
        } else {
            // B:20 28:40
            QString s = url.toString();
            if (s.startsWith(QStringLiteral("B:"), Qt::CaseInsensitive))
                s = s.mid(2);
            QRegularExpression re(QStringLiteral("^\\s*(\\d+)\\s+(\\d+):(\\d+)"));
            auto m = re.match(s);
            if (m.hasMatch()) {
                book = m.captured(1).toInt();
                chapter = m.captured(2).toInt();
                verse = m.captured(3).toInt();
            }
        }

        if (book > 0 && chapter > 0 && verse > 0) {
            // Имя книги по номеру MyBible (русские модули)
            static const QHash<int, QString> kBooks = []() {
                QHash<int, QString> h;
                const struct { int n; const char *name; } t[] = {
                    {10,"Бытие"},{20,"Исход"},{30,"Левит"},{40,"Числа"},{50,"Второзаконие"},
                    {60,"Иисус Навин"},{70,"Судьи"},{80,"Руфь"},
                    {90,"1 Царств"},{100,"2 Царств"},{110,"3 Царств"},{120,"4 Царств"},
                    {130,"1 Паралипоменон"},{140,"2 Паралипоменон"},{150,"Ездра"},{160,"Неемия"},
                    {190,"Есфирь"},{220,"Иов"},{230,"Псалтирь"},{240,"Притчи"},{250,"Екклесиаст"},
                    {260,"Песнь Песней"},{290,"Исаия"},{300,"Иеремия"},{310,"Плач Иеремии"},
                    {330,"Иезекииль"},{340,"Даниил"},{350,"Осия"},{360,"Иоиль"},{370,"Амос"},
                    {380,"Авдий"},{390,"Иона"},{400,"Михей"},{410,"Наум"},{420,"Аввакум"},
                    {430,"Софония"},{440,"Аггей"},{450,"Захария"},{460,"Малахия"},
                    {470,"Матфей"},{480,"Марк"},{490,"Лука"},{500,"Иоанн"},{510,"Деяния"},
                    {520,"Иаков"},{530,"1 Петра"},{540,"2 Петра"},{550,"1 Иоанна"},
                    {560,"2 Иоанна"},{570,"3 Иоанна"},{580,"Иуда"},{590,"Римлянам"},
                    {600,"1 Коринфянам"},{610,"2 Коринфянам"},{620,"Галатам"},{630,"Ефесянам"},
                    {640,"Филиппийцам"},{650,"Колоссянам"},{660,"1 Фессалоникийцам"},
                    {670,"2 Фессалоникийцам"},{680,"1 Тимофею"},{690,"2 Тимофею"},
                    {700,"Титу"},{710,"Филимону"},{720,"Евреям"},{730,"Откровение"},
                    // второканонические (часто в русских модулях)
                    {165,"2 Ездры"},{170,"Товит"},{180,"Иудифь"},{270,"Премудрость Соломона"},
                    {280,"Премудрость Сираха"},{315,"Послание Иеремии"},{320,"Варух"},
                    {462,"1 Маккавейская"},{464,"2 Маккавейская"},{466,"3 Маккавейская"}
                };
                for (const auto &e : t)
                    h.insert(e.n, QString::fromUtf8(e.name));
                return h;
            }();

            const QString bookName = kBooks.value(book, QString::fromUtf8("Книга %1").arg(book));
            const QString ref = QStringLiteral("%1 %2:%3").arg(bookName).arg(chapter).arg(verse);

            // Пытаемся взять текст стиха из установленного синодального/библейского модуля
            QString verseHtml;
            const QStringList roots = {
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/modules"),
                QCoreApplication::applicationDirPath() + QStringLiteral("/../share/bibleapp/modules"),
                QStringLiteral("/usr/share/bibleapp/modules")
            };
            for (const QString &root : roots) {
                QDir dir(root);
                if (!dir.exists())
                    continue;
                for (const QString &folder : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    Module mod(dir.absoluteFilePath(folder));
                    if (!mod.isValid() || !mod.isBibleText())
                        continue;
                    // сначала пробуем id = номер MyBible, затем порядковый поиск по имени
                    QString t = mod.getVerseText(book, chapter, verse);
                    if (t.isEmpty()) {
                        // поиск книги по имени
                        for (const auto &bi : mod.books()) {
                            if (bi.fullName.contains(bookName, Qt::CaseInsensitive)
                                || bookName.contains(bi.fullName, Qt::CaseInsensitive)) {
                                t = mod.getVerseText(bi.id, chapter, verse);
                                break;
                            }
                        }
                    }
                    if (!t.isEmpty()) {
                        verseHtml = t;
                        break;
                    }
                }
                if (!verseHtml.isEmpty())
                    break;
            }

            QString html = QStringLiteral("<div class='ref-title'>%1</div>")
                .arg(ref.toHtmlEscaped());
            if (!verseHtml.isEmpty()) {
                html += QStringLiteral("<div class='verse-text'>%1</div>").arg(verseHtml);
            } else {
                html += QString::fromUtf8(
                    "<p class='hint'>Текст стиха не найден в установленных библейских модулях.<br/>"
                    "Откройте синодальный перевод в другом окне или установите модуль Библии.</p>");
            }
            showWordInfo(html);
        }
        return;
    }

    if (url.scheme() == QStringLiteral("v")) {
        // Клик по номеру стиха: параллели + Стронг
        QString path = url.path();
        if (path.startsWith('/')) path = path.mid(1);
        // host may hold book if URL is v://book/chapter/verse
        QStringList parts = path.split('/');
        if (parts.size() < 3 && !url.host().isEmpty()) {
            parts = (QStringList() << url.host()) + parts;
        }
        if (parts.size() >= 3) {
            int book = parts[0].toInt();
            int chapter = parts[1].toInt();
            int verse = parts[2].toInt();
            if (book <= 0) book = m_currentBook;
            if (chapter <= 0) chapter = m_currentChapter;

            // Только параллельные места справа — панель греческого слова не трогаем
            showParallelsForVerse(book, chapter, verse);
        }
        return;
    }
    if (url.scheme() != QStringLiteral("w"))
        return;

    QUrlQuery q(url);
    QString word = q.queryItemValue(QStringLiteral("g"));
    QString meaning = q.queryItemValue(QStringLiteral("m"));
    QString strong = q.queryItemValue(QStringLiteral("s"));

    if (word.isEmpty()) {
        QString s = url.toString();
        if (s.startsWith("w:"))
            s = s.mid(2);
        int bar = s.indexOf('|');
        if (bar >= 0) {
            word = QUrl::fromPercentEncoding(s.left(bar).toUtf8());
            meaning = QUrl::fromPercentEncoding(s.mid(bar + 1).toUtf8());
        } else {
            word = QUrl::fromPercentEncoding(s.toUtf8());
        }
    }

    QString html;
    if (!strong.isEmpty()) {
        QString article = Module::lookupStrongArticle(strong);
        if (!article.isEmpty()) {
            html = article;
        }
    }
    if (html.isEmpty()) {
        if (meaning.isEmpty())
            meaning = Module::lookupGloss(word);
        if (meaning.isEmpty() || meaning == QString::fromUtf8("—"))
            meaning = Module::lookupStrongGloss(strong);
        if (meaning.isEmpty() || meaning == QString::fromUtf8("—"))
            meaning = QString::fromUtf8("перевод не найден");

        html = QString::fromUtf8("<b>%1</b> — %2").arg(word.toHtmlEscaped(), meaning.toHtmlEscaped());
        if (!strong.isEmpty())
            html = QString::fromUtf8("<b>%1</b> · ").arg(strong.toHtmlEscaped()) + html;

        QString thayer = Module::lookupThayer(word);
        if (!thayer.isEmpty()) {
            QString shortT = thayer;
            if (shortT.length() > 900)
                shortT = shortT.left(900) + "...";
            html += QString::fromUtf8("<br><span style='color:#333; font-size:12px;'><b>Thayer:</b> %1</span>")
                        .arg(shortT.toHtmlEscaped());
        }
    }
    showWordInfo(html);
}

void BibleView::showWordInfo(const QString &html)
{
    if (!m_wordWindow) {
        // Родитель — главное окно приложения, чтобы не привязываться к панели
        QWidget *top = window();
        m_wordWindow = new WordInfoWindow(top);
    }
    m_wordWindow->setHtml(html);
}

void BibleView::addHistoryEntry(int book, int chapter, int verse)
{
    if (!m_module || book <= 0 || chapter < 0)
        return;
    const QString modName = m_module->name();
    QString bookName = modName;
    for (const auto &b : m_module->books()) {
        if (b.id == book) {
            bookName = b.fullName;
            break;
        }
    }
    // формат: module|bookId|chapter|verse|bookName
    const QString entry = QStringLiteral("%1|%2|%3|%4|%5")
                              .arg(modName)
                              .arg(book)
                              .arg(chapter)
                              .arg(verse > 0 ? verse : 1)
                              .arg(bookName);

    QSettings &s = appSettings();
    QStringList list = s.value(QStringLiteral("history/entries")).toStringList();
    // убрать дубликат того же места
    list.removeAll(entry);
    list.prepend(entry);
    while (list.size() > 200)
        list.removeLast();
    s.setValue(QStringLiteral("history/entries"), list);
    s.sync();
    refreshHistoryList();
}

void BibleView::loadHistory()
{
    refreshHistoryList();
}

void BibleView::saveHistory()
{
    // уже пишется в addHistoryEntry / onClearHistory
}

void BibleView::refreshHistoryList()
{
    if (!m_historyList)
        return;
    m_historyList->clear();
    QSettings &s = appSettings();
    const QStringList list = s.value(QStringLiteral("history/entries")).toStringList();
    for (const QString &raw : list) {
        const QStringList p = raw.split(QLatin1Char('|'));
        if (p.size() < 5)
            continue;
        const QString mod = p[0];
        const int book = p[1].toInt();
        const int chapter = p[2].toInt();
        const int verse = p[3].toInt();
        const QString bookName = p[4];
        QString label = QString::fromUtf8("%1 — %2 %3:%4")
                            .arg(mod, bookName)
                            .arg(chapter)
                            .arg(verse);
        auto *item = new QListWidgetItem(label, m_historyList);
        item->setData(Qt::UserRole, mod);
        item->setData(Qt::UserRole + 1, book);
        item->setData(Qt::UserRole + 2, chapter);
        item->setData(Qt::UserRole + 3, verse);
    }
}

void BibleView::onHistoryClicked()
{
    if (!m_historyList)
        return;
    auto *item = m_historyList->currentItem();
    if (!item || !item->data(Qt::UserRole + 1).isValid())
        return;
    const QString modName = item->data(Qt::UserRole).toString();
    const int book = item->data(Qt::UserRole + 1).toInt();
    const int chapter = item->data(Qt::UserRole + 2).toInt();
    const int verse = item->data(Qt::UserRole + 3).toInt();

    // если другой модуль — ищем среди загруженных через менеджер нельзя отсюда;
    // переходим в текущем модуле, если имя совпадает; иначе всё равно goTo по книге
    if (m_module && m_module->name() == modName) {
        goTo(book, chapter, verse);
        return;
    }
    // попробовать найти модуль на диске через путь modules
    // просто goTo — если книги есть в текущем модуле
    goTo(book, chapter, verse);
}

void BibleView::onClearHistory()
{
    QSettings &s = appSettings();
    s.remove(QStringLiteral("history/entries"));
    s.sync();
    if (m_historyList)
        m_historyList->clear();
}


