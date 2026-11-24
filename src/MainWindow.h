#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSettings>
#include <QVector>
#include "ModuleManager.h"

class QLabel;
class QPushButton;
class QSplitter;
class QAction;
class QActionGroup;
class BibleView;
class Module;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    static const int MaxPanels = 3;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onMinimize();
    void onMaximize();
    void onClose();
    void onControlsPositionChanged(int index);
    void openModuleManagerFor(BibleView *view, bool managementMode = true);
    void rebindViewsFromSettings();
    void onModuleSelected(Module *module);
    void applySystemTheme();
    void syncOtherView(BibleView *source, int book, int chapter, int verse);
    void syncScrollFrom(BibleView *source, double ratio);
    void setActivePanel(BibleView *view);
    void setPanelCount(int count);
    static bool isSyncableModule(Module *mod);

private:
    void setupUi();
    void loadSettings();
    void saveSettings();
    void wireView(BibleView *view);
    void applyPanelVisibility();
    QString panelKey(int index) const;
    int indexOfView(BibleView *view) const;
    void storeSplitterSizes();
    void applySplitterSizes();

    QWidget *m_centralRoot = nullptr;
    QSplitter *m_splitter = nullptr;
    QVector<BibleView *> m_views;
    int m_panelCount = 2;

    QLabel *m_statusLabel = nullptr;
    QPushButton *m_modulesButton = nullptr;
    QActionGroup *m_windowGroup = nullptr;
    QAction *m_windowActions[MaxPanels] = {};

    ModuleManager m_moduleManager;
    BibleView *m_pendingView = nullptr;
    bool m_uiRestored = false;
    bool m_syncingViews = false;
    bool m_syncingScroll = false;
    bool m_scrollSyncEnabled = true;
    QAction *m_scrollSyncAction = nullptr;
};

#endif
