#ifndef MODULE_H
#define MODULE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QSqlDatabase>

struct Verse {
    int book = 0;
    int chapter = 0;
    int verse = 0;
    QString text;
};

struct BookInfo {
    int id = 0;
    QString shortName;
    QString fullName;
    int chapters = 0;
};

struct SearchHit {
    int book = 0;
    int chapter = 0;
    int verse = 0;
    QString bookName;
    QString snippet;
};

struct CrossRef {
    int refBook = 0;
    int refChapter = 0;
    int refVerseStart = 0;
    int refVerseEnd = 0;
    QString note;
    QString display; // «Ин 1:1–3»
};

class Module
{
public:
    Module() = default;
    explicit Module(const QString &path);

    bool isValid() const { return m_valid; }
    /** true для библейского текста / подстрочника; false для комментариев и словарей */
    bool isBibleText() const;
    /** словарь / симфония / конкорданс */
    bool isDictionary() const;
    bool isCommentary() const;
    QString path() const { return m_path; }
    QString name() const { return m_name; }
    QString description() const { return m_description; }
    QString type() const { return m_type; }
    QString language() const { return m_language; }

    QVector<BookInfo> books() const { return m_books; }
    QStringList bookNames() const;

    QString getVerseText(int book, int chapter, int verse) const;
    QString getChapterText(int book, int chapter, const QString &highlightQuery = QString()) const;
    int chapterCount(int book) const;
    int verseCount(int book, int chapter) const;
    /** Реально существующие номера глав (может включать 0 — «Введение»). */
    QVector<int> chapterNumbers(int book) const;
    /** Реально существующие номера стихов (без «дыр»). */
    QVector<int> verseNumbers(int book, int chapter) const;

    QVector<SearchHit> search(const QString &query, int limit = 50) const;

    static bool createTestModule(const QString &targetDir);
    static bool createInterlinearModule(const QString &targetDir);
    static QString lookupThayer(const QString &greekWord);
    static QString lookupVerseStrongs(int book, int chapter, int verse);
    static QString lookupStrongGloss(const QString &strongNum);
    /** Полная статья Стронга (HTML, с русским кратким значением). */
    static QString lookupStrongArticle(const QString &strongNum);
    /** Подобрать номер Стронга по форме слова / русской глоссе */
    static QString lookupStrongByWord(const QString &orig, const QString &gloss);
    static QString lookupGloss(const QString &greekWord);
    static QVector<CrossRef> lookupCrossRefs(int book, int chapter, int verse);
    static QString bookAbbr(int bookId);
    static QString formatRef(int book, int chapter, int verseStart, int verseEnd = 0);
    QString bookNameById(int id) const;
    QString bookShortNameById(int id) const;
    int findBookIdMatching(const Module *other, int otherBookId) const;

private:
    bool load();
    bool openDatabase() const;

    QString m_path;
    QString m_name;
    QString m_description;
    QString m_type = "bible";
    QString m_language = "ru";
    bool m_valid = false;
    QVector<BookInfo> m_books;

    mutable QSqlDatabase m_db;
    QString m_connectionName;
};

#endif
