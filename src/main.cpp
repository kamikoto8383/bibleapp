#include <QApplication>
#include <QLocale>
#include <QSqlDatabase>
#include <QMessageBox>
#include <QFontDatabase>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include "MainWindow.h"
#include <QIcon>

static void loadBundledFonts()
{
    QStringList dirs;
    dirs << QStringLiteral("/usr/share/bibleapp/fonts");
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/../share/bibleapp/fonts");
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/fonts");
    dirs << QStringLiteral("/home/workdir/artifacts/bibleapp/resources/fonts");

    for (const QString &dir : dirs) {
        QDir d(dir);
        if (!d.exists())
            continue;
        const QStringList files = d.entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")},
                                              QDir::Files);
        for (const QString &f : files) {
            const QString path = d.absoluteFilePath(f);
            const int id = QFontDatabase::addApplicationFont(path);
            if (id >= 0) {
                const QStringList families = QFontDatabase::applicationFontFamilies(id);
                qDebug() << "Font loaded:" << path << families;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ModulariuM"));
    QApplication::setApplicationName(QStringLiteral("ModulariuM"));
    QApplication::setApplicationVersion(QStringLiteral("1.6.0"));
    QApplication::setApplicationDisplayName(QString::fromUtf8("ModulariuM"));
    QLocale::setDefault(QLocale(QLocale::Russian, QLocale::Russia));

    loadBundledFonts();

    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        QMessageBox::critical(nullptr,
            QString::fromUtf8("Ошибка"),
            QString::fromUtf8(
                "Не найден драйвер SQLite.\n\n"
                "Выполните в терминале:\n"
                "sudo apt install libqt6sql6-sqlite"));
        return 1;
    }

    MainWindow window;
    window.setWindowIcon(QIcon(":/bibleapp.png"));
    window.show();
    return app.exec();
}
