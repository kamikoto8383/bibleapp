#ifndef MODULEMANAGER_H
#define MODULEMANAGER_H

#include <QObject>
#include <QVector>
#include <QString>
#include "Module.h"

class ModuleManager : public QObject
{
    Q_OBJECT
public:
    explicit ModuleManager(QObject *parent = nullptr);

    QString modulesPath() const;
    QVector<Module*> modules() const { return m_modules; }

    bool loadModules();
    Module* moduleByName(const QString &name) const;
    Module* moduleAt(int index) const;

    bool ensureTestModule();
    bool importConvertedModule(const QString &sourcePath, const QString &name);
    bool removeModule(int index);

signals:
    void modulesChanged();

private:
    QVector<Module*> m_modules;
    QString m_modulesPath;
};

#endif // MODULEMANAGER_H