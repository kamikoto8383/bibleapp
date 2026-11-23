#ifndef MODULEMANAGERDIALOG_H
#define MODULEMANAGERDIALOG_H

#include <QDialog>
#include "ModuleManager.h"
#include "Converter.h"

class QListWidget;
class QLabel;
class QPushButton;

class ModuleManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModuleManagerDialog(ModuleManager *manager, QWidget *parent = nullptr,
                                 const QString &currentModuleName = QString());

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
    ModuleManager *m_manager = nullptr;
    Converter m_converter;
    QString m_currentModuleName;

    QListWidget *m_list = nullptr;
    QLabel *m_infoLabel = nullptr;
    QPushButton *m_convertBtn = nullptr;
    QPushButton *m_myBibleBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
};

#endif // MODULEMANAGERDIALOG_H
