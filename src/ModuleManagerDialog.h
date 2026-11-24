#ifndef MODULEMANAGERDIALOG_H
#define MODULEMANAGERDIALOG_H

#include <QDialog>
#include "ModuleManager.h"
#include "Converter.h"

class QListWidget;
class QLabel;
class QPushButton;
class QButtonGroup;

class ModuleManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModuleManagerDialog(ModuleManager *manager, QWidget *parent = nullptr,
                                 const QString &currentModuleName = QString(),
                                 const QString &windowTitle = QString(),
                                 bool managementMode = true);

signals:
    void moduleSelected(Module *module);
    void moduleDeleted(const QString &moduleName);

private slots:
    void refreshList();
    void onConvert();
    void onConvertMyBible();
    void onOpenSelected();
    void onSelectionChanged();
    void onDeleteSelected();

private:
    enum Category { CatAll = 0, CatBible = 1, CatDict = 2, CatComment = 3, CatExposition = 4, CatInterlinear = 5 };
    static int classifyModule(Module *m);

    ModuleManager *m_manager = nullptr;
    Converter m_converter;
    QString m_currentModuleName;
    bool m_managementMode = true;
    int m_category = CatBible;

    QListWidget *m_list = nullptr;
    QLabel *m_infoLabel = nullptr;
    QPushButton *m_convertBtn = nullptr;
    QPushButton *m_myBibleBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QButtonGroup *m_categoryGroup = nullptr;
};

#endif // MODULEMANAGERDIALOG_H
