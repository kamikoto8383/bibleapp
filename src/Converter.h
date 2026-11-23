#ifndef CONVERTER_H
#define CONVERTER_H

#include <QString>
#include <QObject>
#include <QStringList>

struct ConversionResult {
    bool success = false;
    QString message;
    QString outputPath;
    QString detectedName;
    QString detectedType;
    // Анализ / аудит
    QString analysisReport;
    QString auditReport;
    int booksExpected = 0;
    int booksConverted = 0;
    int versesConverted = 0;
    int auditWarnings = 0;
    int auditErrors = 0;
};

class Converter : public QObject
{
    Q_OBJECT
public:
    explicit Converter(QObject *parent = nullptr);

    // Глубокий анализ папки модуля BibleQuote (до конвертации)
    ConversionResult analyze(const QString &bibleQuoteModulePath) const;

    // Конвертация + обязательный пост-аудит качества
    ConversionResult convert(const QString &bibleQuoteModulePath, const QString &outputDir);

    /** Импорт модуля MyBible (.SQLite3): словарь / симфония / commentary */
    ConversionResult convertMyBible(const QString &sqliteFilePath, const QString &outputDir);

signals:
    void progress(int current, int total, const QString &status);

private:
    QString runPostConversionAudit(const QString &moduleDir, int expectedBooks,
                                   const QStringList &expectedBookNames) const;
};

#endif // CONVERTER_H
