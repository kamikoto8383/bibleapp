#include "MainWindow.h"
#include "BibleView.h"
#include "ModuleManagerDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QApplication>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSplitter>
#include <QList>
#include <QMenuBar>
#include <QMenu>
#include <QTextBrowser>
#include <QActionGroup>

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setMinimumSize(1000, 650);
    resize(1280, 800);
    setWindowTitle(QString::fromUtf8("Программа по изучению БИБЛИИ"));

    setupUi();
    loadSettings();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        storeSplitterSizes();
        saveSettings();
    });

    m_moduleManager.ensureTestModule();
    m_moduleManager.loadModules();

    QSettings &settings = appSettings();
    // Число окон
    int count = settings.value(QStringLiteral("ui/panelCount"), 2).toInt();
    if (count < 1) count = 1;
    if (count > MaxPanels) count = MaxPanels; // макс. 3 окна
    setPanelCount(count);

    auto restoreView = [this, &settings](BibleView *view, const QString &keyPrefix, bool fallbackFirst) {
        if (!view) return;
        const QString modName = settings.value(keyPrefix + "/module").toString();
        Module *mod = nullptr;
        if (!modName.isEmpty())
            mod = m_moduleManager.moduleByName(modName);
        if (!mod && fallbackFirst && !m_moduleManager.modules().isEmpty())
            mod = m_moduleManager.moduleAt(0);
        if (mod) {
            view->setModule(mod);
            int book = settings.value(keyPrefix + "/book", 0).toInt();
            int chapter = settings.value(keyPrefix + "/chapter", 1).toInt();
            if (book > 0)
                view->goTo(book, qMax(1, chapter), 1);
        }
    };

    for (BibleView *v : m_views)
        if (v) v->setSettingsSaveBlocked(true);

    // Новые ключи panel/0..3; совместимость с panel/left, panel/right
    for (int i = 0; i < m_views.size(); ++i) {
        QString key = panelKey(i);
        if (i == 0 && !settings.contains(key + "/module") && settings.contains(QStringLiteral("panel/left/module")))
            key = QStringLiteral("panel/left");
        if (i == 1 && !settings.contains(panelKey(1) + "/module") && settings.contains(QStringLiteral("panel/right/module")))
            key = QStringLiteral("panel/right");
        restoreView(m_views[i], key, i == 0);
    }

    for (BibleView *v : m_views)
        if (v) v->setSettingsSaveBlocked(false);

    saveSettings();

    if (m_statusLabel && !m_views.isEmpty() && m_views[0] && m_views[0]->module())
        m_statusLabel->setText(QString::fromUtf8("Загружен модуль: %1")
                                   .arg(m_views[0]->module()->name()));
}

MainWindow::~MainWindow()
{
    saveSettings();
}

QString MainWindow::panelKey(int index) const
{
    return QStringLiteral("panel/%1").arg(index);
}

int MainWindow::indexOfView(BibleView *view) const
{
    return m_views.indexOf(view);
}

void MainWindow::wireView(BibleView *view)
{
    if (!view)
        return;
    connect(view, &BibleView::requestLoadModule, this, [this, view]() {
        openModuleManagerFor(view);
    });
    connect(view, &BibleView::verseChanged, this, [this, view](int book, int chapter, int verse) {
        syncOtherView(view, book, chapter, verse);
    });
    connect(view, &BibleView::scrollRatioChanged, this, [this, view](double ratio) {
        syncScrollFrom(view, ratio);
    });
    view->installEventFilter(this);
}

void MainWindow::setupUi()
{
    // Меню «Модули»
    auto *menuModules = menuBar()->addMenu(QString::fromUtf8("Модули"));
    auto *actManager = menuModules->addAction(QString::fromUtf8("Менеджер модулей…"));
    connect(actManager, &QAction::triggered, this, [this]() {
        BibleView *target = m_views.isEmpty() ? nullptr : m_views[0];
        for (BibleView *v : m_views) {
            if (v && v->isActiveHighlight()) {
                target = v;
                break;
            }
        }
        openModuleManagerFor(target);
    });

    // Меню «Окно» — выбор 1..4 панелей
    auto *menuWindow = menuBar()->addMenu(QString::fromUtf8("Окно"));
    m_windowGroup = new QActionGroup(this);
    m_windowGroup->setExclusive(true);
    const char *labels[MaxPanels] = {
        "1 окно", "2 окна", "3 окна"
    };
    for (int i = 0; i < MaxPanels; ++i) {
        m_windowActions[i] = menuWindow->addAction(QString::fromUtf8(labels[i]));
        m_windowActions[i]->setCheckable(true);
        m_windowGroup->addAction(m_windowActions[i]);
        const int count = i + 1;
        connect(m_windowActions[i], &QAction::triggered, this, [this, count]() {
            setPanelCount(count);
        });
    }
    if (m_windowActions[1])
        m_windowActions[1]->setChecked(true); // по умолчанию 2

    // Меню «Справка»
    auto *menuHelp = menuBar()->addMenu(QString::fromUtf8("Справка"));
    menuHelp->addAction(QString::fromUtf8("О программе"), this, [this]() {
        if (m_statusLabel)
            m_statusLabel->setText(QString::fromUtf8("Программа по изучению БИБЛИИ, версия 0.3.2"));
    });

    m_centralRoot = new QWidget(this);
    setCentralWidget(m_centralRoot);

    auto *rootLayout = new QVBoxLayout(m_centralRoot);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto *toolbar = new QWidget(m_centralRoot);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);

    m_modulesButton = new QPushButton(QString::fromUtf8("Менеджер модулей…"), toolbar);
    toolbarLayout->addWidget(m_modulesButton);
    connect(m_modulesButton, &QPushButton::clicked, this, [this]() {
        BibleView *target = m_views.isEmpty() ? nullptr : m_views[0];
        for (BibleView *v : m_views) {
            if (v && v->isActiveHighlight()) {
                target = v;
                break;
            }
        }
        openModuleManagerFor(target);
    });
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    m_splitter = new QSplitter(Qt::Horizontal, m_centralRoot);
    m_splitter->setChildrenCollapsible(false);
    m_views.clear();
    m_views.reserve(MaxPanels);
    for (int i = 0; i < MaxPanels; ++i) {
        auto *view = new BibleView(m_splitter);
        view->setObjectName(QStringLiteral("panel%1").arg(i));
        m_splitter->addWidget(view);
        m_splitter->setStretchFactor(i, 0);
        wireView(view);
        m_views.append(view);
    }
    rootLayout->addWidget(m_splitter, 1);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) {
        storeSplitterSizes();
    });

    applyPanelVisibility();

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);
}

void MainWindow::setPanelCount(int count)
{
    if (count < 1) count = 1;
    if (count > MaxPanels) count = MaxPanels;
    if (count == m_panelCount && !m_views.isEmpty()) {
        // обновить галочку
        if (count >= 1 && count <= MaxPanels && m_windowActions[count - 1])
            m_windowActions[count - 1]->setChecked(true);
        return;
    }

    storeSplitterSizes();
    m_panelCount = count;
    applyPanelVisibility();

    if (count >= 1 && count <= MaxPanels && m_windowActions[count - 1])
        m_windowActions[count - 1]->setChecked(true);

    appSettings().setValue(QStringLiteral("ui/panelCount"), m_panelCount);
    appSettings().sync();

    // Равномерно раздать ширину новым панелям, если нет сохранённых размеров
    QTimer::singleShot(0, this, [this]() {
        if (!m_splitter) return;
        QList<int> sizes = m_splitter->sizes();
        int visible = 0;
        int total = 0;
        for (int i = 0; i < sizes.size() && i < m_views.size(); ++i) {
            if (m_views[i] && m_views[i]->isVisible()) {
                visible++;
                total += sizes[i];
            }
        }
        if (visible < 1) return;
        if (total < 100)
            total = m_splitter->width() > 100 ? m_splitter->width() : 800;
        const int each = total / visible;
        QList<int> neu;
        for (int i = 0; i < m_views.size(); ++i) {
            if (m_views[i] && m_views[i]->isVisible())
                neu << each;
            else
                neu << 0;
        }
        m_splitter->setSizes(neu);
        storeSplitterSizes();
    });

    if (m_statusLabel)
        m_statusLabel->setText(QString::fromUtf8("Окон: %1").arg(m_panelCount));
}

void MainWindow::applyPanelVisibility()
{
    for (int i = 0; i < m_views.size(); ++i) {
        if (!m_views[i])
            continue;
        const bool on = (i < m_panelCount);
        m_views[i]->setVisible(on);
        // Скрытые панели не должны участвовать в split
        if (QSplitter *sp = m_splitter) {
            // QSplitter скрывает через setVisible на виджете
            Q_UNUSED(sp);
        }
    }
}

void MainWindow::storeSplitterSizes()
{
    if (!m_splitter)
        return;
    const QList<int> sizes = m_splitter->sizes();
    if (sizes.isEmpty())
        return;
    QSettings &st = appSettings();
    st.setValue(QStringLiteral("ui/panelCount"), m_panelCount);
    QStringList list;
    int sum = 0;
    for (int s : sizes) {
        list << QString::number(s);
        sum += s;
    }
    if (sum < 50)
        return;
    st.setValue(QStringLiteral("ui/splitSizes"), list.join(QLatin1Char(',')));
    // совместимость со старым форматом
    if (sizes.size() >= 2) {
        st.setValue(QStringLiteral("ui/splitLeft"), sizes.at(0));
        st.setValue(QStringLiteral("ui/splitRight"), sizes.at(1));
    }
    st.sync();
}

void MainWindow::applySplitterSizes()
{
    if (!m_splitter)
        return;
    QSettings &st = appSettings();
    const QString raw = st.value(QStringLiteral("ui/splitSizes")).toString();
    if (!raw.isEmpty()) {
        const QStringList parts = raw.split(QLatin1Char(','));
        QList<int> sizes;
        for (const QString &p : parts)
            sizes << p.toInt();
        while (sizes.size() < m_views.size())
            sizes << 0;
        // Обнулить скрытые
        for (int i = 0; i < sizes.size() && i < m_views.size(); ++i) {
            if (i >= m_panelCount)
                sizes[i] = 0;
            else if (sizes[i] < 40)
                sizes[i] = 200;
        }
        m_splitter->setSizes(sizes);
        return;
    }
    // Старый формат left/right
    int left = st.value(QStringLiteral("ui/splitLeft"), -1).toInt();
    int right = st.value(QStringLiteral("ui/splitRight"), -1).toInt();
    if (left > 50 && right > 50 && m_panelCount == 2 && m_views.size() >= 2) {
        QList<int> sizes;
        sizes << left << right;
        for (int i = 2; i < m_views.size(); ++i)
            sizes << 0;
        m_splitter->setSizes(sizes);
    }
}

void MainWindow::setActivePanel(BibleView *view)
{
    for (BibleView *v : m_views) {
        if (v)
            v->setActiveHighlight(v == view);
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget *w = qobject_cast<QWidget *>(watched);
        while (w) {
            for (BibleView *v : m_views) {
                if (v && (w == v || v->isAncestorOf(w))) {
                    setActivePanel(v);
                    return QMainWindow::eventFilter(watched, event);
                }
            }
            w = w->parentWidget();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::syncOtherView(BibleView *source, int book, int chapter, int verse)
{
    if (m_syncingViews || !source || book < 1 || chapter < 1)
        return;

    Module *srcMod = source->module();
    if (!srcMod || !srcMod->isValid())
        return;

    m_syncingViews = true;
    for (BibleView *other : m_views) {
        if (!other || other == source || !other->isVisible())
            continue;
        Module *dstMod = other->module();
        if (!dstMod || !dstMod->isValid())
            continue;
        if (!isSyncableModule(srcMod) || !isSyncableModule(dstMod))
            continue;

        const int dstBook = dstMod->findBookIdMatching(srcMod, book);
        if (dstBook < 1)
            continue;
        if (other->currentBook() == dstBook && other->currentChapter() == chapter
            && other->currentVerse() == verse)
            continue;
        other->goTo(dstBook, chapter, verse > 0 ? verse : 1);
    }
    m_syncingViews = false;
}

bool MainWindow::isSyncableModule(Module *mod)
{
    if (!mod || !mod->isValid())
        return false;
    if (mod->isBibleText())
        return true;
    const QString t = mod->type().toLower();
    if (t.contains(QStringLiteral("comment")) || t.contains(QStringLiteral("коммент")))
        return true;
    if (t.contains(QStringLiteral("dict")) || t.contains(QStringLiteral("слов")))
        return true;
    if (t == QStringLiteral("book") || t == QStringLiteral("bible"))
        return true;
    return false;
}

void MainWindow::syncScrollFrom(BibleView *source, double ratio)
{
    if (m_syncingScroll || m_syncingViews || !source)
        return;
    Module *srcMod = source->module();
    if (!isSyncableModule(srcMod))
        return;

    m_syncingScroll = true;
    for (BibleView *other : m_views) {
        if (!other || other == source || !other->isVisible())
            continue;
        if (!isSyncableModule(other->module()))
            continue;
        other->setScrollRatio(ratio);
    }
    m_syncingScroll = false;
}

void MainWindow::rebindViewsFromSettings()
{
    QSettings &settings = appSettings();
    auto bind = [this, &settings](BibleView *view, const QString &keyPrefix) {
        if (!view)
            return;
        const QString name = settings.value(keyPrefix + "/module").toString();
        Module *mod = name.isEmpty() ? nullptr : m_moduleManager.moduleByName(name);
        if (view->module() && mod && view->module()->name() == mod->name()) {
            view->setModule(mod);
            int book = settings.value(keyPrefix + "/book", 0).toInt();
            int chapter = settings.value(keyPrefix + "/chapter", 1).toInt();
            if (book > 0)
                view->goTo(book, qMax(1, chapter), 1);
            return;
        }
        view->setModule(mod);
        if (mod) {
            int book = settings.value(keyPrefix + "/book", 0).toInt();
            int chapter = settings.value(keyPrefix + "/chapter", 1).toInt();
            if (book > 0)
                view->goTo(book, qMax(1, chapter), 1);
        }
    };
    for (int i = 0; i < m_views.size(); ++i)
        bind(m_views[i], panelKey(i));
}

void MainWindow::openModuleManagerFor(BibleView *view)
{
    if (!view)
        return;

    saveSettings();

    // Снимок всех панелей на случай сбоев
    struct Snap {
        QString name;
        int book = 0;
        int chapter = 1;
    };
    QVector<Snap> snaps(m_views.size());
    for (int i = 0; i < m_views.size(); ++i) {
        BibleView *v = m_views[i];
        if (v && v->module()) {
            snaps[i].name = v->module()->name();
            snaps[i].book = v->currentBook();
            snaps[i].chapter = v->currentChapter();
        }
    }

    m_pendingView = view;
    const QString currentName = (view && view->module()) ? view->module()->name() : QString();
    ModuleManagerDialog dlg(&m_moduleManager, this, currentName);
    connect(&dlg, &ModuleManagerDialog::moduleSelected, this, [this, view](Module *module) {
        if (!module || !view)
            return;
        view->setModule(module);
        QSettings &settings = appSettings();
        const int idx = indexOfView(view);
        const QString key = (idx >= 0) ? panelKey(idx) : QStringLiteral("panel/0");
        settings.setValue(key + QStringLiteral("/module"), module->name());
        settings.setValue(key + QStringLiteral("/book"), view->currentBook());
        settings.setValue(key + QStringLiteral("/chapter"), view->currentChapter());
        if (m_statusLabel)
            m_statusLabel->setText(QString::fromUtf8("Загружен модуль: %1").arg(module->name()));
    });
    connect(&dlg, &ModuleManagerDialog::moduleDeleted, this, [this](const QString &name) {
        if (name.isEmpty())
            return;
        QSettings &settings = appSettings();
        for (int i = 0; i < m_views.size(); ++i) {
            BibleView *v = m_views[i];
            if (!v || !v->module())
                continue;
            if (v->module()->name() != name)
                continue;
            v->setModule(nullptr);
            const QString key = panelKey(i);
            settings.remove(key + QStringLiteral("/module"));
            settings.remove(key + QStringLiteral("/book"));
            settings.remove(key + QStringLiteral("/chapter"));
        }
        if (m_statusLabel)
            m_statusLabel->setText(QString::fromUtf8("Модуль удалён: %1").arg(name));
    });
    dlg.exec();
    m_pendingView = nullptr;

    // Восстановить опустевшие панели
    for (int i = 0; i < m_views.size(); ++i) {
        BibleView *v = m_views[i];
        if (!v || v->module() || snaps[i].name.isEmpty())
            continue;
        if (Module *m = m_moduleManager.moduleByName(snaps[i].name)) {
            v->setModule(m);
            if (snaps[i].book > 0)
                v->goTo(snaps[i].book, qMax(1, snaps[i].chapter), 1);
        }
    }
}

void MainWindow::onModuleSelected(Module *module)
{
    Q_UNUSED(module);
}

void MainWindow::loadSettings()
{
    QSettings &settings = appSettings();
    if (settings.contains("window/geometry"))
        restoreGeometry(settings.value("window/geometry").toByteArray());
    if (settings.contains("window/state"))
        restoreState(settings.value("window/state").toByteArray());

    for (int i = 0; i < m_views.size(); ++i) {
        if (!m_views[i])
            continue;
        int fs = settings.value(panelKey(i) + QStringLiteral("/fontSize"), 13).toInt();
        // совместимость
        if (i == 0 && !settings.contains(panelKey(0) + "/fontSize"))
            fs = settings.value(QStringLiteral("panel/left/fontSize"), 13).toInt();
        if (i == 1 && !settings.contains(panelKey(1) + "/fontSize"))
            fs = settings.value(QStringLiteral("panel/right/fontSize"), 13).toInt();
        m_views[i]->setFontSize(fs);
    }
}

void MainWindow::saveSettings()
{
    QSettings &settings = appSettings();
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("ui/panelCount"), m_panelCount);
    storeSplitterSizes();

    for (int i = 0; i < m_views.size(); ++i) {
        BibleView *view = m_views[i];
        if (!view)
            continue;
        const QString keyPrefix = panelKey(i);
        settings.setValue(keyPrefix + QStringLiteral("/fontSize"), view->fontSize());
        if (!view->module())
            continue;
        settings.setValue(keyPrefix + QStringLiteral("/module"), view->module()->name());
        settings.setValue(keyPrefix + QStringLiteral("/book"), view->currentBook());
        settings.setValue(keyPrefix + QStringLiteral("/chapter"), view->currentChapter());
    }
    // дублируем в старые ключи для совместимости
    if (m_views.size() > 0 && m_views[0] && m_views[0]->module()) {
        settings.setValue(QStringLiteral("panel/left/module"), m_views[0]->module()->name());
        settings.setValue(QStringLiteral("panel/left/book"), m_views[0]->currentBook());
        settings.setValue(QStringLiteral("panel/left/chapter"), m_views[0]->currentChapter());
    }
    if (m_views.size() > 1 && m_views[1] && m_views[1]->module()) {
        settings.setValue(QStringLiteral("panel/right/module"), m_views[1]->module()->name());
        settings.setValue(QStringLiteral("panel/right/book"), m_views[1]->currentBook());
        settings.setValue(QStringLiteral("panel/right/chapter"), m_views[1]->currentChapter());
    }
    settings.sync();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(50, this, [this]() { applySplitterSizes(); });
    QTimer::singleShot(250, this, [this]() { applySplitterSizes(); });
    if (!m_uiRestored) {
        m_uiRestored = true;
        QTimer::singleShot(600, this, [this]() { applySplitterSizes(); });
    }
}

void MainWindow::hideEvent(QHideEvent *event)
{
    storeSplitterSizes();
    QMainWindow::hideEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    storeSplitterSizes();
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::onMinimize() {}
void MainWindow::onMaximize() {}
void MainWindow::onClose() { close(); }
void MainWindow::onControlsPositionChanged(int) {}
void MainWindow::applySystemTheme() {}
void MainWindow::changeEvent(QEvent *event) { QMainWindow::changeEvent(event); }
