#include <QCoreApplication>
#include <QFile>
#include "ModuleManager.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QRegularExpression>
#include <QFileInfo>
#include <QHash>

ModuleManager::ModuleManager(QObject *parent)
    : QObject(parent)
{
    m_modulesPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/modules";
    QDir().mkpath(m_modulesPath);
}

QString ModuleManager::modulesPath() const
{
    return m_modulesPath;
}

bool ModuleManager::loadModules()
{
    // Сохраняем уже открытые Module* (по пути), чтобы панели не теряли текст
    QHash<QString, Module*> byPath;
    for (Module *m : m_modules) {
        if (m)
            byPath.insert(QDir(m->path()).absolutePath(), m);
    }

    QVector<Module*> next;
    QDir dir(m_modulesPath);
    const auto folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &folder : folders) {
        QString path = QDir(dir.absoluteFilePath(folder)).absolutePath();
        if (byPath.contains(path)) {
            next.append(byPath.take(path));
            continue;
        }
        auto *mod = new Module(path);
        if (mod->isValid())
            next.append(mod);
        else
            delete mod;
    }

    // Удалить только те модули, которых больше нет на диске
    for (Module *orphaned : byPath)
        delete orphaned;

    m_modules = next;
    emit modulesChanged();
    return true;
}

Module* ModuleManager::moduleByName(const QString &name) const
{
    for (auto *m : m_modules) {
        if (m->name() == name)
            return m;
    }
    return nullptr;
}

Module* ModuleManager::moduleAt(int index) const
{
    if (index >= 0 && index < m_modules.size())
        return m_modules.at(index);
    return nullptr;
}

bool ModuleManager::ensureTestModule()
{
    bool ok = true;

    // Полный синодальный перевод из /usr/share/bibleapp/modules/synodal
    QString synPath = m_modulesPath + "/synodal";
    if (!(QDir(synPath).exists() && QFile::exists(synPath + "/data.sqlite"))) {
        QStringList candidates;
        candidates << "/usr/share/bibleapp/modules/synodal";
        candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/modules/synodal";
        candidates << QCoreApplication::applicationDirPath() + "/modules/synodal";
        for (const QString &src : candidates) {
            if (QDir(src).exists() && QFile::exists(src + "/data.sqlite")) {
                QDir().mkpath(synPath);
                QFile::copy(src + "/data.sqlite", synPath + "/data.sqlite");
                QFile::copy(src + "/module.json", synPath + "/module.json");
                break;
            }
        }
    }

    QString testPath = m_modulesPath + "/synodal_test";
    if (!(QDir(testPath).exists() && QFile::exists(testPath + "/module.json"))) {
        if (!Module::createTestModule(testPath))
            ok = false;
    }

    // Полный подстрочник 66 книг
    QString ilPath = m_modulesPath + "/interlinear";
    if (!(QDir(ilPath).exists() && QFile::exists(ilPath + "/data.sqlite"))) {
        QStringList candidates;
        candidates << "/usr/share/bibleapp/modules/interlinear";
        candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/modules/interlinear";
        candidates << QCoreApplication::applicationDirPath() + "/modules/interlinear";
        for (const QString &src : candidates) {
            if (QDir(src).exists() && QFile::exists(src + "/data.sqlite")) {
                QDir().mkpath(ilPath);
                QFile::copy(src + "/data.sqlite", ilPath + "/data.sqlite");
                QFile::copy(src + "/module.json", ilPath + "/module.json");
                break;
            }
        }
    }

    loadModules();
    return ok;
}

bool ModuleManager::importConvertedModule(const QString &sourcePath, const QString &name)
{
    QString safeName = name;
    safeName.replace(QRegularExpression("[^\\w\\-]"), "_");
    if (safeName.isEmpty())
        safeName = "imported_module";

    QString target = m_modulesPath + "/" + safeName;

    // Простое копирование уже сконвертированной папки
    QDir targetDir(target);
    if (targetDir.exists())
        targetDir.removeRecursively();

    if (!QDir().mkpath(target))
        return false;

    QDir src(sourcePath);
    for (const QFileInfo &fi : src.entryInfoList(QDir::Files)) {
        QFile::copy(fi.absoluteFilePath(), target + "/" + fi.fileName());
    }

    loadModules();
    return true;
}

bool ModuleManager::removeModule(int index)
{
    if (index < 0 || index >= m_modules.size())
        return false;

    Module *mod = m_modules.at(index);
    QString path = mod->path();

    // Не даём удалить, если путь не внутри нашей папки модулей
    if (!path.startsWith(m_modulesPath))
        return false;

    delete mod;
    m_modules.removeAt(index);

    QDir dir(path);
    if (dir.exists())
        dir.removeRecursively();

    emit modulesChanged();
    return true;
}
