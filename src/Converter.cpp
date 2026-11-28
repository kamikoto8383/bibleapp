#include "Converter.h"
#include <QMap>
#include <QHash>
#include <QVector>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringDecoder>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <QDebug>

#include <algorithm>

// ============================================================================
//  Вспомогательные структуры
// ============================================================================

struct IniBook {
    QString pathName;
    QString fullName;
    QString shortName;
    int chapterQty = 1;
};

struct IniData {
    QMap<QString, QString> values;   // ключи в нижнем регистре
    QVector<IniBook> books;

    QString get(const QString &key, const QString &def = QString()) const
    {
        return values.value(key.toLower(), def);
    }

    bool flag(const QString &key, bool def = false) const
    {
        const QString v = get(key).trimmed();
        if (v.isEmpty())
            return def;
        return v.compare("Y", Qt::CaseInsensitive) == 0
            || v.compare("YES", Qt::CaseInsensitive) == 0
            || v.compare("TRUE", Qt::CaseInsensitive) == 0
            || v == "1";
    }
};

struct ChapterMark {
    int num = 0;
    int pos = 0;   // начало маркера
    int end = 0;   // конец маркера (отсюда начинается текст главы)
};

struct VerseMark {
    int num = 0;
    int endNum = 0;  // для диапазонов вида «1-17»
    int markStart = 0;
    int bodyStart = 0;
};

Converter::Converter(QObject *parent)
    : QObject(parent)
{
}

// Префикс номеров Стронга для текущей книги (H — ВЗ, G — НЗ).
// Конвертер работает в одном потоке, поэтому статической переменной достаточно.
static QString g_strongPrefix;

// ============================================================================
//  Кодировки и INI
// ============================================================================

static QString decodeBytesWithHint(const QByteArray &data, const QString &hint = QString())
{
    QByteArray raw = data;
    if (raw.startsWith("\xEF\xBB\xBF"))
        raw = raw.mid(3);

    if (raw.startsWith("\xFF\xFE") || raw.startsWith("\xFE\xFF")) {
        QStringDecoder dec(QStringDecoder::Utf16);
        QString s = dec(raw);
        s.remove(QChar(0xFEFF));
        return s;
    }

    const QString h = hint.trimmed().toLower();

    auto tryDecoder = [&](const char *name) -> QString {
        QStringDecoder dec(name);
        if (!dec.isValid())
            return QString();
        QString s = dec(raw);
        if (!s.contains(QChar::ReplacementCharacter))
            return s;
        return QString();
    };

    if (h.contains("1251") || h.contains("cp1251") || h.contains("windows-1251")) {
        QString s = tryDecoder("Windows-1251");
        if (!s.isEmpty())
            return s;
    }
    if (h.contains("koi8")) {
        QString s = tryDecoder("KOI8-R");
        if (!s.isEmpty())
            return s;
    }
    if (h.contains("utf-8") || h.contains("utf8")) {
        QString s = QString::fromUtf8(raw);
        if (!s.contains(QChar::ReplacementCharacter)) {
            s.remove(QChar(0xFEFF));
            return s;
        }
    }

    QString utf8 = QString::fromUtf8(raw);
    const int utf8Bad = utf8.count(QChar::ReplacementCharacter);
    if (utf8Bad == 0) {
        utf8.remove(QChar(0xFEFF));
        return utf8;
    }

    QString cp1251 = tryDecoder("Windows-1251");
    if (!cp1251.isEmpty()) {
        const int bad = cp1251.count(QChar::ReplacementCharacter);
        if (bad < utf8Bad) {
            cp1251.remove(QChar(0xFEFF));
            return cp1251;
        }
    }

    QString koi8 = tryDecoder("KOI8-R");
    if (!koi8.isEmpty()) {
        const int bad = koi8.count(QChar::ReplacementCharacter);
        if (bad < utf8Bad) {
            koi8.remove(QChar(0xFEFF));
            return koi8;
        }
    }

    utf8.remove(QChar(0xFEFF));
    return utf8;
}

static QString findIniFile(const QString &path)
{
    QDir dir(path);
    if (!dir.exists())
        return QString();

    const QStringList files = dir.entryList(QDir::Files);
    // BibleQt.ini в любом регистре
    for (const QString &fn : files) {
        if (fn.compare(QStringLiteral("bibleqt.ini"), Qt::CaseInsensitive) == 0)
            return dir.absoluteFilePath(fn);
    }
    // Любой *.ini
    for (const QString &fn : files) {
        if (fn.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive))
            return dir.absoluteFilePath(fn);
    }
    return QString();
}

static bool hasDictionaryIdxFiles(const QString &path)
{
    QDir dir(path);
    return !dir.entryList(QStringList() << QStringLiteral("*.idx")
                                        << QStringLiteral("*.IDX"),
                          QDir::Files).isEmpty();
}

static QString locateBookFile(const QString &moduleRoot, const QString &pathName);

static IniData parseIni(const QString &iniPath)
{
    IniData ini;
    QFile f(iniPath);
    if (!f.open(QIODevice::ReadOnly))
        return ini;

    QString content = decodeBytesWithHint(f.readAll());
    content.remove(QChar(0xFEFF));

    IniBook cur;
    bool hasCur = false;

    const QStringList lines = content.split(QRegularExpression(QStringLiteral("[\r\n]+")));
    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        // комментарии ; # //  — но не трогаем строки с '='
        if ((line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))
             || line.startsWith(QStringLiteral("//")))
            && !line.contains(QLatin1Char('=')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
            continue;

        // убрать хвостовой комментарий после значения: PathName=1Ti.htm ; Timothy
        int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            // иногда бывает "PathName book.htm" без '='
            const QString low = line.toLower();
            if (low.startsWith(QStringLiteral("pathname ")) || low.startsWith(QStringLiteral("pathname\t"))) {
                eq = 8;
                line = QStringLiteral("pathname=") + line.mid(9).trimmed();
                eq = line.indexOf(QLatin1Char('='));
            } else {
                continue;
            }
        }

        QString key = line.left(eq).trimmed().toLower();
        QString value = line.mid(eq + 1).trimmed();
        // срезать inline-комментарий
        int semi = value.indexOf(QLatin1Char(';'));
        if (semi > 0)
            value = value.left(semi).trimmed();
        // PathName1 / FullName2 → pathname / fullname
        while (!key.isEmpty() && key.back().isDigit())
            key.chop(1);

        const bool isPath = (key == QLatin1String("pathname")
                             || key == QLatin1String("filename")
                             || key == QLatin1String("filepath")
                             || key == QLatin1String("file")
                             || key == QLatin1String("path"));
        if (isPath) {
            if (hasCur)
                ini.books.append(cur);
            cur = IniBook();
            cur.pathName = value;
            hasCur = true;
            continue;
        }

        if (hasCur) {
            if (key == QLatin1String("fullname") || key == QLatin1String("bookname")
                || key == QLatin1String("name") || key == QLatin1String("full_name")
                || key == QLatin1String("title"))
                cur.fullName = value;
            else if (key == QLatin1String("shortname") || key == QLatin1String("short_name")
                     || key == QLatin1String("abbr") || key == QLatin1String("abbreviation"))
                cur.shortName = value;
            else if (key == QLatin1String("chapterqty") || key == QLatin1String("chapters")
                     || key == QLatin1String("chaptercount") || key == QLatin1String("chapter_qty"))
                cur.chapterQty = value.toInt();
            else if (key == QLatin1String("chapterqtyps") && cur.chapterQty <= 0)
                cur.chapterQty = value.toInt();
        }

        ini.values.insert(key, value);
    }

    if (hasCur)
        ini.books.append(cur);

    return ini;
}

static QStringList standardBibleBookFiles()
{
    return QStringList{
        QStringLiteral("Gen"), QStringLiteral("Ex"), QStringLiteral("Lev"), QStringLiteral("Num"),
        QStringLiteral("Deu"), QStringLiteral("Josh"), QStringLiteral("Judg"), QStringLiteral("Ruth"),
        QStringLiteral("1Sam"), QStringLiteral("2Sam"), QStringLiteral("1Kin"), QStringLiteral("2Kin"),
        QStringLiteral("1Chr"), QStringLiteral("2Chr"), QStringLiteral("Ezra"), QStringLiteral("Neh"),
        QStringLiteral("Esth"), QStringLiteral("Job"), QStringLiteral("Ps"), QStringLiteral("Prov"),
        QStringLiteral("Eccl"), QStringLiteral("Song"), QStringLiteral("Isa"), QStringLiteral("Jer"),
        QStringLiteral("Lam"), QStringLiteral("Ezek"), QStringLiteral("Dan"), QStringLiteral("Hos"),
        QStringLiteral("Joel"), QStringLiteral("Amos"), QStringLiteral("Obad"), QStringLiteral("Jonah"),
        QStringLiteral("Mic"), QStringLiteral("Nah"), QStringLiteral("Hab"), QStringLiteral("Zeph"),
        QStringLiteral("Hag"), QStringLiteral("Zech"), QStringLiteral("Mal"),
        QStringLiteral("Matt"), QStringLiteral("Mark"), QStringLiteral("Luke"), QStringLiteral("John"),
        QStringLiteral("Acts"), QStringLiteral("Rom"), QStringLiteral("1Cor"), QStringLiteral("2Cor"),
        QStringLiteral("Gal"), QStringLiteral("Eph"), QStringLiteral("Phil"), QStringLiteral("Col"),
        QStringLiteral("1Thes"), QStringLiteral("2Thes"), QStringLiteral("1Tim"), QStringLiteral("2Tim"),
        QStringLiteral("Titus"), QStringLiteral("Phlm"), QStringLiteral("Heb"), QStringLiteral("James"),
        QStringLiteral("1Pet"), QStringLiteral("2Pet"), QStringLiteral("1Jn"), QStringLiteral("2Jn"),
        QStringLiteral("3Jn"), QStringLiteral("Jude"), QStringLiteral("Rev")
    };
}

static QStringList standardBibleBookNamesRu()
{
    return QStringList{
        QString::fromUtf8("Бытие"), QString::fromUtf8("Исход"), QString::fromUtf8("Левит"),
        QString::fromUtf8("Числа"), QString::fromUtf8("Второзаконие"), QString::fromUtf8("Иисус Навин"),
        QString::fromUtf8("Судей"), QString::fromUtf8("Руфь"), QString::fromUtf8("1 Царств"),
        QString::fromUtf8("2 Царств"), QString::fromUtf8("3 Царств"), QString::fromUtf8("4 Царств"),
        QString::fromUtf8("1 Паралипоменон"), QString::fromUtf8("2 Паралипоменон"),
        QString::fromUtf8("Ездра"), QString::fromUtf8("Неемия"), QString::fromUtf8("Есфирь"),
        QString::fromUtf8("Иов"), QString::fromUtf8("Псалтирь"), QString::fromUtf8("Притчи"),
        QString::fromUtf8("Екклесиаст"), QString::fromUtf8("Песнь Песней"), QString::fromUtf8("Исаия"),
        QString::fromUtf8("Иеремия"), QString::fromUtf8("Плач Иеремии"), QString::fromUtf8("Иезекииль"),
        QString::fromUtf8("Даниил"), QString::fromUtf8("Осия"), QString::fromUtf8("Иоиль"),
        QString::fromUtf8("Амос"), QString::fromUtf8("Авдий"), QString::fromUtf8("Иона"),
        QString::fromUtf8("Михей"), QString::fromUtf8("Наум"), QString::fromUtf8("Аввакум"),
        QString::fromUtf8("Софония"), QString::fromUtf8("Аггей"), QString::fromUtf8("Захария"),
        QString::fromUtf8("Малахия"),
        QString::fromUtf8("От Матфея"), QString::fromUtf8("От Марка"), QString::fromUtf8("От Луки"),
        QString::fromUtf8("От Иоанна"), QString::fromUtf8("Деяния"), QString::fromUtf8("К Римлянам"),
        QString::fromUtf8("1 Коринфянам"), QString::fromUtf8("2 Коринфянам"),
        QString::fromUtf8("К Галатам"), QString::fromUtf8("К Ефесянам"),
        QString::fromUtf8("К Филиппийцам"), QString::fromUtf8("К Колоссянам"),
        QString::fromUtf8("1 Фессалоникийцам"), QString::fromUtf8("2 Фессалоникийцам"),
        QString::fromUtf8("1 Тимофею"), QString::fromUtf8("2 Тимофею"),
        QString::fromUtf8("К Титу"), QString::fromUtf8("К Филимону"),
        QString::fromUtf8("К Евреям"), QString::fromUtf8("Иакова"),
        QString::fromUtf8("1 Петра"), QString::fromUtf8("2 Петра"),
        QString::fromUtf8("1 Иоанна"), QString::fromUtf8("2 Иоанна"),
        QString::fromUtf8("3 Иоанна"), QString::fromUtf8("Иуды"),
        QString::fromUtf8("Откровение")
    };
}

static int guessBookIndexFromStem(QString stemLow)
{
    stemLow = stemLow.toLower().trimmed();
    stemLow.replace(QString::fromUtf8("ё"), QString::fromUtf8("е"));
    // убрать префиксы 54_ 54- 54.
    stemLow.replace(QRegularExpression(QStringLiteral("^\\d+[._\\-\\s]+")), QString());
    stemLow.replace(QRegularExpression(QStringLiteral("[\\s_\\-]+")), QString());

    struct Pair { const char *key; int idx; };
    static const Pair table[] = {
        {"gen",0},{"ge",0},{"бытие",0},{"быт",0},
        {"ex",1},{"exo",1},{"exod",1},{"исход",1},{"исх",1},
        {"lev",2},{"le",2},{"левит",2},{"лев",2},
        {"num",3},{"nu",3},{"числа",3},{"чис",3},
        {"deu",4},{"dt",4},{"deut",4},{"второзаконие",4},{"втор",4},
        {"josh",5},{"jos",5},{"навин",5},{"исуснавин",5},{"нав",5},
        {"judg",6},{"jdg",6},{"судей",6},{"суд",6},
        {"ruth",7},{"ru",7},{"руфь",7},{"руф",7},
        {"1sam",8},{"1sa",8},{"1царств",8},{"1цар",8},{"1ц",8},
        {"2sam",9},{"2sa",9},{"2царств",9},{"2цар",9},{"2ц",9},
        {"1kin",10},{"1ki",10},{"1kings",10},{"3царств",10},{"3цар",10},{"3ц",10},
        {"2kin",11},{"2ki",11},{"2kings",11},{"4царств",11},{"4цар",11},{"4ц",11},
        {"1chr",12},{"1ch",12},{"1пар",12},{"1паралипоменон",12},
        {"2chr",13},{"2ch",13},{"2пар",13},{"2паралипоменон",13},
        {"ezra",14},{"ezr",14},{"ездра",14},{"езд",14},
        {"neh",15},{"ne",15},{"неемия",15},{"неем",15},
        {"esth",16},{"es",16},{"est",16},{"есфирь",16},{"есф",16},
        {"job",17},{"иов",17},
        {"ps",18},{"psa",18},{"psalm",18},{"псалтирь",18},{"псалом",18},{"пс",18},
        {"prov",19},{"pr",19},{"притчи",19},{"прит",19},
        {"eccl",20},{"ecc",20},{"екклесиаст",20},{"еккл",20},
        {"song",21},{"so",21},{"ss",21},{"песнь",21},{"песн",21},{"пп",21},
        {"isa",22},{"is",22},{"исаия",22},{"ис",22},
        {"jer",23},{"иеремия",23},{"иер",23},
        {"lam",24},{"la",24},{"плач",24},{"плачтеремии",24},
        {"ezek",25},{"eze",25},{"иезекииль",25},{"иез",25},
        {"dan",26},{"da",26},{"даниил",26},{"дан",26},
        {"hos",27},{"ho",27},{"осия",27},{"ос",27},
        {"joel",28},{"joe",28},{"иоиль",28},{"иоил",28},
        {"amos",29},{"am",29},{"амос",29},{"ам",29},
        {"obad",30},{"ob",30},{"авдий",30},{"авд",30},
        {"jonah",31},{"jon",31},{"иона",31},
        {"mic",32},{"михей",32},{"мих",32},
        {"nah",33},{"na",33},{"наум",33},
        {"hab",34},{"аввакум",34},{"авв",34},
        {"zeph",35},{"zep",35},{"софония",35},{"соф",35},
        {"hag",36},{"аггей",36},{"агг",36},
        {"zech",37},{"zec",37},{"захария",37},{"зах",37},
        {"mal",38},{"малахия",38},{"мал",38},
        {"matt",39},{"mat",39},{"mt",39},{"матфея",39},{"матф",39},{"мф",39},
        {"mark",40},{"mrk",40},{"mk",40},{"марка",40},{"мар",40},{"мк",40},
        {"luke",41},{"luk",41},{"lk",41},{"луки",41},{"лук",41},{"лк",41},
        {"john",42},{"joh",42},{"jn",42},{"иоанна",42},{"ин",42},
        {"acts",43},{"act",43},{"ac",43},{"деяния",43},{"деян",43},{"деян",43},
        {"rom",44},{"ro",44},{"римлянам",44},{"рим",44},{"римл",44},
        {"1cor",45},{"1co",45},{"1коринфянам",45},{"1кор",45},
        {"2cor",46},{"2co",46},{"2коринфянам",46},{"2кор",46},
        {"gal",47},{"ga",47},{"галатам",47},{"гал",47},
        {"eph",48},{"ефесянам",48},{"еф",48},
        {"phil",49},{"php",49},{"php",49},{"филиппийцам",49},{"флп",49},{"фил",49},
        {"col",50},{"колоссянам",50},{"кол",50},
        {"1thes",51},{"1th",51},{"1thess",51},{"1фессалоникийцам",51},{"1фес",51},{"1фесс",51},
        {"2thes",52},{"2th",52},{"2thess",52},{"2фессалоникийцам",52},{"2фес",52},{"2фесс",52},
        {"1tim",53},{"1ti",53},{"1timothy",53},{"1тимофею",53},{"1тим",53},{"1тимофей",53},
        {"2tim",54},{"2ti",54},{"2timothy",54},{"2тимофею",54},{"2тим",54},{"2тимофей",54},
        {"titus",55},{"tit",55},{"tt",55},{"титу",55},{"тит",55},
        {"phlm",56},{"phm",56},{"philemon",56},{"филимону",56},{"флм",56},{"филимон",56},
        {"heb",57},{"hebrews",57},{"евреям",57},{"евр",57},
        {"james",58},{"jas",58},{"jam",58},{"иакова",58},{"иак",58},{"як",58},
        {"1pet",59},{"1pe",59},{"1peter",59},{"1петра",59},{"1пет",59},
        {"2pet",60},{"2pe",60},{"2peter",60},{"2петра",60},{"2пет",60},
        {"1jn",61},{"1jo",61},{"1john",61},{"1иоанна",61},{"1ин",61},{"1иоан",61},
        {"2jn",62},{"2jo",62},{"2john",62},{"2иоанна",62},{"2ин",62},
        {"3jn",63},{"3jo",63},{"3john",63},{"3иоанна",63},{"3ин",63},
        {"jude",64},{"jud",64},{"иуды",64},{"иуд",64},
        {"rev",65},{"re",65},{"revelation",65},{"apoc",65},{"откровение",65},{"откр",65},{"апок",65},
        {nullptr, -1}
    };
    for (int i = 0; table[i].key; ++i) {
        if (stemLow == QLatin1String(table[i].key)
            || stemLow.startsWith(QLatin1String(table[i].key)))
            return table[i].idx;
    }
    // содержит ключ
    for (int i = 0; table[i].key; ++i) {
        if (stemLow.contains(QLatin1String(table[i].key)))
            return table[i].idx;
    }
    return -1;
}

/** Если в INI оборвался список книг — добираем ВСЕ HTML-файлы модуля */
static void completeBooksFromDirectory(const QString &moduleRoot, IniData &ini)
{
    if (moduleRoot.isEmpty())
        return;

    QSet<QString> usedFiles; // lowercase absolute paths and stems already in INI
    QSet<QString> usedStems;
    for (const IniBook &b : ini.books) {
        QString rel = b.pathName;
        rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QFileInfo fi(rel);
        usedStems.insert(fi.completeBaseName().toLower());
        usedStems.insert(fi.fileName().toLower());
        const QString abs = locateBookFile(moduleRoot, b.pathName);
        if (!abs.isEmpty())
            usedFiles.insert(QFileInfo(abs).absoluteFilePath().toLower());
    }

    const QStringList names = standardBibleBookNamesRu();
    const QStringList stems = standardBibleBookFiles();

    struct Found { int index; QString absPath; QString stem; };
    QVector<Found> found;

    QDirIterator it(moduleRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi(p);
        const QString ext = fi.suffix().toLower();
        if (ext != QLatin1String("htm") && ext != QLatin1String("html")
            && ext != QLatin1String("txt") && ext != QLatin1String("xhtml"))
            continue;

        const QString absLow = fi.absoluteFilePath().toLower();
        if (usedFiles.contains(absLow))
            continue;

        const QString stem = fi.completeBaseName();
        const QString stemLow = stem.toLower();
        if (usedStems.contains(stemLow) || usedStems.contains(fi.fileName().toLower()))
            continue;

        // служебные
        if (stemLow.contains(QStringLiteral("copyright")) || stemLow == QStringLiteral("index")
            || stemLow == QStringLiteral("contents") || stemLow.startsWith(QStringLiteral("bibleqt"))
            || stemLow.contains(QStringLiteral("comment")) || stemLow == QStringLiteral("readme")
            || stemLow.contains(QStringLiteral("content")))
            continue;

        int idx = guessBookIndexFromStem(stemLow);
        found.append({idx, fi.absoluteFilePath(), stem});
    }

    if (found.isEmpty())
        return;

    // Сначала известные по индексу, потом неизвестные (index < 0)
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.index < 0 && b.index < 0)
            return a.stem.toLower() < b.stem.toLower();
        if (a.index < 0)
            return false;
        if (b.index < 0)
            return true;
        return a.index < b.index;
    });

    QSet<int> takenIndex;
    for (const IniBook &b : ini.books) {
        int g = guessBookIndexFromStem(QFileInfo(b.pathName).completeBaseName());
        if (g < 0 && !b.fullName.isEmpty())
            g = guessBookIndexFromStem(b.fullName);
        if (g >= 0)
            takenIndex.insert(g);
    }

    int added = 0;
    for (const Found &fnd : found) {
        if (fnd.index >= 0 && takenIndex.contains(fnd.index))
            continue;

        IniBook b;
        QString rel = fnd.absPath;
        const QString rootAbs = QDir(moduleRoot).absolutePath();
        if (rel.startsWith(rootAbs)) {
            rel = rel.mid(rootAbs.size());
            while (rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\')))
                rel = rel.mid(1);
        }
        b.pathName = rel;
        if (fnd.index >= 0 && fnd.index < names.size()) {
            b.fullName = names.at(fnd.index);
            b.shortName = stems.value(fnd.index, fnd.stem);
            takenIndex.insert(fnd.index);
        } else {
            b.fullName = fnd.stem;
            b.shortName = fnd.stem;
        }
        b.chapterQty = 1;
        ini.books.append(b);
        usedStems.insert(fnd.stem.toLower());
        ++added;
    }

    if (added > 0)
        qInfo("BibleQuote: добрано книг из папки модуля: %d (всего теперь %d)",
              added, ini.books.size());
}

static QString detectModuleType(const IniData &ini)
{
    const QString name = (ini.get(QStringLiteral("modulename")) + QLatin1Char(' ')
                          + ini.get(QStringLiteral("biblename"))).toLower();

    if (name.contains(QString::fromUtf8("словар"))
        || name.contains(QStringLiteral("dict"))
        || name.contains(QString::fromUtf8("энциклоп"))
        || name.contains(QString::fromUtf8("симфон"))
        || name.contains(QString::fromUtf8("справоч")))
        return QStringLiteral("dictionary");

    const bool bibleFlag = ini.flag(QStringLiteral("bible"), true);
    if (!bibleFlag)
        return QStringLiteral("commentary");

    if (name.contains(QString::fromUtf8("толкован"))
        || name.contains(QString::fromUtf8("коммент"))
        || name.contains(QStringLiteral("commentary"))
        || name.contains(QString::fromUtf8("проповед"))
        || name.contains(QString::fromUtf8("экзегет"))
        || name.contains(QString::fromUtf8("спурджен"))
        || name.contains(QString::fromUtf8("баркли"))
        || name.contains(QString::fromUtf8("кальвин"))
        || name.contains(QString::fromUtf8("генри"))
        || name.contains(QString::fromUtf8("макинтош")))
        return QStringLiteral("commentary");

    if (name.contains(QString::fromUtf8("подстроч"))
        || name.contains(QLatin1String("interlinear"))
        || name.contains(QLatin1String("interlin"))
        || name.contains(QString::fromUtf8("межстроч"))
        || name.contains(QLatin1String("greek-russian"))
        || name.contains(QString::fromUtf8("греч"))
        || ini.flag(QStringLiteral("greek"), false)
        || ini.flag(QStringLiteral("hebrew"), false)
        || ini.flag(QStringLiteral("strongnumbers"), false)
           && (name.contains(QString::fromUtf8("подстр"))
               || name.contains(QLatin1String("inter"))))
        return QStringLiteral("interlinear");

    return QStringLiteral("bible");
}

// ============================================================================
//  Работа с HTML
// ============================================================================

static QString decodeHtmlEntities(QString s)
{
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&ensp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&emsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&thinsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&laquo;"), QString::fromUtf8("«"));
    s.replace(QStringLiteral("&raquo;"), QString::fromUtf8("»"));
    s.replace(QStringLiteral("&mdash;"), QString::fromUtf8("—"));
    s.replace(QStringLiteral("&ndash;"), QString::fromUtf8("–"));
    s.replace(QStringLiteral("&hellip;"), QString::fromUtf8("…"));
    s.replace(QStringLiteral("&copy;"), QString::fromUtf8("©"));
    s.replace(QStringLiteral("&reg;"), QString::fromUtf8("®"));
    s.replace(QStringLiteral("&deg;"), QString::fromUtf8("°"));
    s.replace(QStringLiteral("&plusmn;"), QString::fromUtf8("±"));
    s.replace(QStringLiteral("&sup2;"), QString::fromUtf8("²"));
    s.replace(QStringLiteral("&sup3;"), QString::fromUtf8("³"));
    s.replace(QStringLiteral("&micro;"), QString::fromUtf8("µ"));

    static const QRegularExpression reNum(QStringLiteral("&#(x?[0-9A-Fa-f]{1,6});"));
    QString out;
    out.reserve(s.size());
    int pos = 0;
    auto it = reNum.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(pos, m.capturedStart() - pos);
        const QString code = m.captured(1);
        bool ok = false;
        const uint v = code.startsWith(QLatin1Char('x'), Qt::CaseInsensitive)
                           ? code.mid(1).toUInt(&ok, 16)
                           : code.toUInt(&ok, 10);
        if (ok && v > 0 && v <= 0x10FFFF) {
            if (v == '<' || v == '>' || v == '&') {
                out += m.captured(0);
            } else {
                out += QString::fromUcs4(&v, 1);
            }
        } else {
            out += m.captured(0);
        }
        pos = m.capturedEnd();
    }
    out += s.mid(pos);
    return out;
}

static QString stripTagsToPlain(QString s)
{
    static const QRegularExpression reTag(QStringLiteral("<[^>]*>"));
    s.replace(reTag, QStringLiteral(" "));
    s = decodeHtmlEntities(s);
    s.replace(QStringLiteral("<"), QStringLiteral("<"));
    s.replace(QStringLiteral(">"), QStringLiteral(">"));
    s.replace(QStringLiteral("&"), QStringLiteral("&"));
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    return s.simplified();
}

static QString htmlToPlainWithLines(QString html)
{
    static const QRegularExpression reBr(
        QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reBlockEnd(
        QStringLiteral("</(?:p|div|h[1-6]|li|tr|table|ul|ol|blockquote)\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reBlockStart(
        QStringLiteral("<(?:p|div|h[1-6]|li|tr|table|ul|ol|blockquote)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reTag(QStringLiteral("<[^>]*>"));

    html.replace(reBr, QStringLiteral("\n"));
    html.replace(reBlockEnd, QStringLiteral("\n"));
    html.replace(reBlockStart, QStringLiteral("\n"));
    html.replace(reTag, QStringLiteral(" "));
    html = decodeHtmlEntities(html);
    html.replace(QStringLiteral("<"), QStringLiteral("<"));
    html.replace(QStringLiteral(">"), QStringLiteral(">"));
    html.replace(QStringLiteral("&"), QStringLiteral("&"));
    html.replace(QChar(0x00A0), QLatin1Char(' '));

    QStringList lines = html.split(QLatin1Char('\n'));
    for (QString &ln : lines) {
        ln.replace(QLatin1Char('\t'), QLatin1Char(' '));
        ln.replace(QRegularExpression(QStringLiteral(" {2,}")), QStringLiteral(" "));
        ln = ln.trimmed();
    }
    return lines.join(QLatin1Char('\n'));
}

static QString strongLinkHtml(const QString &rawNumber)
{
    const QString n = rawNumber.toUpper();
    QUrl href;
    href.setScheme(QStringLiteral("w"));
    href.setPath(QStringLiteral("/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("s"), n);
    href.setQuery(q);

    return QStringLiteral(
               "<sup><a href='%1' title='%2' "
               "style='color:#9a7a4a; text-decoration:none; font-size:0.72em;'>%2</a></sup>")
        .arg(href.toString(QUrl::FullyEncoded), n);
}

/** Нормализует красный текст (слова Иисуса) к единому span. */
static QString normalizeRedLetterHtml(QString html)
{
    static const QRegularExpression reFontOpen(
        QStringLiteral(
            "<font\\b[^>]*\\bcolor\\s*=\\s*[\"']?\\s*"
            "(?:#?(?:ff0000|c00000|c62828|cc0000|b71c1c|e53935|f44336|d32f2f|800000|990000|8b0000|a52a2a|b22222|dc143c|b71c1c|red)|"
            "rgb\\s*\\(\\s*2?5?5\\s*,\\s*0\\s*,\\s*0\\s*\\))"
            "[\"']?[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reFontClose(
        QStringLiteral("</font\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reSpanRedOpen(
        QStringLiteral(
            "<span\\b[^>]*style\\s*=\\s*[\"'][^\"']*\\bcolor\\s*:\\s*"
            "(?:#?(?:ff0000|c00000|c62828|cc0000|b71c1c|e53935|f44336|d32f2f)|red|"
            "rgb\\s*\\(\\s*2?5?5\\s*,\\s*0\\s*,\\s*0\\s*\\))"
            "[^\"']*[\"'][^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reWjClass(
        QStringLiteral(
            "<(?:span|font)\\b[^>]*class\\s*=\\s*[\"'][^\"']*"
            "(?:\\bwj\\b|words?[-_]?of[-_]?jesus|jesus[-_]?words)"
            "[^\"']*[\"'][^>]*>"),
        QRegularExpression::CaseInsensitiveOption);

    const QString open = QStringLiteral("<span style=\"color:#c62828;font-weight:500\">");
    const QString close = QStringLiteral("</span>");
    html.replace(reFontOpen, open);
    html.replace(reWjClass, open);
    html.replace(reSpanRedOpen, open);
    html.replace(reFontClose, close);
    return html;
}

/** Приводит фрагмент к «чистому» inline-HTML: только i/b/sup/sub/a/br. */
static QString cleanInlineHtml(QString html, bool strongLinks)
{
    html.remove(QChar(0xFEFF));
    html = normalizeRedLetterHtml(html);

    static const QRegularExpression reScript(
        QStringLiteral("<script[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reStyle(
        QStringLiteral("<style[^>]*>.*?</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reComment(
        QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    html.replace(reScript, QStringLiteral(" "));
    html.replace(reStyle, QStringLiteral(" "));
    html.replace(reComment, QStringLiteral(" "));

    static const QRegularExpression reImg(
        QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reImg, QStringLiteral(" "));

    static const QRegularExpression reBr(
        QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reBr, QStringLiteral("\n"));

    static const QRegularExpression reBlock(
        QStringLiteral("<\\/?(?:p|div|h[1-6]|li|tr|table|tbody|thead|tfoot|ul|ol|dl|dt|dd|"
                       "blockquote|section|article|header|footer|figure|figcaption|pre)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(reBlock, QStringLiteral("\n"));

    static const QRegularExpression reHr(
        QStringLiteral("<hr\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reHr, QStringLiteral("\n"));

    static const QRegularExpression reCell(
        QStringLiteral("<\\/?t[dh]\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reCell, QStringLiteral(" "));

    // Нормализуем акценты
    static const QRegularExpression reStrongOpen(
        QStringLiteral("<strong\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reStrongClose(
        QStringLiteral("</strong\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmOpen(
        QStringLiteral("<em\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmClose(
        QStringLiteral("</em\\s*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reStrongOpen, QStringLiteral("<b>"));
    html.replace(reStrongClose, QStringLiteral("</b>"));
    html.replace(reEmOpen, QStringLiteral("<i>"));
    html.replace(reEmClose, QStringLiteral("</i>"));

    static const QRegularExpression reIOpen(
        QStringLiteral("<i\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reBOpen(
        QStringLiteral("<b\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reIOpen, QStringLiteral("<i>"));
    html.replace(reBOpen, QStringLiteral("<b>"));

    // Убираем все теги, кроме разрешённых
    static const QRegularExpression reOther(
        QStringLiteral("<(?!/?(?:i|b|sup|sub|a|br|span)\\b)[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(reOther, QStringLiteral(" "));

    // span без красного стиля — снимаем обёртку
    {
        static const QRegularExpression reBadSpanOpen(
            QStringLiteral("<span\\b(?![^>]*c62828)[^>]*>"),
            QRegularExpression::CaseInsensitiveOption);
        html.replace(reBadSpanOpen, QString());
        static const QRegularExpression reEmptySpan(
            QStringLiteral("<span[^>]*>\\s*</span>"),
            QRegularExpression::CaseInsensitiveOption);
        html.replace(reEmptySpan, QString());
    }

    // Ссылки нам не нужны — оставляем только текст
    static const QRegularExpression reAOpen(
        QStringLiteral("<a\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reAClose(
        QStringLiteral("</a\\s*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reAOpen, QString());
    html.replace(reAClose, QString());

    html = decodeHtmlEntities(html);

    if (strongLinks) {
        // Номера Стронга — маленькие кликабельные надстрочные ссылки
        static const QRegularExpression reStrong(QStringLiteral("\\b([HG]\\d{1,5})\\b"),
                                                 QRegularExpression::CaseInsensitiveOption);
        QString out;
        out.reserve(html.size());
        int pos = 0;
        auto it = reStrong.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            out += html.mid(pos, m.capturedStart() - pos);
            out += strongLinkHtml(m.captured(1));
            pos = m.capturedEnd();
        }
        out += html.mid(pos);
        html = out;

        // В некоторых модулях номера Стронга записаны без H/G (07225, 2424)
        if (!g_strongPrefix.isEmpty()) {
            static const QRegularExpression rePlainStrong(QStringLiteral("\\b(\\d{3,5})\\b"));
            QString out2;
            out2.reserve(html.size());
            int pos2 = 0;
            auto it2 = rePlainStrong.globalMatch(html);
            while (it2.hasNext()) {
                const auto m = it2.next();
                out2 += html.mid(pos2, m.capturedStart() - pos2);
                QString digits = m.captured(1);
                while (digits.size() > 1 && digits.startsWith(QLatin1Char('0')))
                    digits.remove(0, 1);
                out2 += strongLinkHtml(g_strongPrefix + digits);
                pos2 = m.capturedEnd();
            }
            out2 += html.mid(pos2);
            html = out2;
        }
    } else {
        static const QRegularExpression reStrong(QStringLiteral("\\b[HG]\\d{1,5}\\b"),
                                                 QRegularExpression::CaseInsensitiveOption);
        html.replace(reStrong, QStringLiteral(" "));
    }

    // Пустые парные теги
    static const QRegularExpression reEmptyI(
        QStringLiteral("<i>\\s*</i>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmptyB(
        QStringLiteral("<b>\\s*</b>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmptySup(
        QStringLiteral("<sup>\\s*</sup>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reEmptyI, QString());
    html.replace(reEmptyB, QString());
    html.replace(reEmptySup, QString());

    // Построчная нормализация пробелов
    QStringList lines = html.split(QLatin1Char('\n'));
    for (QString &ln : lines) {
        ln.replace(QLatin1Char('\t'), QLatin1Char(' '));
        ln.replace(QChar(0x00A0), QLatin1Char(' '));
        ln.replace(QRegularExpression(QStringLiteral(" {2,}")), QStringLiteral(" "));
        ln.replace(QRegularExpression(QStringLiteral("\\s+([,.;:!?»”])")), QStringLiteral("\\1"));
        ln.replace(QRegularExpression(QStringLiteral("([«„(])\\s+")), QStringLiteral("\\1"));
        ln = ln.trimmed();
    }

    QStringList out;
    bool prevEmpty = false;
    for (const QString &ln : lines) {
        if (ln.isEmpty()) {
            if (!prevEmpty && !out.isEmpty())
                out << QString();
            prevEmpty = true;
        } else {
            out << ln;
            prevEmpty = false;
        }
    }
    return out.join(QLatin1Char('\n')).trimmed();
}

// ============================================================================
//  Извлечение маркеров
// ============================================================================

static int firstNumberInText(const QString &s, int maxValue = 300)
{
    static const QRegularExpression re(QStringLiteral("(\\d{1,3})"));
    const auto m = re.match(s);
    if (!m.hasMatch())
        return 0;
    const int n = m.captured(1).toInt();
    return (n > 0 && n <= maxValue) ? n : 0;
}

static void parseRangeInText(const QString &s, int &start, int &end)
{
    // «1-6», «1–6а», «6б-11», «12-16»
    static const QRegularExpression reRange(
        QStringLiteral("(\\d{1,3})[a-zA-Zа-яА-ЯёЁ]?\\s*[-–—]\\s*(\\d{1,3})[a-zA-Zа-яА-ЯёЁ]?"));
    const auto rm = reRange.match(s);
    if (rm.hasMatch()) {
        start = rm.captured(1).toInt();
        end = rm.captured(2).toInt();
        if (end < start)
            end = start;
        return;
    }
    start = firstNumberInText(s);
    end = start;
}

static int extractChapterNumber(const QString &content, const QString &lookahead,
                                int fallback, bool chapterZero, int expectedChapters)
{
    QString text = stripTagsToPlain(content);
    if (text.isEmpty())
        text = stripTagsToPlain(lookahead.left(500));
    else if (!lookahead.isEmpty())
        text += QLatin1Char(' ') + stripTagsToPlain(lookahead.left(300));
    text = text.simplified();

    if (text.isEmpty())
        return fallback;

    static const QRegularExpression reWord(
        QStringLiteral("(?:глава|главы|глав|chapter|chapters|psalm|psalms|псалом|псалмы|"
                       "part|часть)\\s*([0-9]{1,3})"),
        QRegularExpression::CaseInsensitiveOption);
    const auto wm = reWord.match(text);

    int n = -1;
    if (wm.hasMatch()) {
        n = wm.captured(1).toInt();
    } else {
        QList<int> nums;
        static const QRegularExpression reNum(QStringLiteral("(\\d{1,3})"));
        auto it = reNum.globalMatch(text);
        while (it.hasNext()) {
            const int v = it.next().captured(1).toInt();
            if (v >= 0 && v <= 200)
                nums << v;
        }
        if (!nums.isEmpty())
            n = nums.last();
    }

    if (n < 0 || n > 200)
        n = fallback;
    if (!chapterZero && n == 0)
        n = fallback;
    if (expectedChapters > 0 && n > expectedChapters + 5)
        n = fallback;
    return n;
}

static QVector<ChapterMark> findChapterMarks(const QString &html, const IniData &ini,
                                             int expectedChapters)
{
    QVector<ChapterMark> marks;

    const QString sign = ini.get(QStringLiteral("chaptersign"), QStringLiteral("<h4>")).trimmed();
    QString compact = sign.toLower();
    compact.remove(QLatin1Char(' '));
    const bool chapterZero = ini.flag(QStringLiteral("chapterzero"), false);

    int seq = chapterZero ? 0 : 1;

    auto add = [&](int pos, int end, const QString &content, const QString &lookahead) {
        const int n = extractChapterNumber(content, lookahead, seq, chapterZero, expectedChapters);
        marks.append({n, pos, end});
        seq = qMax(seq + 1, n + 1);
    };

    if (compact.startsWith(QStringLiteral("<!--"))) {
        const QRegularExpression re(QRegularExpression::escape(sign),
                                    QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), QString(),
                html.mid(m.capturedEnd(), 500));
        }
    } else if (compact.startsWith(QStringLiteral("aname"))) {
        static const QRegularExpression re(
            QStringLiteral("<a\\s+[^>]*\\bname\\s*=\\s*[\"']?([^\"'\\s>]+)[\"']?[^>]*>"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            bool ok = false;
            int n = m.captured(1).toInt(&ok);
            if (!ok || n < 0 || n > 200)
                n = seq;
            marks.append({n, m.capturedStart(), m.capturedEnd()});
            seq = qMax(seq + 1, n + 1);
        }
    } else if (compact.startsWith(QStringLiteral("<g"))) {
        static const QRegularExpression re(
            QStringLiteral("<g\\b[^>]{0,40}(?:\\r?\\n|>)"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), QString(),
                html.mid(m.capturedEnd(), 500));
        }
    } else if (compact.startsWith(QStringLiteral("<h"))) {
        static const QRegularExpression reTag(QStringLiteral("<(h[1-6])\\b"),
                                              QRegularExpression::CaseInsensitiveOption);
        const auto tm = reTag.match(sign);
        const QString tag = tm.hasMatch() ? tm.captured(1).toLower() : QStringLiteral("h4");

        const QRegularExpression re(QStringLiteral("<%1\\b[^>]*>(.*?)</%1\\s*>").arg(tag),
                                    QRegularExpression::CaseInsensitiveOption
                                        | QRegularExpression::DotMatchesEverythingOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), m.captured(1), QString());
        }
        if (marks.isEmpty()) {
            const QRegularExpression re2(QStringLiteral("<%1\\b[^>]*>([^\\r\\n<]*)").arg(tag),
                                         QRegularExpression::CaseInsensitiveOption);
            auto it2 = re2.globalMatch(html);
            while (it2.hasNext()) {
                const auto m = it2.next();
                add(m.capturedStart(), m.capturedEnd(), m.captured(1), QString());
            }
        }
    } else if (compact.startsWith(QStringLiteral("<p"))) {
        static const QRegularExpression re(
            QStringLiteral("<p\\b[^>]*>\\s*<hr\\b[^>]*>"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), QString(),
                html.mid(m.capturedEnd(), 500));
        }
    } else if (compact.startsWith(QLatin1Char('<'))) {
        const QRegularExpression re(QRegularExpression::escape(sign) + QStringLiteral("[^>]*>"),
                                    QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), QString(),
                html.mid(m.capturedEnd(), 500));
        }
    } else {
        const QRegularExpression re(
            QStringLiteral("(?:^|[\\r\\n])\\s*") + QRegularExpression::escape(sign)
                + QStringLiteral("\\s*(\\d{1,3})"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            marks.append({m.captured(1).toInt(), m.capturedStart(), m.capturedEnd()});
        }
    }

    // Универсальные запасные варианты
    if (marks.isEmpty()) {
        static const QRegularExpression reGlava(
            QStringLiteral("(?:^|[\\r\\n])\\s*(?:Глава|ГЛАВА|Chapter|Псалом|Psalm)\\s+(\\d{1,3})"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        auto it = reGlava.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            marks.append({m.captured(1).toInt(), m.capturedStart(), m.capturedEnd()});
        }
    }
    if (marks.isEmpty()) {
        static const QRegularExpression reH4(
            QStringLiteral("<h[1-6]\\b[^>]*>\\s*(\\d{1,3})\\s*</h[1-6]\\s*>"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = reH4.globalMatch(html);
        while (it.hasNext()) {
            const auto m = it.next();
            marks.append({m.captured(1).toInt(), m.capturedStart(), m.capturedEnd()});
        }
    }

    std::sort(marks.begin(), marks.end(),
              [](const ChapterMark &a, const ChapterMark &b) { return a.pos < b.pos; });
    return marks;
}

static QVector<VerseMark> extractSupMarks(const QString &html)
{
    QVector<VerseMark> marks;
    static const QRegularExpression re(
        QStringLiteral("<sup\\b[^>]*>(.*?)</sup>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    auto it = re.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString inner = stripTagsToPlain(m.captured(1));
        int num = 0;
        int endNum = 0;
        parseRangeInText(inner, num, endNum);
        if (num <= 0 || num > 300)
            continue;
        // <sup>2</sup>*  — сноска
        const QString afterSup = html.mid(m.capturedEnd(), 8).trimmed();
        if (afterSup.startsWith(QLatin1Char('*')))
            continue;
        const QString inn = inner.trimmed();
        if (inn.contains(QLatin1Char('*')) && !inn.contains(QLatin1Char('-'))
            && inn.length() < 6)
            continue;
        marks.append({num, endNum, int(m.capturedStart()), int(m.capturedEnd())});
    }
    return marks;
}

static QVector<VerseMark> extractPMarks(const QString &html, const QString &moduleKind)
{
    QVector<VerseMark> marks;
    static const QRegularExpression reP(QStringLiteral("<p\\b[^>]*>"),
                                        QRegularExpression::CaseInsensitiveOption);

    QVector<int> starts;
    auto it = reP.globalMatch(html);
    while (it.hasNext())
        starts << int(it.next().capturedStart());

    static const QRegularExpression reRange(
        QStringLiteral("^(\\d{1,3})[a-zA-Zа-яА-ЯёЁ]?\\s*[-–—]\\s*(\\d{1,3})[a-zA-Zа-яА-ЯёЁ]?"));
    static const QRegularExpression reNum(
        QStringLiteral("^(\\d{1,3})(?:\\s*[\\.\\)\\:\\*]|\\s|$)"));
    static const QRegularExpression reSupLead(
        QStringLiteral("^\\s*<sup\\b[^>]*>\\s*(?:<[^>]+>)*\\s*(\\d{1,3})\\s*"
                       "(?:<[^>]+>)*\\s*</sup>"),
        QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression reFootOnly(
        QStringLiteral("^(\\d{1,3})\\s*[*†‡§]\\s*$"));
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int end = (i + 1 < starts.size()) ? starts[i + 1] : html.size();
        const int gt = html.indexOf(QLatin1Char('>'), start);
        if (gt < 0)
            continue;
        const int pEnd = gt + 1;
        const QString para = html.mid(pEnd, end - pEnd);
        const QString plain = stripTagsToPlain(para.left(400)).trimmed();

        // «2 *» / «3*» — сноска, не начало стиха
        if (reFootOnly.match(plain).hasMatch())
            continue;
        // абзац из одних сносок «2 * 3 * 4 * 5 *»
        {
            QString p2 = plain;
            p2.replace(QRegularExpression(QString::fromUtf8(R"(\d{1,3}\s*[*†‡§])")), QString());
            if (plain.contains(QLatin1Char('*')) && p2.trimmed().isEmpty())
                continue;
        }

        int num = 0;
        int endNum = 0;
        const auto rm = reRange.match(plain);
        if (rm.hasMatch()) {
            num = rm.captured(1).toInt();
            endNum = rm.captured(2).toInt();
        } else {
            const auto nm = reNum.match(plain);
            if (nm.hasMatch()) {
                num = nm.captured(1).toInt();
                endNum = num;
            }
        }
        if (num <= 0 || num > 300)
            continue;

        int bodyStart = pEnd;
        const auto sm = reSupLead.match(para);
        if (sm.hasMatch() && sm.captured(1).toInt() == num)
            bodyStart = pEnd + int(sm.capturedEnd());

        marks.append({num, endNum, start, bodyStart});

        // Внутри абзаца могут быть номера следующих стихов.
        // Для диапазонов вида «1-17» этого делать нельзя — там номера
        // почти наверняка являются ссылками внутри толкования.
        if (endNum == num) {
            static const QRegularExpression reBoldNum(
                QStringLiteral("<(?:b|strong)\\b[^>]*>\\s*(\\d{1,3})\\s*[\\.\\)]"),
                QRegularExpression::CaseInsensitiveOption);
            auto bit = reBoldNum.globalMatch(para);
            while (bit.hasNext()) {
                const auto bm = bit.next();
                const int n2 = bm.captured(1).toInt();
                if (n2 <= num || n2 > num + 20 || n2 > 300)
                    continue;
                marks.append({n2, n2, pEnd + int(bm.capturedStart()),
                              pEnd + int(bm.capturedEnd())});
            }

            if (moduleKind == QLatin1String("commentary")) {
                static const QRegularExpression rePlainNum(
                    QStringLiteral("(?:^|[\\s>])(\\d{1,3})\\s*[\\.\\)]\\s+(?=\\S)"));
                auto pit = rePlainNum.globalMatch(para);
                while (pit.hasNext()) {
                    const auto pm = pit.next();
                    const int n2 = pm.captured(1).toInt();
                    if (n2 != num + 1 && n2 != num + 2)
                        continue;
                    if (n2 > 300)
                        continue;
                    marks.append({n2, n2, pEnd + int(pm.capturedStart()),
                                  pEnd + int(pm.capturedEnd())});
                }
            }
        }
    }
    return marks;
}

static QVector<VerseMark> extractAnchorMarks(const QString &html)
{
    QVector<VerseMark> marks;
    static const QRegularExpression re(
        QStringLiteral("<a\\s+[^>]*\\bname\\s*=\\s*[\"']?([^\"'\\s>]+)[\"']?[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);

    auto it = re.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        bool ok = false;
        const int n = m.captured(1).toInt(&ok);
        if (!ok || n <= 0 || n > 300)
            continue;
        marks.append({n, n, int(m.capturedStart()), int(m.capturedEnd())});
    }
    return marks;
}

static QVector<VerseMark> extractHeadingMarks(const QString &html, const QString &verseSign)
{
    QVector<VerseMark> marks;
    static const QRegularExpression reTag(QStringLiteral("<(h[1-6])\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    const auto tm = reTag.match(verseSign);
    const QString tag = tm.hasMatch() ? tm.captured(1).toLower() : QStringLiteral("h4");

    const QRegularExpression re(QStringLiteral("<%1\\b[^>]*>(.*?)</%1\\s*>").arg(tag),
                                QRegularExpression::CaseInsensitiveOption
                                    | QRegularExpression::DotMatchesEverythingOption);
    auto it = re.globalMatch(html);
    int seq = 0;
    while (it.hasNext()) {
        const auto m = it.next();
        ++seq;
        marks.append({seq, seq, int(m.capturedStart()), int(m.capturedEnd())});
    }
    return marks;
}

static QVector<VerseMark> extractInlineMarks(const QString &html)
{
    QVector<VerseMark> marks;
    QString plain = htmlToPlainWithLines(html);

    static const QRegularExpression reInline(
        QStringLiteral("([^\\n\\d])[ \\t]+(\\d{1,3})([\\.\\)\\:\\*])[ \\t]+"));
    for (int k = 0; k < 8; ++k) {
        const QString before = plain;
        plain.replace(reInline, QStringLiteral("\\1\n\\2\\3 "));
        if (plain == before)
            break;
    }

    static const QRegularExpression re(
        QStringLiteral("(?:^|[\\n\\r])[ \\t]*(\\d{1,3})(?:[\\.\\)\\:\\*]|[ \\t]+)(?=\\S)"),
        QRegularExpression::MultilineOption);

    auto it = re.globalMatch(plain);
    while (it.hasNext()) {
        const auto m = it.next();
        const int n = m.captured(1).toInt();
        if (n <= 0 || n > 300)
            continue;
        // после номера только * или конец — сноска, не стих
        const int after = m.capturedEnd();
        const QString tail = plain.mid(after, 12).trimmed();
        if (tail.startsWith(QLatin1Char('*')) || tail.startsWith(QString::fromUtf8("†"))
            || tail.startsWith(QString::fromUtf8("‡")))
            continue;
        // проверяем, что дальше есть буквы (текст стиха)
        bool hasLetter = false;
        for (int k = after; k < plain.size() && k < after + 80; ++k) {
            if (plain.at(k).isLetter()) { hasLetter = true; break; }
            if (plain.at(k) == QLatin1Char('\n')) break;
        }
        if (!hasLetter)
            continue;
        marks.append({n, n, int(m.capturedStart()), int(m.capturedEnd())});
    }
    return marks;
}

static QVector<VerseMark> filterVerseMarks(QVector<VerseMark> marks)
{
    std::sort(marks.begin(), marks.end(), [](const VerseMark &a, const VerseMark &b) {
        if (a.markStart != b.markStart)
            return a.markStart < b.markStart;
        return a.num < b.num;
    });

    QVector<VerseMark> out;
    int coveredUntil = 0;

    for (const VerseMark &m : marks) {
        if (m.num <= 0 || m.num > 300)
            continue;
        // внутри уже покрытого диапазона (сноски 2* 3* при «1-6а»)
        if (m.num <= coveredUntil)
            continue;
        if (!out.isEmpty() && out.last().num == m.num)
            continue;

        out << m;
        const int end = (m.endNum > m.num) ? m.endNum : m.num;
        coveredUntil = qMax(coveredUntil, end);
    }
    return out;
}

// ============================================================================
//  Очистка текста стиха
// ============================================================================

static QString cleanVerseText(QString text, int verseNum, bool stripLeading,
                              bool strongLinks, int nextChapter = 0)
{
    text = cleanInlineHtml(text, strongLinks);

    if (stripLeading) {
        // Ведущий заголовок «Глава N» (Calvin и подобные)
        static const QRegularExpression reBoldChapterLead(
            QStringLiteral("^\\s*<b>\\s*(?:Глава|ГЛАВА|Chapter|Псалом|Psalm)\\s+\\d{1,3}\\s*</b>\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(reBoldChapterLead, QString());
        static const QRegularExpression reChapterLead(
            QStringLiteral("^\\s*(?:Глава|ГЛАВА|Chapter|Псалом|Psalm)\\s+\\d{1,3}\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(reChapterLead, QString());

        // Ведущий «<b>N.</b>»
        static const QRegularExpression reBoldLead(
            QStringLiteral("^\\s*<(?:b|strong)\\b[^>]*>\\s*(\\d{1,3})\\s*[\\.\\)]\\s*"
                           "(?:</(?:b|strong)\\s*>)?\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        auto bm = reBoldLead.match(text);
        if (bm.hasMatch()) {
            const int n = bm.captured(1).toInt();
            if (n == verseNum || (n > 0 && n <= 200))
                text = text.mid(bm.capturedEnd());
        }

        // Номера/диапазоны «1-6а», «6б-11», «17» оставляем в тексте как в источнике.
        // Не вырезаем их (стиль «Радостная Весть» и подобные).

        // Ведущий <sup>N</sup>
        static const QRegularExpression reSup(
            QStringLiteral("^\\s*<sup\\b[^>]*>\\s*(\\d{1,3})\\s*</sup>\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        auto sm = reSup.match(text);
        if (sm.hasMatch()) {
            const int n = sm.captured(1).toInt();
            if (n == verseNum || (n > 0 && n <= 200))
                text = text.mid(sm.capturedEnd());
        }
    }

    // Висячие сноски и маркеры
    static const QRegularExpression reJunkLead(
        QStringLiteral("^[\\s\\*\\†\\‡\\§]+"));
    text.replace(reJunkLead, QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+\\*\\s*")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral("\\s+\\*$")), QString());

    // Хвостовой номер следующей главы
    if (nextChapter > 0) {
        static const QRegularExpression reTrailLine(
            QStringLiteral("(?:^|[\\r\\n])[ \\t]*(\\d{1,3})[ \\t]*$"));
        auto tm = reTrailLine.match(text);
        if (tm.hasMatch() && tm.captured(1).toInt() == nextChapter) {
            const QString core = text.left(tm.capturedStart()).trimmed();
            if (core.size() >= 8)
                text = core;
        }
        static const QRegularExpression reTrailSpace(
            QStringLiteral("[ \\t]+(\\d{1,3})[ \\t]*$"));
        auto sm = reTrailSpace.match(text);
        if (sm.hasMatch() && sm.captured(1).toInt() == nextChapter) {
            const QString core = text.left(sm.capturedStart()).trimmed();
            if (core.size() >= 40)
                text = core;
        }
    }

    // Точный дубль половины текста (частая проблема модулей BibleQuote)
    if (text.size() >= 24) {
        const int mid = text.size() / 2;
        for (int i = mid - 8; i <= mid + 8 && i > 0 && i < text.size() - 1; ++i) {
            if (text.at(i) != QLatin1Char(' ') && text.at(i) != QLatin1Char('\n'))
                continue;
            const QString a = text.left(i).trimmed();
            const QString b = text.mid(i + 1).trimmed();
            if (a.size() >= 12 && a == b) {
                text = a;
                break;
            }
        }
    }

    text = text.trimmed();
    return text;
}

// ============================================================================
//  Запись в SQLite
// ============================================================================

static bool upsertVerse(QSqlQuery &select, QSqlQuery &insert, int bookId, int chapter,
                        int verse, const QString &text, bool allowEmpty = false)
{
    // Для библейских модулей (как в Синодальном) сохраняем номер стиха
    // даже если текст пустой или только сноска «*».
    QString store = text;
    if (store.trimmed().isEmpty()) {
        if (!allowEmpty)
            return false;
        store = QString(QChar(0x200B)); // zero-width space — маркер пустого стиха
    }

    QString existing;
    select.bindValue(0, bookId);
    select.bindValue(1, chapter);
    select.bindValue(2, verse);
    if (select.exec() && select.next())
        existing = select.value(0).toString();

    QString merged = store;
    if (!existing.isEmpty()) {
        const bool existEmpty = existing.trimmed().isEmpty()
                                || existing == QString(QChar(0x200B));
        const bool storeEmpty = store == QString(QChar(0x200B));
        if (!existEmpty && storeEmpty)
            return false; // не затираем реальный текст пустышкой
        if (existing.contains(store) && !storeEmpty)
            return false;
        if (store.contains(existing) && !existEmpty)
            merged = store;
        else if (!existEmpty && !storeEmpty)
            merged = existing + QLatin1Char('\n') + store;
        else if (existEmpty)
            merged = store;
    }

    insert.bindValue(0, bookId);
    insert.bindValue(1, chapter);
    insert.bindValue(2, verse);
    insert.bindValue(3, merged);
    return insert.exec();
}

// ============================================================================
//  Разбор главы
// ============================================================================


/** Стиль «Радостная Весть»: блоки «1-6а … 2* 3*», «6б-11 …», «17 Текст».
 *  Делим только по реальным началам блоков; сноски «2 *» не становятся стихами. */
static QVector<VerseMark> extractRangeStyleMarks(const QString &html)
{
    QVector<VerseMark> marks;
    // В plain-тексте ищем начала блоков
    const QString plain = htmlToPlainWithLines(html);

    // Диапазон: 1-6а | 6б-11 | 12-16
    // Обычный стих: 17 + пробел + буква (не *)
    static const QRegularExpression reBlock(
        QStringLiteral(
            "(?:^|[\\n\\r])[ \\t]*"
            "("
              "\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?\\s*[-–—]\\s*\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?"
              "|"
              "\\d{1,3}"
            ")"
            "(?=[ \\t]+[A-Za-zА-Яа-яЁё«\"„(])"
        ),
        QRegularExpression::MultilineOption);

    struct Hit { int num; int endNum; int pos; int endPos; QString label; };
    QVector<Hit> hits;

    auto it = reBlock.globalMatch(plain);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString label = m.captured(1).trimmed();
        int startN = 0, endN = 0;
        parseRangeInText(label, startN, endN);
        if (startN <= 0 || startN > 300)
            continue;
        // отсечь ложные «2 *» — lookahead уже требует букву, но на всякий случай
        const int afterLabel = m.capturedEnd();
        const QString tail = plain.mid(afterLabel, 6).trimmed();
        if (tail.startsWith(QLatin1Char('*')))
            continue;
        hits.append({startN, endN > startN ? endN : startN,
                     int(m.capturedStart()), int(m.capturedEnd()), label});
    }

    if (hits.size() < 2)
        return marks;

    // Сколько диапазонов — признак стиля РВ
    int rangeCount = 0;
    for (const Hit &h : hits)
        if (h.endNum > h.num)
            ++rangeCount;
    if (rangeCount < 1)
        return marks; // обычный модуль — не вмешиваемся

    // Позиции в plain нельзя напрямую мапить в html; работаем с plain-разбиением
    // через специальные маркеры в html: вставляем якоря не будем —
    // parseChapterChunkRangeStyle обработает plain целиком.
    for (const Hit &h : hits)
        marks.append({h.num, h.endNum, h.pos, h.endPos});

    return marks;
}

/** Разбор главы в стиле диапазонов (Радостная Весть) из plain-текста. */
static int parseRangeStyleChapter(const QString &html, int chapterNum, int bookId,
                                  bool strongLinks, QSqlQuery &select, QSqlQuery &insert)
{
    QString work = normalizeRedLetterHtml(html);
    const QString redOpenMark = QStringLiteral("[[RED]]");
    const QString redCloseMark = QStringLiteral("[[/RED]]");
    work.replace(QStringLiteral("<span style=\"color:#c62828;font-weight:500\">"), redOpenMark);
    work.replace(QStringLiteral("</span>"), redCloseMark);
    QString plain = htmlToPlainWithLines(work);
    plain.replace(redOpenMark, QStringLiteral("<span style=\"color:#c62828;font-weight:500\">"));
    plain.replace(redCloseMark, QStringLiteral("</span>"));
    plain.replace(QRegularExpression(QStringLiteral("[\\t\\v\\f]+")), QStringLiteral(" "));

    static const QRegularExpression reBlock(
        QStringLiteral(
            "(?:^|[\\n\\r])[ \\t]*"
            "("
              "\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?\\s*[-–—]\\s*\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?"
              "|"
              "\\d{1,3}"
            ")"
            "(?=[ \\t]+[A-Za-zА-Яа-яЁё«\"„(])"
        ),
        QRegularExpression::MultilineOption);

    struct Hit { int num; int endNum; int pos; int labelLen; };
    QVector<Hit> hits;
    auto it = reBlock.globalMatch(plain);
    while (it.hasNext()) {
        const auto m = it.next();
        QString label = m.captured(1).trimmed();
        int startN = 0, endN = 0;
        parseRangeInText(label, startN, endN);
        if (startN <= 0 || startN > 300)
            continue;
        const QString tail = plain.mid(m.capturedEnd(), 6).trimmed();
        if (tail.startsWith(QLatin1Char('*')))
            continue;
        // capturedStart may include newline — body starts at label
        int labelStart = int(m.capturedStart());
        // shift to first digit
        while (labelStart < plain.size() && !plain.at(labelStart).isDigit())
            ++labelStart;
        hits.append({startN, endN > startN ? endN : startN,
                     labelStart, label.size()});
    }

    if (hits.isEmpty())
        return 0;

    int inserted = 0;
    for (int i = 0; i < hits.size(); ++i) {
        const Hit &h = hits[i];
        const int to = (i + 1 < hits.size()) ? hits[i + 1].pos : plain.size();
        QString body = plain.mid(h.pos, to - h.pos).trimmed();
        // нормализуем пробелы, сохраняя переносы абзацев
        body.replace(QRegularExpression(QStringLiteral(" *\\n *")), QStringLiteral("\n"));
        body.replace(QRegularExpression(QStringLiteral("\\n{3,}")), QStringLiteral("\n\n"));
        body = body.trimmed();

        bool hasLetter = false;
        for (QChar ch : body) {
            if (ch.isLetter()) { hasLetter = true; break; }
        }
        if (!hasLetter)
            continue;

        // лёгкая очистка HTML-сущностей уже сделана; strong не трогаем в plain
        Q_UNUSED(strongLinks);
        if (upsertVerse(select, insert, bookId, chapterNum, h.num, body, false))
            ++inserted;
    }
    return inserted;
}

static bool chapterLooksLikeRangeStyle(const QString &html)
{
    const QString plain = stripTagsToPlain(html.left(8000));
    static const QRegularExpression reRange(
        QStringLiteral("\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?\\s*[-–—]\\s*\\d{1,3}[a-zA-Zа-яА-ЯёЁ]?"));
    int ranges = 0;
    auto it = reRange.globalMatch(plain);
    while (it.hasNext()) {
        it.next();
        ++ranges;
        if (ranges >= 2)
            return true;
    }
    // сноски 2 * 3 * 4 *
    static const QRegularExpression reFoots(
        QStringLiteral("\\d{1,3}\\s*\\*\\s*\\d{1,3}\\s*\\*"));
    return reFoots.match(plain).hasMatch() && ranges >= 1;
}

static QVector<VerseMark> extractVerseMarks(const QString &chapterHtml,
                                            const QString &verseSign,
                                            const QString &moduleKind,
                                            QString *methodOut)
{
    const QString sign = verseSign.toLower();

    const QVector<VerseMark> supMarks = extractSupMarks(chapterHtml);
    const QVector<VerseMark> pMarks = extractPMarks(chapterHtml, moduleKind);

    if (sign.contains(QStringLiteral("sup"))) {
        if (!supMarks.isEmpty()) {
            if (methodOut) *methodOut = QStringLiteral("sup");
            return supMarks;
        }
    }

    // Если <sup>-маркеров достаточно много, значит это номера стихов,
    // а не сноски (в одном абзаце может быть несколько стихов — Сперджен и т.п.)
    if (!supMarks.isEmpty() && supMarks.size() >= 2
        && (moduleKind == QLatin1String("commentary")
            || supMarks.size() > pMarks.size()
            || supMarks.size() * 2 >= pMarks.size())) {
        if (methodOut) *methodOut = QStringLiteral("sup");
        return supMarks;
    }

    if (sign.contains(QLatin1Char('<')) && sign.contains(QLatin1Char('h'))) {
        const QVector<VerseMark> hMarks = extractHeadingMarks(chapterHtml, verseSign);
        if (!hMarks.isEmpty()) {
            if (methodOut) *methodOut = QStringLiteral("heading");
            return hMarks;
        }
    }

    if (sign.contains(QStringLiteral("name"))) {
        const QVector<VerseMark> aMarks = extractAnchorMarks(chapterHtml);
        if (!aMarks.isEmpty()) {
            if (methodOut) *methodOut = QStringLiteral("anchor");
            return aMarks;
        }
    }

    if (!pMarks.isEmpty()) {
        if (methodOut) *methodOut = QStringLiteral("p");
        return pMarks;
    }

    if (!supMarks.isEmpty()) {
        if (methodOut) *methodOut = QStringLiteral("sup");
        return supMarks;
    }

    const QVector<VerseMark> inlineMarks = extractInlineMarks(chapterHtml);
    if (!inlineMarks.isEmpty()) {
        if (methodOut) *methodOut = QStringLiteral("inline");
        return inlineMarks;
    }

    return {};
}

static int insertParagraphs(const QString &chunk, int chapterNum, int bookId,
                            bool strongLinks, QSqlQuery &select, QSqlQuery &insert)
{
    QString normalized = chunk;
    static const QRegularExpression reDoubleBr(
        QStringLiteral("<br\\s*/?>\\s*<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rePClose(
        QStringLiteral("</p\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rePOpen(
        QStringLiteral("<p\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reBlock(
        QStringLiteral("</?(?:div|h[1-6]|li|tr|table|blockquote)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);

    normalized.replace(reDoubleBr, QStringLiteral("\n\n"));
    normalized.replace(rePClose, QStringLiteral("\n\n"));
    normalized.replace(rePOpen, QStringLiteral("\n\n"));
    normalized.replace(reBlock, QStringLiteral("\n\n"));

    const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\n{2,}")),
                                               Qt::SkipEmptyParts);
    int seq = 0;
    for (const QString &part : parts) {
        const QString text = cleanVerseText(part, seq + 1, false, strongLinks, 0);
        if (text.length() < 2)
            continue;
        ++seq;
        upsertVerse(select, insert, bookId, chapterNum, seq, text);
    }
    return seq;
}

/** Раскладывает блок толкования по номерам стихов (для Сперджена и подобных). */
static int insertCommentaryTail(const QString &tail, int fallbackVerse, int chapterNum, int bookId,
                                bool strongLinks, QSqlQuery &select, QSqlQuery &insert)
{
    QString normalized = tail;
    static const QRegularExpression rePClose(
        QStringLiteral("</p\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rePOpen(
        QStringLiteral("<p\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    normalized.replace(rePClose, QStringLiteral("\n\n"));
    normalized.replace(rePOpen, QStringLiteral("\n\n"));

    const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\n{2,}")),
                                               Qt::SkipEmptyParts);
    int lastVerse = fallbackVerse;
    int inserted = 0;

    static const QRegularExpression reLead(
        QStringLiteral("^(\\d{1,3})\\s*[\\.\\)\\:\\*]\\s*"));

    for (const QString &part : parts) {
        const QString plain = stripTagsToPlain(part.left(120)).trimmed();
        int target = lastVerse;
        const auto m = reLead.match(plain);
        if (m.hasMatch()) {
            const int n = m.captured(1).toInt();
            if (n > 0 && n <= 200) {
                target = n;
                lastVerse = n;
            }
        }
        const QString text = cleanVerseText(part, target, true, strongLinks, 0);
        if (text.length() >= 2
            && upsertVerse(select, insert, bookId, chapterNum, target, text)) {
            ++inserted;
        }
    }
    return inserted;
}

static int parseChapterChunk(const QString &chunk, int chapterNum, int bookId,
                             const QString &verseSign, const QString &moduleKind,
                             bool strongLinks, int nextChapter,
                             QSqlQuery &select, QSqlQuery &insert,
                             QString *methodOut)
{
    // Радостная Весть и подобные — разбор «как в источнике»
    if (moduleKind != QLatin1String("commentary")
        && moduleKind != QLatin1String("dictionary")
        && chapterLooksLikeRangeStyle(chunk)) {
        if (methodOut)
            *methodOut = QStringLiteral("range-style");
        const int n = parseRangeStyleChapter(chunk, chapterNum, bookId,
                                             strongLinks, select, insert);
        if (n > 0)
            return n;
    }

    QVector<VerseMark> marks = extractVerseMarks(chunk, verseSign, moduleKind, methodOut);
    marks = filterVerseMarks(marks);
    const bool supMethod = (methodOut && *methodOut == QLatin1String("sup"));

    if (marks.isEmpty()) {
        if (moduleKind == QLatin1String("bible")) {
            const QString text = cleanVerseText(chunk, 1, true, strongLinks, nextChapter);
            if (text.length() >= 2 && upsertVerse(select, insert, bookId, chapterNum, 1, text))
                return 1;
            return 0;
        }
        return insertParagraphs(chunk, chapterNum, bookId, strongLinks, select, insert);
    }

    // Текст до первого маркера — заголовок/подзаголовок главы
    QString prefix = chunk.left(marks.first().markStart);
    prefix = cleanVerseText(prefix, marks.first().num, false, strongLinks, 0);
    {
        static const QRegularExpression reHead(
            QStringLiteral("^\\s*(?:<b>\\s*)?(?:Глава|ГЛАВА|Chapter|Псалом|Psalm)\\s+"
                           "\\d{1,3}\\s*(?:</b>)?\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        prefix.replace(reHead, QString());
        prefix = prefix.trimmed();
    }

    const bool commentary = moduleKind == QLatin1String("commentary");
    auto attachPrefix = [&](int verseNum, QString &mainText) {
        if (prefix.isEmpty())
            return;
        // Длинное вступление комментария выносим в отдельный «стих 0»,
        // чтобы не раздувать первый стих Писания.
        if (commentary && prefix.length() > 250) {
            upsertVerse(select, insert, bookId, chapterNum, 0, prefix);
            return;
        }
        if (!mainText.isEmpty())
            mainText = prefix + QLatin1Char('\n') + mainText;
        else
            mainText = prefix;
        Q_UNUSED(verseNum);
    };

    int inserted = 0;
    for (int i = 0; i < marks.size(); ++i) {
        const VerseMark &m = marks[i];
        int to = (i + 1 < marks.size()) ? marks[i + 1].markStart : chunk.size();
        if (to < m.bodyStart)
            to = chunk.size();

        QString text = chunk.mid(m.bodyStart, to - m.bodyStart);

        // В комментариях с <sup> отделяем толкование от стиха Писания
        // и раскладываем его по номерам стихов (Сперджен и т.п.).
        if (commentary && supMethod) {
            static const QRegularExpression rePClose(
                QStringLiteral("</p\\s*>"), QRegularExpression::CaseInsensitiveOption);
            const auto pm = rePClose.match(text);
            if (pm.hasMatch() && pm.capturedEnd() < text.size()) {
                const QString tail = text.mid(pm.capturedEnd());
                text = text.left(pm.capturedEnd());

                QString mainText = cleanVerseText(text, m.num, true, strongLinks, nextChapter);
                if (i == 0)
                    attachPrefix(m.num, mainText);
                if (mainText.length() >= 2
                    && upsertVerse(select, insert, bookId, chapterNum, m.num, mainText)) {
                    ++inserted;
                }
                inserted += insertCommentaryTail(tail, m.num, chapterNum, bookId,
                                                 strongLinks, select, insert);
                continue;
            }
        }

        QString cleaned = cleanVerseText(text, m.num, true, strongLinks, nextChapter);

        if (i == 0)
            attachPrefix(m.num, cleaned);

        // Сноски вида «*» / пустой текст после диапазона (1-6а) — в библейских
        // модулях всё равно сохраняем номер стиха, как в Синодальном.
        static const QRegularExpression reOnlyFoot(
            QStringLiteral(R"(^[*†‡§·•\s]+$)"));
        if (reOnlyFoot.match(cleaned.trimmed()).hasMatch())
            cleaned.clear();

        // Не создаём пустые стихи — нужен реальный текст с буквами.
        {
            bool hasLetter = false;
            for (QChar ch : cleaned) {
                if (ch.isLetter()) { hasLetter = true; break; }
            }
            if (hasLetter
                && upsertVerse(select, insert, bookId, chapterNum, m.num, cleaned, false)) {
                ++inserted;
            }
        }

        // Диапазоны (1-6а и т.п.) — один блок, без пустых промежуточных стихов.
    }
    return inserted;
}

// ============================================================================
//  Разбор книги
// ============================================================================

static QString locateBookFile(const QString &moduleRoot, const QString &pathName)
{
    QString rel = pathName;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QString direct = QDir(moduleRoot).absoluteFilePath(rel);
    if (QFile::exists(direct))
        return direct;

    const QFileInfo fi(rel);
    const QString fileName = fi.fileName();
    const QString stem = fi.completeBaseName();

    // Поиск по всему модулю без учёта регистра
    QDirIterator it(moduleRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo pfi(p);
        if (pfi.fileName().compare(fileName, Qt::CaseInsensitive) == 0)
            return p;
        if (pfi.completeBaseName().compare(stem, Qt::CaseInsensitive) == 0)
            return p;
    }
    return QString();
}

static bool parseBookFile(const QString &htmlPath, int bookId, const IniBook &book,
                          const IniData &ini, const QString &moduleKind,
                          QSqlQuery &select, QSqlQuery &insert,
                          int &chaptersOut, int &versesOut, QString &methodOut)
{
    chaptersOut = 0;
    versesOut = 0;

    const bool strongLinks = ini.flag(QStringLiteral("strongnumbers"), false);
    if (strongLinks) {
        if (!ini.flag(QStringLiteral("oldtestament"), true)
            && ini.flag(QStringLiteral("newtestament"), false)) {
            g_strongPrefix = QStringLiteral("G");
        } else if (bookId <= 39) {
            g_strongPrefix = QStringLiteral("H");
        } else if (bookId <= 66) {
            g_strongPrefix = QStringLiteral("G");
        } else {
            g_strongPrefix.clear();
        }
    } else {
        g_strongPrefix.clear();
    }

    QFile f(htmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QString content = decodeBytesWithHint(f.readAll(), ini.get(QStringLiteral("defaultencoding")));
    content.remove(QChar(0xFEFF));

    static const QRegularExpression reScript(
        QStringLiteral("<script[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reStyle(
        QStringLiteral("<style[^>]*>.*?</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    content.replace(reScript, QStringLiteral(" "));
    content.replace(reStyle, QStringLiteral(" "));

    const int expectedChapters = qMax(1, book.chapterQty);
    QVector<ChapterMark> chapterMarks = findChapterMarks(content, ini, expectedChapters);

    if (chapterMarks.size() < 2 && expectedChapters > 1) {
        // Один маркер на много глав — пробуем сплошной разбор по номерам стихов
        const QVector<VerseMark> marks = filterVerseMarks(
            extractVerseMarks(content, ini.get(QStringLiteral("versesign"), QStringLiteral("<p>")),
                              moduleKind, &methodOut));
        if (marks.size() >= 3) {
            const bool chapterZero = ini.flag(QStringLiteral("chapterzero"), false);
            int chapter = chapterZero ? 0 : 1;
            int prevNum = 0;
            QSet<int> seenChapters;

            // Текст до первого стиха
            QString prefix = content.left(marks.first().markStart);
            prefix = cleanVerseText(prefix, 1, false,
                                    ini.flag(QStringLiteral("strongnumbers")), 0);

            for (int i = 0; i < marks.size(); ++i) {
                const VerseMark &m = marks[i];
                if (prevNum > 0 && m.num == 1 && prevNum >= 2)
                    ++chapter;
                else if (prevNum > 0 && m.num < prevNum && m.num <= 2 && prevNum >= 5)
                    ++chapter;

                int to = (i + 1 < marks.size()) ? marks[i + 1].markStart : content.size();
                QString text = content.mid(m.bodyStart, to - m.bodyStart);
                QString cleaned = cleanVerseText(text, m.num, true,
                                                 ini.flag(QStringLiteral("strongnumbers")), 0);
                if (i == 0 && !prefix.isEmpty()) {
                    if (!cleaned.isEmpty())
                        cleaned = prefix + QLatin1Char('\n') + cleaned;
                    else
                        cleaned = prefix;
                }
                if (cleaned.length() >= 2) {
                    if (upsertVerse(select, insert, bookId, chapter, m.num, cleaned)) {
                        ++versesOut;
                        seenChapters.insert(chapter);
                    }
                }
                prevNum = m.num;
            }
            chaptersOut = seenChapters.size();
            methodOut = QStringLiteral("continuous");
            return versesOut > 0;
        }
    }

    if (chapterMarks.isEmpty()) {
        const bool chapterZero = ini.flag(QStringLiteral("chapterzero"), false);
        const int chapter = chapterZero ? 0 : 1;
        const int v = parseChapterChunk(content, chapter, bookId,
                                        ini.get(QStringLiteral("versesign"), QStringLiteral("<p>")),
                                        moduleKind, ini.flag(QStringLiteral("strongnumbers")),
                                        chapter + 1, select, insert, &methodOut);
        if (v > 0) {
            chaptersOut = 1;
            versesOut = v;
        }
        return v > 0;
    }

    QSet<int> seenChapters;
    for (int i = 0; i < chapterMarks.size(); ++i) {
        const ChapterMark &cm = chapterMarks[i];
        const int from = cm.end;
        const int to = (i + 1 < chapterMarks.size()) ? chapterMarks[i + 1].pos : content.size();
        if (to <= from)
            continue;

        const QString chunk = content.mid(from, to - from);
        const int nextChapter = (i + 1 < chapterMarks.size())
                                    ? chapterMarks[i + 1].num
                                    : cm.num + 1;
        const int v = parseChapterChunk(chunk, cm.num, bookId,
                                        ini.get(QStringLiteral("versesign"), QStringLiteral("<p>")),
                                        moduleKind, ini.flag(QStringLiteral("strongnumbers")),
                                        nextChapter, select, insert, &methodOut);
        if (v > 0) {
            seenChapters.insert(cm.num);
            versesOut += v;
        }
    }

    chaptersOut = seenChapters.size();
    return versesOut > 0;
}

// ============================================================================
//  Словари BibleQuote (.idx + .htm)
// ============================================================================

static QString findDictionaryHtmlForStem(const QString &moduleRoot, const QString &stem)
{
    QDirIterator it(moduleRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi(p);
        if (fi.completeBaseName().compare(stem, Qt::CaseInsensitive) != 0)
            continue;
        const QString suffix = fi.suffix().toLower();
        if (suffix == QLatin1String("htm") || suffix == QLatin1String("html"))
            return p;
    }
    return QString();
}

static QVector<QPair<QString, int>> parseDictionaryIdxEntries(const QString &idxPath,
                                                              const QString &encodingHint)
{
    QVector<QPair<QString, int>> entries;

    QFile inf(idxPath);
    if (!inf.open(QIODevice::ReadOnly))
        return entries;
    const QString idxText = decodeBytesWithHint(inf.readAll(), encodingHint);
    inf.close();

    const QStringList lines = idxText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);
    for (int i = 0; i + 1 < lines.size(); ++i) {
        const QString term = lines[i].trimmed();
        bool ok = false;
        const int off = lines[i + 1].trimmed().toInt(&ok);
        if (!ok || term.isEmpty() || term.size() > 250)
            continue;
        entries.append(qMakePair(term, off));
        ++i;
    }

    // Бинарные/экзотические индексы (например, Strong's) не должны
    // превращаться в «книги» из мусорных символов.
    static const QRegularExpression reLetter(QStringLiteral("\\p{L}"));
    int withLetters = 0;
    for (const auto &e : entries) {
        if (reLetter.match(e.first).hasMatch())
            ++withLetters;
    }
    if (withLetters == 0)
        entries.clear();

    return entries;
}

static int convertDictionaryModule(const QString &moduleRoot, const IniData &ini,
                                   bool strongLinks,
                                   QSqlQuery &insertBook, QSqlQuery &insertVerse,
                                   QSqlQuery &selectVerse,
                                   int &outBooks, int &outVerses,
                                   QStringList &outNames, QSet<QString> &methods)
{
    outBooks = 0;
    outVerses = 0;
    g_strongPrefix.clear();

    QDir dir(moduleRoot);
    const QStringList idxFiles = dir.entryList(QStringList() << QStringLiteral("*.idx")
                                                             << QStringLiteral("*.IDX"),
                                               QDir::Files);
    int bookId = 0;

    for (const QString &idxName : idxFiles) {
        const QString idxPath = dir.absoluteFilePath(idxName);
        const QString stem = QFileInfo(idxName).completeBaseName();

        const QString htmlPath = findDictionaryHtmlForStem(moduleRoot, stem);
        if (htmlPath.isEmpty())
            continue;

        QFile hf(htmlPath);
        if (!hf.open(QIODevice::ReadOnly))
            continue;
        const QByteArray raw = hf.readAll();
        hf.close();
        if (raw.isEmpty())
            continue;

        const QString enc = ini.get(QStringLiteral("defaultencoding"));
        QVector<QPair<QString, int>> entries = parseDictionaryIdxEntries(idxPath, enc);
        if (entries.isEmpty())
            continue;

        std::sort(entries.begin(), entries.end(),
                  [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                      return a.second < b.second;
                  });

        QVector<QPair<QString, int>> unique;
        for (const auto &e : entries) {
            if (unique.isEmpty() || unique.last().second != e.second)
                unique.append(e);
        }

        for (int e = 0; e < unique.size(); ++e) {
            const int off = unique[e].second;
            if (off < 0 || off >= raw.size())
                continue;
            const int next = (e + 1 < unique.size()) ? unique[e + 1].second : raw.size();
            if (next <= off)
                continue;

            QString text = cleanVerseText(decodeBytesWithHint(raw.mid(off, next - off), enc),
                                          1, false, strongLinks, 0);
            const QString term = unique[e].first.trimmed();

            // Убираем повтор заголовка статьи
            const QString firstLine = text.section(QLatin1Char('\n'), 0, 0).trimmed();
            if (firstLine.compare(term, Qt::CaseInsensitive) == 0)
                text = text.section(QLatin1Char('\n'), 1).trimmed();
            else if (text.startsWith(term, Qt::CaseInsensitive))
                text = text.mid(term.size()).trimmed();

            if (text.length() < 2)
                continue;

            ++bookId;
            QString fullName = term;
            if (bookId == 1
                && (fullName.contains(QString::fromUtf8("словар"), Qt::CaseInsensitive)
                    || fullName.contains(QStringLiteral("dictionary"), Qt::CaseInsensitive)))
                fullName = QString::fromUtf8("Введение");

            insertBook.bindValue(0, bookId);
            insertBook.bindValue(1, term.left(60));
            insertBook.bindValue(2, fullName);
            insertBook.bindValue(3, 1);
            insertBook.exec();

            insertVerse.bindValue(0, bookId);
            insertVerse.bindValue(1, 1);
            insertVerse.bindValue(2, 1);
            insertVerse.bindValue(3, text);
            if (insertVerse.exec()) {
                ++outBooks;
                ++outVerses;
                outNames << fullName;
            }
        }
        methods.insert(QStringLiteral("idx"));
    }

    // Резерв: словарь без .idx — разбираем первый HTML как набор абзацев
    if (outBooks == 0) {
        QString htmlPath;
        QDirIterator it(moduleRoot, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            const QString suffix = QFileInfo(p).suffix().toLower();
            if (suffix == QLatin1String("htm") || suffix == QLatin1String("html")) {
                htmlPath = p;
                break;
            }
        }
        if (!htmlPath.isEmpty()) {
            QFile f(htmlPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QString content = decodeBytesWithHint(f.readAll(),
                                                            ini.get(QStringLiteral("defaultencoding")));
                const int v = insertParagraphs(content, 1, 1, strongLinks, selectVerse, insertVerse);
                if (v > 0) {
                    insertBook.bindValue(0, 1);
                    insertBook.bindValue(1, ini.get(QStringLiteral("bibleshortname"),
                                                    QStringLiteral("Dict")));
                    insertBook.bindValue(2, ini.get(QStringLiteral("biblename"),
                                                    QString::fromUtf8("Словарь")));
                    insertBook.bindValue(3, 1);
                    insertBook.exec();
                    outBooks = 1;
                    outVerses = v;
                    outNames << ini.get(QStringLiteral("biblename"), QString::fromUtf8("Словарь"));
                    methods.insert(QStringLiteral("paragraphs"));
                }
            }
        }
    }

    return outVerses;
}

// ============================================================================
//  Анализ
// ============================================================================

static QString previewFirstChapter(const QString &moduleRoot, const IniBook &book,
                                   const IniData &ini, const QString &moduleKind)
{
    g_strongPrefix = ini.flag(QStringLiteral("strongnumbers"), false)
                         ? QStringLiteral("H")
                         : QString();
    const QString path = locateBookFile(moduleRoot, book.pathName);
    if (path.isEmpty())
        return QString();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();

    QString content = decodeBytesWithHint(f.readAll(), ini.get(QStringLiteral("defaultencoding")));
    content.remove(QChar(0xFEFF));

    const int expected = qMax(1, book.chapterQty);
    QVector<ChapterMark> marks = findChapterMarks(content, ini, expected);
    QString chunk;
    int chapterNum = ini.flag(QStringLiteral("chapterzero"), false) ? 0 : 1;

    if (!marks.isEmpty()) {
        chapterNum = marks.first().num;
        const int from = marks.first().end;
        const int to = (marks.size() > 1) ? marks[1].pos : content.size();
        chunk = content.mid(from, to - from);
    } else {
        chunk = content;
    }

    QString method;
    QVector<VerseMark> vmarks = extractVerseMarks(
        chunk, ini.get(QStringLiteral("versesign"), QStringLiteral("<p>")), moduleKind, &method);
    vmarks = filterVerseMarks(vmarks);

    if (vmarks.isEmpty()) {
        const QString text = cleanVerseText(chunk, 1, true,
                                            ini.flag(QStringLiteral("strongnumbers")), 0);
        QString plain = stripTagsToPlain(text);
        if (plain.size() > 140)
            plain = plain.left(140) + QString::fromUtf8("…");
        return plain.isEmpty() ? QString() : QString::fromUtf8("гл. %1, ст. 1: %2")
                                                     .arg(chapterNum)
                                                     .arg(plain);
    }

    QStringList lines;
    for (int i = 0; i < qMin(3, vmarks.size()); ++i) {
        const VerseMark &m = vmarks[i];
        int to = (i + 1 < vmarks.size()) ? vmarks[i + 1].markStart : chunk.size();
        QString text = chunk.mid(m.bodyStart, to - m.bodyStart);
        text = cleanVerseText(text, m.num, true,
                              ini.flag(QStringLiteral("strongnumbers")), 0);
        QString plain = stripTagsToPlain(text);
        if (plain.size() > 120)
            plain = plain.left(120) + QString::fromUtf8("…");
        lines << QString::fromUtf8("  %1. %2").arg(m.num).arg(plain);
    }
    return QString::fromUtf8("гл. %1 (%2):\n%3")
        .arg(chapterNum)
        .arg(method.isEmpty() ? QStringLiteral("—") : method)
        .arg(lines.join(QLatin1Char('\n')));
}

ConversionResult Converter::analyze(const QString &bibleQuoteModulePath) const
{
    ConversionResult result;
    if (!QDir(bibleQuoteModulePath).exists()) {
        result.message = QString::fromUtf8("Папка не существует");
        return result;
    }

    const QString iniPath = findIniFile(bibleQuoteModulePath);
    if (iniPath.isEmpty()) {
        result.message = QString::fromUtf8("Не найден файл BibleQt.ini / BIBLEQT.INI");
        return result;
    }

    IniData ini = parseIni(iniPath);
    completeBooksFromDirectory(bibleQuoteModulePath, ini);
    result.success = true;
    result.detectedName = ini.get(QStringLiteral("biblename"),
                                  QFileInfo(bibleQuoteModulePath).fileName());
    result.detectedType = detectModuleType(ini);
    if (ini.books.isEmpty() && hasDictionaryIdxFiles(bibleQuoteModulePath))
        result.detectedType = QStringLiteral("dictionary");
    result.booksExpected = ini.books.size();

    const QString chapterSign = ini.get(QStringLiteral("chaptersign"), QStringLiteral("<h4>"));
    const QString verseSign = ini.get(QStringLiteral("versesign"), QStringLiteral("<p>"));
    const QString encodingHint = ini.get(QStringLiteral("defaultencoding"));

    // Словари BibleQuote: .idx + .htm, без списка PathName
    if (result.detectedType == QLatin1String("dictionary") && ini.books.isEmpty()) {
        QDir dir(bibleQuoteModulePath);
        const QStringList idxFiles = dir.entryList(QStringList() << QStringLiteral("*.idx")
                                                                 << QStringLiteral("*.IDX"),
                                                   QDir::Files);
        int totalEntries = 0;
        QStringList idxNames;
        QString preview;

        for (const QString &idxName : idxFiles) {
            const auto entries = parseDictionaryIdxEntries(dir.absoluteFilePath(idxName),
                                                           encodingHint);
            if (entries.isEmpty())
                continue;
            totalEntries += entries.size();
            idxNames << idxName;

            if (preview.isEmpty()) {
                const QString htmlPath = findDictionaryHtmlForStem(
                    bibleQuoteModulePath, QFileInfo(idxName).completeBaseName());
                if (!htmlPath.isEmpty()) {
                    QFile hf(htmlPath);
                    if (hf.open(QIODevice::ReadOnly)) {
                        const QByteArray raw = hf.readAll();
                        hf.close();
                        int off = entries.first().second;
                        if (off >= 0 && off < raw.size()) {
                            QString text = cleanVerseText(
                                decodeBytesWithHint(raw.mid(off), encodingHint),
                                1, false, ini.flag(QStringLiteral("strongnumbers")), 0);
                            const QString term = entries.first().first.trimmed();
                            const QString firstLine = text.section(QLatin1Char('\n'), 0, 0).trimmed();
                            if (firstLine.compare(term, Qt::CaseInsensitive) == 0)
                                text = text.section(QLatin1Char('\n'), 1).trimmed();
                            preview = stripTagsToPlain(text);
                            if (preview.size() > 220)
                                preview = preview.left(220) + QString::fromUtf8("…");
                        }
                    }
                }
            }
        }

        if (totalEntries > 0) {
            result.success = true;
            result.booksExpected = totalEntries;
            QStringList rep;
            rep << QString::fromUtf8("══ АНАЛИЗ СЛОВАРЯ ══");
            rep << QString::fromUtf8("Название: %1").arg(result.detectedName);
            rep << QString::fromUtf8("Тип: dictionary");
            rep << QString::fromUtf8("Статей в индексах: %1").arg(totalEntries);
            rep << QString::fromUtf8("Индексы: %1").arg(idxNames.join(QStringLiteral(", ")));
            if (!preview.isEmpty()) {
                rep << QString::fromUtf8("Предпросмотр:");
                rep << QString::fromUtf8("  %1").arg(preview);
            }
            rep << QString::fromUtf8("✓ Словарь можно конвертировать: каждая статья станет отдельной книгой.");
            result.analysisReport = rep.join(QLatin1Char('\n'));
            result.message = result.analysisReport;
            return result;
        }

        // Без .idx — попробуем разобрать первый HTML по абзацам
        QDirIterator it(bibleQuoteModulePath, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            const QString suffix = QFileInfo(p).suffix().toLower();
            if (suffix == QLatin1String("htm") || suffix == QLatin1String("html")) {
                result.success = true;
                result.booksExpected = 1;
                result.analysisReport = QString::fromUtf8(
                    "══ АНАЛИЗ СЛОВАРЯ ══\nНазвание: %1\nТип: dictionary\n"
                    "Индекс .idx не найден — HTML будет разобран по абзацам.")
                    .arg(result.detectedName);
                result.message = result.analysisReport;
                return result;
            }
        }

        result.success = false;
        result.message = QString::fromUtf8("Словарь не содержит ни .idx, ни HTML-файлов.");
        return result;
    }

    int htmlFound = 0;
    int htmlMissing = 0;
    QStringList missingFiles;
    QString encodingNote;
    int sampleH4 = 0;
    int sampleSup = 0;
    int sampleP = 0;

    for (int i = 0; i < ini.books.size(); ++i) {
        const IniBook &b = ini.books[i];
        const QString p = locateBookFile(bibleQuoteModulePath, b.pathName);
        if (p.isEmpty()) {
            ++htmlMissing;
            if (missingFiles.size() < 8)
                missingFiles << b.pathName;
            continue;
        }
        ++htmlFound;

        if (i < 6) {
            QFile f(p);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray raw = f.read(256 * 1024);
                const QString html = decodeBytesWithHint(raw, encodingHint);
                sampleH4 += html.count(QRegularExpression(QStringLiteral("<h[1-6]"),
                                                          QRegularExpression::CaseInsensitiveOption));
                sampleSup += html.count(QRegularExpression(QStringLiteral("<sup"),
                                                           QRegularExpression::CaseInsensitiveOption));
                sampleP += html.count(QRegularExpression(QStringLiteral("<p\\b"),
                                                         QRegularExpression::CaseInsensitiveOption));
                if (encodingNote.isEmpty()) {
                    if (raw.startsWith("\xFF\xFE") || raw.startsWith("\xFE\xFF"))
                        encodingNote = QStringLiteral("UTF-16");
                    else if (raw.startsWith("\xEF\xBB\xBF"))
                        encodingNote = QStringLiteral("UTF-8 (BOM)");
                    else if (QString::fromUtf8(raw).contains(QChar::ReplacementCharacter))
                        encodingNote = QStringLiteral("Windows-1251 / KOI8-R");
                    else
                        encodingNote = QStringLiteral("UTF-8");
                }
            }
        }
    }

    QStringList report;
    report << QString::fromUtf8("══ АНАЛИЗ МОДУЛЯ ══");
    report << QString::fromUtf8("Название: %1").arg(result.detectedName);
    report << QString::fromUtf8("Тип: %1").arg(result.detectedType);
    report << QString::fromUtf8("Копирайт: %1").arg(ini.get(QStringLiteral("copyright"),
                                                             QString::fromUtf8("—")));
    report << QString::fromUtf8("Книг в INI: %1").arg(ini.books.size());
    report << QString::fromUtf8("HTML найдено: %1, не найдено: %2").arg(htmlFound).arg(htmlMissing);
    if (!missingFiles.isEmpty())
        report << QString::fromUtf8("Нет файлов: %1").arg(missingFiles.join(QStringLiteral(", ")));
    report << QString::fromUtf8("ChapterSign: %1").arg(chapterSign);
    report << QString::fromUtf8("VerseSign: %1").arg(verseSign);
    report << QString::fromUtf8("ChapterZero: %1").arg(ini.flag(QStringLiteral("chapterzero")) ? "Y" : "N");
    report << QString::fromUtf8("StrongNumbers: %1").arg(ini.flag(QStringLiteral("strongnumbers")) ? "Y" : "N");
    report << QString::fromUtf8("Кодировка: %1").arg(encodingNote.isEmpty() ? "—" : encodingNote);
    report << QString::fromUtf8("В выборке HTML: <h*>≈%1, <sup>≈%2, <p>≈%3")
                  .arg(sampleH4).arg(sampleSup).arg(sampleP);

    if (!ini.books.isEmpty()) {
        report << QString::fromUtf8("Первые книги:");
        for (int i = 0; i < qMin(5, ini.books.size()); ++i) {
            const IniBook &b = ini.books[i];
            report << QString::fromUtf8("  • %1 — %2 гл. (%3)")
                          .arg(b.fullName.isEmpty() ? b.pathName : b.fullName)
                          .arg(b.chapterQty)
                          .arg(b.pathName);
        }

        const QString preview = previewFirstChapter(bibleQuoteModulePath, ini.books.first(),
                                                    ini, result.detectedType);
        if (!preview.isEmpty()) {
            report << QString::fromUtf8("Предпросмотр:");
            report << preview;
        }
    }

    if (htmlFound == 0) {
        report << QString::fromUtf8("⚠ HTML-файлы книг не найдены — конвертация текста невозможна.");
        result.success = false;
    } else if (htmlMissing > 0) {
        report << QString::fromUtf8("⚠ Часть файлов отсутствует — эти книги будут пропущены.");
    } else {
        report << QString::fromUtf8("✓ Структура выглядит пригодной для конвертации.");
    }

    result.analysisReport = report.join(QLatin1Char('\n'));
    result.message = result.analysisReport;
    return result;
}

// ============================================================================
//  Пост-аудит
// ============================================================================

QString Converter::runPostConversionAudit(const QString &moduleDir, int expectedBooks,
                                          const QStringList &expectedBookNames) const
{
    Q_UNUSED(expectedBookNames)

    QStringList report;
    report << QString::fromUtf8("══ АУДИТ ПОСЛЕ КОНВЕРТАЦИИ ══");

    int errors = 0;
    int warnings = 0;

    const QString dbPath = moduleDir + QStringLiteral("/data.sqlite");
    const QString metaPath = moduleDir + QStringLiteral("/module.json");
    if (!QFile::exists(metaPath)) {
        report << QString::fromUtf8("✗ Нет module.json");
        return report.join(QLatin1Char('\n'));
    }
    if (!QFile::exists(dbPath)) {
        report << QString::fromUtf8("✗ Нет data.sqlite");
        return report.join(QLatin1Char('\n'));
    }

    QString moduleType = QStringLiteral("bible");
    {
        QFile mf(metaPath);
        if (mf.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll());
            moduleType = doc.object().value(QStringLiteral("type")).toString(moduleType);
        }
    }

    const QString conn = QStringLiteral("audit_") + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            report << QString::fromUtf8("✗ Не открывается data.sqlite");
            QSqlDatabase::removeDatabase(conn);
            return report.join(QLatin1Char('\n'));
        }

        QSqlQuery q(db);

        int bookCount = 0;
        QStringList bookNames;
        QList<int> bookIds;
        if (q.exec(QStringLiteral("SELECT id, full_name, chapters FROM books ORDER BY id"))) {
            while (q.next()) {
                ++bookCount;
                bookIds << q.value(0).toInt();
                bookNames << q.value(1).toString();
            }
        }
        report << QString::fromUtf8("Книг в базе: %1 (ожидалось ≈%2)")
                      .arg(bookCount).arg(expectedBooks);
        if (expectedBooks > 0 && bookCount < expectedBooks) {
            ++warnings;
            report << QString::fromUtf8("⚠ Книг меньше, чем в INI (%1 < %2)")
                          .arg(bookCount).arg(expectedBooks);
        }
        if (bookCount == 0) {
            ++errors;
            report << QString::fromUtf8("✗ В базе нет ни одной книги");
        }

        int verseCount = 0;
        int emptyVerses = 0;
        int chaptersWithText = 0;
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM verses"))) {
            if (q.next())
                verseCount = q.value(0).toInt();
        }
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM verses WHERE length(trim(text)) < 2"))) {
            if (q.next())
                emptyVerses = q.value(0).toInt();
        }
        if (q.exec(QStringLiteral(
                "SELECT COUNT(DISTINCT book || '-' || chapter) FROM verses "
                "WHERE length(trim(text)) > 2"))) {
            if (q.next())
                chaptersWithText = q.value(0).toInt();
        }
        report << QString::fromUtf8("Стихов: %1, глав с текстом: %2, почти пустых: %3")
                      .arg(verseCount).arg(chaptersWithText).arg(emptyVerses);

        if (verseCount < 10) {
            ++errors;
            report << QString::fromUtf8("✗ Слишком мало стихов — конвертация почти наверняка провалилась");
        } else if (moduleType == QLatin1String("bible") && expectedBooks >= 20
                   && verseCount < expectedBooks * 10) {
            ++errors;
            report << QString::fromUtf8("✗ Слишком мало стихов для библейского модуля");
        } else if (moduleType == QLatin1String("bible") && expectedBooks >= 20
                   && verseCount < expectedBooks * 50) {
            ++warnings;
            report << QString::fromUtf8("⚠ Мало стихов относительно числа книг");
        }

        // Книги без стихов
        int booksWithoutVerses = 0;
        QStringList emptyBooks;
        for (int i = 0; i < bookIds.size(); ++i) {
            q.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM verses WHERE book = ? AND length(trim(text)) > 1"));
            q.bindValue(0, bookIds[i]);
            int c = 0;
            if (q.exec() && q.next())
                c = q.value(0).toInt();
            if (c == 0) {
                ++booksWithoutVerses;
                if (emptyBooks.size() < 6)
                    emptyBooks << bookNames.value(i);
            }
        }
        if (booksWithoutVerses > 0) {
            ++warnings;
            report << QString::fromUtf8("⚠ Книг без текста: %1 (%2)")
                          .arg(booksWithoutVerses).arg(emptyBooks.join(QStringLiteral(", ")));
        }

        // Выборочная проверка «как видит человек»
        report << QString::fromUtf8("— Выборочный просмотр текста —");
        int samples = 0;
        for (int i = 0; i < bookIds.size() && samples < 5; ++i) {
            q.prepare(QStringLiteral(
                "SELECT verse, text FROM verses WHERE book = ? AND chapter = 1 "
                "ORDER BY verse LIMIT 6"));
            q.bindValue(0, bookIds[i]);
            if (!q.exec())
                continue;

            QStringList lines;
            int vcount = 0;
            bool gluedNumber = false;
            bool blockHtml = false;
            bool rawEntities = false;

            while (q.next()) {
                ++vcount;
                const int vn = q.value(0).toInt();
                QString tx = q.value(1).toString().trimmed();

                if (tx.contains(QRegularExpression(QStringLiteral("<\\/?(?:p|div|h[1-6]|table)\\b"),
                                                    QRegularExpression::CaseInsensitiveOption)))
                    blockHtml = true;
                if (tx.contains(QRegularExpression(QStringLiteral("&[a-zA-Z]+;"))))
                    rawEntities = true;

                static const QRegularExpression reGlued(
                    QStringLiteral("^\\d{1,3}[А-Яа-яA-Za-z«\"]"));
                if (reGlued.match(tx).hasMatch())
                    gluedNumber = true;

                QString plain = stripTagsToPlain(tx);
                if (plain.size() > 100)
                    plain = plain.left(100) + QString::fromUtf8("…");
                lines << QString::fromUtf8("    %1. %2").arg(vn).arg(plain);
            }

            if (vcount == 0)
                continue;
            ++samples;

            report << QString::fromUtf8("  [%1] гл.1 (%2 ст., %3):")
                          .arg(bookNames.value(i))
                          .arg(vcount)
                          .arg(blockHtml ? QString::fromUtf8("HTML-блоки")
                                         : QString::fromUtf8("чистый текст"));
            report << lines.join(QLatin1Char('\n'));

            if (gluedNumber) {
                ++warnings;
                report << QString::fromUtf8("  ⚠ В тексте есть прилипшие номера стихов");
            }
            if (blockHtml) {
                ++errors;
                report << QString::fromUtf8("  ✗ В тексте остались блочные HTML-теги");
            }
            if (rawEntities) {
                ++warnings;
                report << QString::fromUtf8("  ⚠ В тексте остались HTML-сущности");
            }
            if (vcount == 1 && moduleType == QLatin1String("bible")) {
                ++errors;
                report << QString::fromUtf8("  ✗ В главе 1 только 1 запись стиха — текст не разбит");
            }
        }
        if (samples == 0 && verseCount > 0) {
            ++warnings;
            report << QString::fromUtf8("⚠ Не удалось выбрать образцы текста для просмотра");
        }

        report << QString::fromUtf8("— Итог —");
        if (errors == 0 && warnings == 0) {
            report << QString::fromUtf8("✓ Аудит пройден: модуль выглядит готовым к использованию.");
        } else if (errors == 0) {
            report << QString::fromUtf8("△ Аудит с замечаниями (%1). Модуль можно открывать, "
                                        "но проверьте проблемные книги.").arg(warnings);
        } else {
            report << QString::fromUtf8("✗ Аудит не пройден (ошибок: %1, замечаний: %2).")
                          .arg(errors).arg(warnings);
        }

        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return report.join(QLatin1Char('\n'));
}

// ============================================================================
//  Конвертация
// ============================================================================


static QString firstShortAlias(const QString &shortName, const QString &fullName, int bookId)
{
    QString s = shortName.trimmed();
    if (!s.isEmpty()) {
        // первое «слово» / аббревиатура до пробела
        const int sp = s.indexOf(QLatin1Char(' '));
        if (sp > 0)
            s = s.left(sp).trimmed();
        // убрать завершающую точку для компактности оставляем как есть
        if (!s.isEmpty())
            return s;
    }
    if (!fullName.trimmed().isEmpty())
        return fullName.trimmed();
    return QString::number(bookId);
}


ConversionResult Converter::convert(const QString &bibleQuoteModulePath, const QString &outputDir)
{
    ConversionResult result = analyze(bibleQuoteModulePath);
    if (!result.success)
        return result;

    const QString iniPath = findIniFile(bibleQuoteModulePath);
    IniData ini = parseIni(iniPath);
    completeBooksFromDirectory(bibleQuoteModulePath, ini);
    QString moduleType = detectModuleType(ini);
    if (ini.books.isEmpty() && hasDictionaryIdxFiles(bibleQuoteModulePath))
        moduleType = QStringLiteral("dictionary");
    const bool strongLinks = ini.flag(QStringLiteral("strongnumbers"), false);

    QDir().mkpath(outputDir);

    // Чистим предыдущую конвертацию
    QFile::remove(outputDir + QStringLiteral("/data.sqlite"));
    QFile::remove(outputDir + QStringLiteral("/data.sqlite-wal"));
    QFile::remove(outputDir + QStringLiteral("/data.sqlite-shm"));

    QJsonObject meta;
    meta[QStringLiteral("name")] = result.detectedName;
    meta[QStringLiteral("description")] = ini.get(
        QStringLiteral("copyright"), QString::fromUtf8("Сконвертировано из BibleQuote"));
    meta[QStringLiteral("type")] = moduleType;
    meta[QStringLiteral("language")] = ini.get(QStringLiteral("language"), QStringLiteral("ru"));
    meta[QStringLiteral("version")] = QStringLiteral("1.0");
    meta[QStringLiteral("source")] = QStringLiteral("biblequote");
    meta[QStringLiteral("short_name")] = ini.get(QStringLiteral("bibleshortname"));
    meta[QStringLiteral("strong_numbers")] = strongLinks;
    meta[QStringLiteral("books_expected")] = ini.books.size();

    const QString connName = QStringLiteral("converter_")
                             + QUuid::createUuid().toString(QUuid::Id128);
    int totalVerses = 0;
    int totalBooks = 0;
    int expectedBooks = ini.books.size();
    QStringList convertedBookNames;
    QSet<QString> parseMethods;

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(outputDir + QStringLiteral("/data.sqlite"));
        if (!db.open()) {
            result.success = false;
            result.message = QString::fromUtf8("Не удалось создать базу данных");
            QSqlDatabase::removeDatabase(connName);
            return result;
        }

        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA synchronous = OFF"));
        q.exec(QStringLiteral("PRAGMA journal_mode = MEMORY"));
        q.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
        q.exec(QStringLiteral("BEGIN"));
        q.exec(QStringLiteral(
            "CREATE TABLE books (id INTEGER PRIMARY KEY, short_name TEXT, full_name TEXT, "
            "chapters INTEGER)"));
        q.exec(QStringLiteral(
            "CREATE TABLE verses (book INTEGER, chapter INTEGER, verse INTEGER, text TEXT, "
            "PRIMARY KEY(book, chapter, verse))"));

        QSqlQuery insertBook(db);
        insertBook.prepare(QStringLiteral(
            "INSERT INTO books (id, short_name, full_name, chapters) VALUES (?, ?, ?, ?)"));
        QSqlQuery insertVerse(db);
        insertVerse.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO verses (book, chapter, verse, text) VALUES (?, ?, ?, ?)"));
        QSqlQuery selectVerse(db);
        selectVerse.prepare(QStringLiteral(
            "SELECT text FROM verses WHERE book = ? AND chapter = ? AND verse = ?"));

        emit progress(0, ini.books.size(), QString::fromUtf8("Начало конвертации…"));
        QCoreApplication::processEvents();

        bool dictionaryDone = false;
        if (moduleType == QLatin1String("dictionary") && ini.books.isEmpty()) {
            int dBooks = 0;
            int dVerses = 0;
            QStringList dNames;
            QSet<QString> dMethods;

            convertDictionaryModule(bibleQuoteModulePath, ini, strongLinks,
                                    insertBook, insertVerse, selectVerse,
                                    dBooks, dVerses, dNames, dMethods);

            totalBooks = dBooks;
            totalVerses = dVerses;
            convertedBookNames = dNames;
            parseMethods.unite(dMethods);
            expectedBooks = dBooks;
            dictionaryDone = true;

            emit progress(totalBooks, qMax(1, totalBooks),
                          QString::fromUtf8("Словарь: %1 статей").arg(totalVerses));
            QCoreApplication::processEvents();
        }

        if (!dictionaryDone) {
        // Фаза 1: сразу регистрируем ВСЕ книги из INI (чтобы список не обрывался)
        const int bookQtyIni = ini.get(QStringLiteral("bookqty")).toInt();
        QVector<int> chapterHints;
        chapterHints.resize(ini.books.size());
        for (int i = 0; i < ini.books.size(); ++i) {
            const IniBook &book = ini.books[i];
            const int bookId = i + 1;
            const QString fullName = book.fullName.isEmpty()
                                         ? QString::fromUtf8("Книга %1").arg(bookId)
                                         : book.fullName;
            const QString shortName = firstShortAlias(book.shortName, fullName, bookId);
            int chHint = book.chapterQty > 0 ? book.chapterQty : 1;
            chapterHints[i] = chHint;

            insertBook.bindValue(0, bookId);
            insertBook.bindValue(1, shortName);
            insertBook.bindValue(2, fullName);
            insertBook.bindValue(3, chHint);
            if (!insertBook.exec()) {
                qWarning() << "insert book failed" << bookId << fullName << insertBook.lastError();
            }
            convertedBookNames << fullName;
            ++totalBooks;
        }

        // Фаза 2: разбираем HTML и пишем стихи
        for (int i = 0; i < ini.books.size(); ++i) {
            const IniBook &book = ini.books[i];
            const int bookId = i + 1;
            const QString htmlPath = locateBookFile(bibleQuoteModulePath, book.pathName);

            int chaptersParsed = 0;
            int bookVerses = 0;
            QString method;

            if (!htmlPath.isEmpty()) {
                parseBookFile(htmlPath, bookId, book, ini, moduleType,
                              selectVerse, insertVerse,
                              chaptersParsed, bookVerses, method);
                if (!method.isEmpty())
                    parseMethods.insert(method);
            } else {
                qWarning() << "BibleQuote: файл книги не найден:" << book.pathName
                           << book.fullName;
            }

            int actualMaxChapter = 0;
            int actualChapters = 0;
            {
                QSqlQuery cq(db);
                cq.prepare(QStringLiteral(
                    "SELECT COUNT(*), COUNT(DISTINCT chapter), COALESCE(MAX(chapter),0) "
                    "FROM verses WHERE book = ? AND length(trim(text)) > 0"));
                cq.bindValue(0, bookId);
                if (cq.exec() && cq.next()) {
                    bookVerses = cq.value(0).toInt();
                    actualChapters = cq.value(1).toInt();
                    actualMaxChapter = cq.value(2).toInt();
                }
            }

            int chaptersForMeta = actualChapters > 0 ? actualChapters : actualMaxChapter;
            if (chaptersForMeta <= 0)
                chaptersForMeta = chapterHints.value(i, 1);
            if (chaptersForMeta <= 0)
                chaptersForMeta = 1;

            // обновить число глав
            {
                QSqlQuery uq(db);
                uq.prepare(QStringLiteral("UPDATE books SET chapters = ? WHERE id = ?"));
                uq.bindValue(0, chaptersForMeta);
                uq.bindValue(1, bookId);
                uq.exec();
            }

            totalVerses += bookVerses;

            const QString fullName = convertedBookNames.value(i, book.fullName);
            emit progress(i + 1, ini.books.size(),
                          QString::fromUtf8("%1 (%2 гл., %3 ст.)")
                              .arg(fullName).arg(chaptersForMeta).arg(bookVerses));
            QCoreApplication::processEvents();
        }

        if (bookQtyIni > 0 && totalBooks < bookQtyIni) {
            qWarning() << "BibleQuote: BookQty=" << bookQtyIni
                       << "но в INI распознано книг:" << totalBooks;
        }
        }


        if (totalVerses == 0) {
            insertBook.bindValue(0, 1);
            insertBook.bindValue(1, QString::fromUtf8("Инфо"));
            insertBook.bindValue(2, QString::fromUtf8("Информация"));
            insertBook.bindValue(3, 1);
            insertBook.exec();

            insertVerse.bindValue(0, 1);
            insertVerse.bindValue(1, 1);
            insertVerse.bindValue(2, 1);
            insertVerse.bindValue(3, QString::fromUtf8(
                "Модуль «%1» распознан, но текст извлечь не удалось.").arg(result.detectedName));
            insertVerse.exec();
            totalBooks = 1;
        }

        q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_verses_book_chapter ON verses(book, chapter)"));
        q.exec(QStringLiteral("COMMIT"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    meta[QStringLiteral("books_expected")] = expectedBooks;
    meta[QStringLiteral("books_converted")] = totalBooks;
    {
        QJsonArray arr;
        for (const QString &n : convertedBookNames) arr.append(n);
        meta[QStringLiteral("book_names")] = arr;
    }
    meta[QStringLiteral("verses_converted")] = totalVerses;

    QFile metaFile(outputDir + QStringLiteral("/module.json"));
    if (metaFile.open(QIODevice::WriteOnly)) {
        metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
        metaFile.close();
    }

    result.success = (totalVerses > 0);
    result.outputPath = outputDir;
    result.booksConverted = totalBooks;
    result.versesConverted = totalVerses;
    result.booksExpected = expectedBooks;

    emit progress(totalBooks, qMax(1, expectedBooks), QString::fromUtf8("Аудит качества…"));
    QCoreApplication::processEvents();

    result.auditReport = runPostConversionAudit(outputDir, expectedBooks, convertedBookNames);
    result.auditErrors = result.auditReport.count(QString::fromUtf8("✗"));
    result.auditWarnings = result.auditReport.count(QString::fromUtf8("⚠"));

    QStringList finalMsg;
    finalMsg << QString::fromUtf8("Модуль «%1» сконвертирован.").arg(result.detectedName);
    finalMsg << QString::fromUtf8("Тип: %1, книг: %2 (в INI: %3), стихов: %4")
                    .arg(moduleType).arg(totalBooks).arg(expectedBooks).arg(totalVerses);
    if (!parseMethods.isEmpty())
        finalMsg << QString::fromUtf8("Методы разбора: %1")
                        .arg(QStringList(parseMethods.begin(), parseMethods.end())
                                 .join(QStringLiteral(", ")));
    if (!result.analysisReport.isEmpty()) {
        finalMsg << QString();
        finalMsg << result.analysisReport;
    }
    finalMsg << QString();
    finalMsg << result.auditReport;
    result.message = finalMsg.join(QLatin1Char('\n'));

    if (result.auditErrors > 0)
        result.success = false;

    return result;
}

ConversionResult Converter::convertMyBible(const QString &sqliteFilePath, const QString &outputDir)
{
    ConversionResult r;
    QFileInfo fi(sqliteFilePath);
    if (!fi.exists() || !fi.isFile()) {
        r.message = QString::fromUtf8("Файл не найден.");
        return r;
    }

    const QString srcConn = QStringLiteral("mybible_src_") + QUuid::createUuid().toString(QUuid::Id128);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), srcConn);
    db.setDatabaseName(sqliteFilePath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open()) {
        r.message = QString::fromUtf8("Не удалось открыть SQLite: %1").arg(db.lastError().text());
        QSqlDatabase::removeDatabase(srcConn);
        return r;
    }

    const QStringList tables = db.tables();
    auto hasTable = [&](const QString &n) {
        return tables.contains(n, Qt::CaseInsensitive);
    };

    // ---- info ----
    QHash<QString, QString> info;
    if (hasTable(QStringLiteral("info"))) {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT name, value FROM info"))) {
            while (q.next())
                info.insert(q.value(0).toString().toLower(), q.value(1).toString());
        }
    }

    QString modName = info.value(QStringLiteral("description"));
    if (modName.isEmpty())
        modName = info.value(QStringLiteral("module_name"));
    if (modName.isEmpty())
        modName = info.value(QStringLiteral("name"));
    if (modName.isEmpty())
        modName = fi.completeBaseName();

    QString lang = info.value(QStringLiteral("language"));
    if (lang.isEmpty())
        lang = QStringLiteral("ru");

    QString detailed = info.value(QStringLiteral("detailed_info"));
    if (detailed.isEmpty())
        detailed = modName;

    QString infoType = info.value(QStringLiteral("type")).toLower().trimmed();

    // Определяем тип по таблицам (надёжнее, чем только info.type)
    enum Kind { Bible, Dictionary, Commentary, Unknown };
    Kind kind = Unknown;
    if (hasTable(QStringLiteral("verses")) && (hasTable(QStringLiteral("books")) || hasTable(QStringLiteral("book"))))
        kind = Bible;
    else if (hasTable(QStringLiteral("dictionary")))
        kind = Dictionary;
    else if (hasTable(QStringLiteral("commentaries")) || hasTable(QStringLiteral("commentary")))
        kind = Commentary;
    else if (infoType.contains(QStringLiteral("bible")) || infoType.contains(QStringLiteral("text")))
        kind = Bible;
    else if (infoType.contains(QStringLiteral("dict")) || infoType.contains(QStringLiteral("concord")))
        kind = Dictionary;
    else if (infoType.contains(QStringLiteral("comment")))
        kind = Commentary;

    if (kind == Unknown) {
        r.message = QString::fromUtf8(
            "Неизвестный формат MyBible.\nТаблицы: %1\ninfo.type: %2")
            .arg(tables.join(QStringLiteral(", ")), infoType.isEmpty() ? QString::fromUtf8("(нет)") : infoType);
        db.close();
        QSqlDatabase::removeDatabase(srcConn);
        return r;
    }

    // ---- выходная папка ----
    QString folder = modName;
    folder.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    folder = folder.trimmed();
    if (folder.isEmpty())
        folder = QStringLiteral("mybible_module");
    QString outPath = outputDir + QLatin1Char('/') + folder;
    if (QDir(outPath).exists())
        outPath = outputDir + QLatin1Char('/') + folder + QStringLiteral("_mb");
    QDir().mkpath(outPath);

    const QString outDbPath = outPath + QStringLiteral("/data.sqlite");
    QFile::remove(outDbPath);
    const QString outConn = QStringLiteral("mybible_out_") + QUuid::createUuid().toString(QUuid::Id128);
    QSqlDatabase out = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), outConn);
    out.setDatabaseName(outDbPath);
    if (!out.open()) {
        r.message = QString::fromUtf8("Не удалось создать модуль: %1").arg(out.lastError().text());
        db.close();
        QSqlDatabase::removeDatabase(srcConn);
        QSqlDatabase::removeDatabase(outConn);
        return r;
    }

    {
        QSqlQuery oq(out);
        oq.exec(QStringLiteral(
            "CREATE TABLE books (id INTEGER PRIMARY KEY, short_name TEXT, full_name TEXT, chapters INTEGER)"));
        oq.exec(QStringLiteral(
            "CREATE TABLE verses (book INTEGER, chapter INTEGER, verse INTEGER, text TEXT, "
            "PRIMARY KEY(book, chapter, verse))"));
        oq.exec(QStringLiteral("CREATE INDEX idx_v_book ON verses(book, chapter)"));
    }

    int booksConverted = 0;
    int versesConverted = 0;
    QString outType = QStringLiteral("bible");

    // ========== BIBLE ==========
    if (kind == Bible) {
        outType = QStringLiteral("bible");
        // books: book_number, long_name / short_name (варианты схемы)
        struct BBook { int num; QString full; QString shortN; };
        QVector<BBook> bbooks;

        {
            QSqlQuery q(db);
            QString sql;
            if (hasTable(QStringLiteral("books"))) {
                // типичные колонки MyBible
                sql = QStringLiteral("SELECT book_number, long_name, short_name FROM books ORDER BY book_number");
                if (!q.exec(sql)) {
                    q = QSqlQuery(db);
                    sql = QStringLiteral("SELECT book_number, book_color, long_name, short_name FROM books ORDER BY book_number");
                    if (!q.exec(QStringLiteral("SELECT * FROM books ORDER BY book_number"))) {
                        r.message = QString::fromUtf8("Не удалось прочитать books: %1").arg(q.lastError().text());
                        out.close(); db.close();
                        QSqlDatabase::removeDatabase(outConn);
                        QSqlDatabase::removeDatabase(srcConn);
                        return r;
                    }
                    // разбор по именам колонок
                    QSqlRecord rec = q.record();
                    int iNum = rec.indexOf(QStringLiteral("book_number"));
                    if (iNum < 0) iNum = 0;
                    int iLong = rec.indexOf(QStringLiteral("long_name"));
                    int iShort = rec.indexOf(QStringLiteral("short_name"));
                    if (iLong < 0) iLong = rec.indexOf(QStringLiteral("name"));
                    while (q.next()) {
                        BBook b;
                        b.num = q.value(iNum).toInt();
                        b.full = iLong >= 0 ? q.value(iLong).toString() : QString::number(b.num);
                        b.shortN = iShort >= 0 ? q.value(iShort).toString() : b.full;
                        if (b.full.isEmpty()) b.full = QString::number(b.num);
                        if (b.shortN.isEmpty()) b.shortN = b.full;
                        if (b.num > 0)
                            bbooks.append(b);
                    }
                } else {
                    while (q.next()) {
                        BBook b;
                        b.num = q.value(0).toInt();
                        b.full = q.value(1).toString();
                        b.shortN = q.value(2).toString();
                        if (b.full.isEmpty()) b.full = QString::number(b.num);
                        if (b.shortN.isEmpty()) b.shortN = b.full;
                        if (b.num > 0)
                            bbooks.append(b);
                    }
                }
            }
        }

        // Если books пуст — собрать номера из verses
        if (bbooks.isEmpty()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT DISTINCT book_number FROM verses ORDER BY book_number"))) {
                while (q.next()) {
                    BBook b;
                    b.num = q.value(0).toInt();
                    b.full = QString::fromUtf8("Книга %1").arg(b.num);
                    b.shortN = QString::number(b.num);
                    bbooks.append(b);
                }
            }
        }

        if (bbooks.isEmpty()) {
            r.message = QString::fromUtf8("В модуле нет книг (books/verses).");
            out.close(); db.close();
            QSqlDatabase::removeDatabase(outConn);
            QSqlDatabase::removeDatabase(srcConn);
            return r;
        }

        // Сохраняем book_number MyBible как id — для ссылок B:N
        out.transaction();
        int bi = 0;
        for (const BBook &b : bbooks) {
            ++bi;
            // chapters count later
            QSqlQuery oq(out);
            oq.prepare(QStringLiteral(
                "INSERT INTO books (id, short_name, full_name, chapters) VALUES (?, ?, ?, 0)"));
            oq.addBindValue(b.num);
            oq.addBindValue(b.shortN);
            oq.addBindValue(b.full);
            oq.exec();
            emit progress(bi, bbooks.size(), QString::fromUtf8("Книга: %1").arg(b.full));
        }

        // verses
        {
            QSqlQuery q(db);
            // варианты: book_number, chapter, verse, text
            if (!q.exec(QStringLiteral(
                    "SELECT book_number, chapter, verse, text FROM verses "
                    "ORDER BY book_number, chapter, verse"))) {
                r.message = QString::fromUtf8("Не удалось прочитать verses: %1").arg(q.lastError().text());
                out.rollback();
                out.close(); db.close();
                QSqlDatabase::removeDatabase(outConn);
                QSqlDatabase::removeDatabase(srcConn);
                return r;
            }
            QSqlQuery ins(out);
            ins.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO verses (book, chapter, verse, text) VALUES (?, ?, ?, ?)"));
            int n = 0;
            while (q.next()) {
                ins.addBindValue(q.value(0).toInt());
                ins.addBindValue(q.value(1).toInt());
                ins.addBindValue(q.value(2).toInt());
                QString vtext = q.value(3).toString();
                if (vtext.contains(QLatin1Char('<')))
                    vtext = normalizeRedLetterHtml(vtext);
                ins.addBindValue(vtext);
                ins.exec();
                ++n;
                if (n % 2000 == 0) {
                    emit progress(n, n + 1, QString::fromUtf8("Стихов: %1…").arg(n));
                }
            }
            versesConverted = n;
        }

        // обновить chapters
        for (const BBook &b : bbooks) {
            QSqlQuery q(out);
            q.prepare(QStringLiteral("SELECT COUNT(DISTINCT chapter) FROM verses WHERE book = ?"));
            q.addBindValue(b.num);
            int ch = 0;
            if (q.exec() && q.next())
                ch = q.value(0).toInt();
            QSqlQuery u(out);
            u.prepare(QStringLiteral("UPDATE books SET chapters = ? WHERE id = ?"));
            u.addBindValue(ch);
            u.addBindValue(b.num);
            u.exec();
        }
        out.commit();
        booksConverted = bbooks.size();
    }

    // ========== DICTIONARY / CONCORDANCE ==========
    else if (kind == Dictionary) {
        outType = QStringLiteral("dictionary");
        struct Entry { QString topic; QString def; };
        QVector<Entry> entries;
        {
            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT topic, definition FROM dictionary ORDER BY topic"))) {
                r.message = QString::fromUtf8("Ошибка dictionary: %1").arg(q.lastError().text());
                out.close(); db.close();
                QSqlDatabase::removeDatabase(outConn);
                QSqlDatabase::removeDatabase(srcConn);
                return r;
            }
            while (q.next()) {
                Entry e;
                e.topic = q.value(0).toString().trimmed();
                e.def = q.value(1).toString();
                if (!e.topic.isEmpty())
                    entries.append(e);
            }
        }
        if (entries.isEmpty()) {
            r.message = QString::fromUtf8("dictionary пуста.");
            out.close(); db.close();
            QSqlDatabase::removeDatabase(outConn);
            QSqlDatabase::removeDatabase(srcConn);
            return r;
        }

        QMap<QString, QVector<int>> groups;
        for (int i = 0; i < entries.size(); ++i) {
            QString letter = entries[i].topic.left(1).toUpper();
            if (letter.isEmpty())
                letter = QStringLiteral("#");
            else if (letter.at(0).isDigit())
                letter = QStringLiteral("0-9");
            groups[letter].append(i);
        }

        out.transaction();
        int bookId = 0;
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            ++bookId;
            const QString letter = it.key();
            QSqlQuery oq(out);
            oq.prepare(QStringLiteral(
                "INSERT INTO books (id, short_name, full_name, chapters) VALUES (?, ?, ?, 1)"));
            oq.addBindValue(bookId);
            oq.addBindValue(letter);
            oq.addBindValue(letter);
            oq.exec();

            QSqlQuery ins(out);
            ins.prepare(QStringLiteral(
                "INSERT INTO verses (book, chapter, verse, text) VALUES (?, 1, ?, ?)"));
            int verse = 0;
            for (int idx : it.value()) {
                ++verse;
                const Entry &e = entries[idx];
                const QString html = QStringLiteral("<b style='font-size:1.15em'>%1</b><br/>%2")
                                         .arg(e.topic.toHtmlEscaped(), e.def);
                ins.addBindValue(bookId);
                ins.addBindValue(verse);
                ins.addBindValue(html);
                ins.exec();
                ins.finish();
                ++versesConverted;
            }
            emit progress(bookId, groups.size(),
                          QString::fromUtf8("Буква %1 — %2").arg(letter).arg(it.value().size()));
        }
        out.commit();
        booksConverted = groups.size();
    }

    // ========== COMMENTARY / FOOTNOTES ==========
    else if (kind == Commentary) {
        // Сноски / критический аппарат (MyBible is_footnotes)
        const QString isFootFlag = info.value(QStringLiteral("is_footnotes")).toLower();
        const bool isFootnotes =
            isFootFlag == QLatin1String("true") || isFootFlag == QLatin1String("1")
            || infoType.contains(QLatin1String("footnote"))
            || modName.contains(QString::fromUtf8("сноск"), Qt::CaseInsensitive)
            || modName.contains(QString::fromUtf8("аппарат"), Qt::CaseInsensitive)
            || modName.contains(QString::fromUtf8("критическ"), Qt::CaseInsensitive)
            || detailed.contains(QString::fromUtf8("критический аппарат"), Qt::CaseInsensitive)
            || detailed.contains(QString::fromUtf8("is_footnotes"), Qt::CaseInsensitive);
        outType = isFootnotes ? QStringLiteral("footnotes")
                              : QStringLiteral("commentary");
        const QString ctable = hasTable(QStringLiteral("commentaries"))
                                   ? QStringLiteral("commentaries")
                                   : QStringLiteral("commentary");

        // Собираем книги из комментариев
        QMap<int, QString> bookNames;
        {
            QSqlQuery q(db);
            if (hasTable(QStringLiteral("books"))) {
                if (q.exec(QStringLiteral("SELECT book_number, long_name, short_name FROM books ORDER BY book_number"))) {
                    while (q.next()) {
                        QString n = q.value(1).toString();
                        if (n.isEmpty()) n = q.value(2).toString();
                        bookNames.insert(q.value(0).toInt(), n);
                    }
                }
            }
        }

        QSqlQuery q(db);
        // типичная схема MyBible commentaries
        QString sql = QStringLiteral(
            "SELECT book_number, chapter_number_from, verse_number_from, "
            "chapter_number_to, verse_number_to, data FROM %1 "
            "ORDER BY book_number, chapter_number_from, verse_number_from").arg(ctable);
        if (!q.exec(sql)) {
            // упрощённый вариант
            sql = QStringLiteral("SELECT * FROM %1").arg(ctable);
            if (!q.exec(sql)) {
                r.message = QString::fromUtf8("Не удалось прочитать %1: %2")
                                .arg(ctable, q.lastError().text());
                out.close(); db.close();
                QSqlDatabase::removeDatabase(outConn);
                QSqlDatabase::removeDatabase(srcConn);
                return r;
            }
        }

        QSqlRecord rec = q.record();
        auto col = [&](const char *name) { return rec.indexOf(QLatin1String(name)); };
        int iBook = col("book_number");
        int iChF = col("chapter_number_from");
        int iVF = col("verse_number_from");
        int iChT = col("chapter_number_to");
        int iVT = col("verse_number_to");
        int iData = col("data");
        if (iData < 0) iData = col("text");
        int iMarker = col("marker");
        if (iBook < 0) iBook = 0;
        if (iChF < 0) iChF = 1;
        if (iVF < 0) iVF = 2;
        if (iData < 0) iData = rec.count() - 1;

        struct CRow {
            int book, ch, vs;
            QString text;
        };
        QVector<CRow> rows;
        QSet<int> bookSet;
        while (q.next()) {
            CRow cr;
            cr.book = q.value(iBook).toInt();
            cr.ch = iChF >= 0 ? q.value(iChF).toInt() : 1;
            cr.vs = iVF >= 0 ? q.value(iVF).toInt() : 1;
            if (cr.ch <= 0) cr.ch = 1;
            if (cr.vs <= 0) cr.vs = 1;
            cr.text = q.value(iData).toString();
            if (cr.text.trimmed().isEmpty())
                continue;
            // Маркер сноски MyBible: [1], [t], [4]…
            if (iMarker >= 0) {
                QString mk = q.value(iMarker).toString().trimmed();
                if (mk.startsWith(QLatin1Char('[')) && mk.endsWith(QLatin1Char(']')))
                    mk = mk.mid(1, mk.size() - 2).trimmed();
                if (!mk.isEmpty()) {
                    cr.text = QStringLiteral(
                        "<div class='fn' data-marker='%1'>"
                        "<b style='color:#6a4a20'>[%1]</b> %2</div>")
                                  .arg(mk.toHtmlEscaped(), cr.text);
                }
            }
            // если диапазон — пометить в тексте
            if (iChT >= 0 && iVT >= 0) {
                const int chT = q.value(iChT).toInt();
                const int vT = q.value(iVT).toInt();
                if ((chT > cr.ch || (chT == cr.ch && vT > cr.vs)) && chT > 0) {
                    cr.text = QStringLiteral("<i>%1:%2–%3:%4</i><br/>%5")
                                  .arg(cr.ch).arg(cr.vs).arg(chT).arg(vT).arg(cr.text);
                }
            }
            rows.append(cr);
            bookSet.insert(cr.book);
        }

        if (rows.isEmpty()) {
            r.message = QString::fromUtf8("Комментарии пусты.");
            out.close(); db.close();
            QSqlDatabase::removeDatabase(outConn);
            QSqlDatabase::removeDatabase(srcConn);
            return r;
        }

        out.transaction();
        QList<int> sortedBooks = bookSet.values();
        std::sort(sortedBooks.begin(), sortedBooks.end());
        int bi = 0;
        for (int bn : sortedBooks) {
            ++bi;
            QString name = bookNames.value(bn, QString::fromUtf8("Книга %1").arg(bn));
            QSqlQuery oq(out);
            oq.prepare(QStringLiteral(
                "INSERT INTO books (id, short_name, full_name, chapters) VALUES (?, ?, ?, 0)"));
            oq.addBindValue(bn);
            oq.addBindValue(name);
            oq.addBindValue(name);
            oq.exec();
            emit progress(bi, sortedBooks.size(), name);
        }

        // стихи: при конфликте PK склеиваем
        QHash<QString, QString> merged;
        for (const CRow &cr : rows) {
            const QString key = QStringLiteral("%1:%2:%3").arg(cr.book).arg(cr.ch).arg(cr.vs);
            if (merged.contains(key))
                merged[key] += QStringLiteral("<hr/>") + cr.text;
            else
                merged.insert(key, cr.text);
        }
        QSqlQuery ins(out);
        ins.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO verses (book, chapter, verse, text) VALUES (?, ?, ?, ?)"));
        for (auto it = merged.begin(); it != merged.end(); ++it) {
            const QStringList p = it.key().split(QLatin1Char(':'));
            ins.addBindValue(p[0].toInt());
            ins.addBindValue(p[1].toInt());
            ins.addBindValue(p[2].toInt());
            ins.addBindValue(it.value());
            ins.exec();
            ins.finish();
            ++versesConverted;
        }
        for (int bn : sortedBooks) {
            QSqlQuery q2(out);
            q2.prepare(QStringLiteral("SELECT COUNT(DISTINCT chapter) FROM verses WHERE book = ?"));
            q2.addBindValue(bn);
            int ch = 0;
            if (q2.exec() && q2.next()) ch = q2.value(0).toInt();
            QSqlQuery u(out);
            u.prepare(QStringLiteral("UPDATE books SET chapters = ? WHERE id = ?"));
            u.addBindValue(ch);
            u.addBindValue(bn);
            u.exec();
        }
        out.commit();
        booksConverted = sortedBooks.size();
    }

    // module.json
    QJsonObject meta;
    meta.insert(QStringLiteral("name"), modName);
    meta.insert(QStringLiteral("description"), detailed);
    meta.insert(QStringLiteral("type"), outType);
    meta.insert(QStringLiteral("language"), lang);
    meta.insert(QStringLiteral("source"), QStringLiteral("mybible"));
    {
        QFile mf(outPath + QStringLiteral("/module.json"));
        if (mf.open(QIODevice::WriteOnly)) {
            mf.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
            mf.close();
        }
    }

    out.close();
    db.close();
    QSqlDatabase::removeDatabase(outConn);
    QSqlDatabase::removeDatabase(srcConn);

    if (versesConverted <= 0 && booksConverted <= 0) {
        r.message = QString::fromUtf8("Не удалось извлечь данные из модуля MyBible.");
        return r;
    }

    r.success = true;
    r.outputPath = outPath;
    r.detectedName = modName;
    r.detectedType = outType;
    r.booksConverted = booksConverted;
    r.versesConverted = versesConverted;
    r.message = QString::fromUtf8("MyBible (%1): книг %2, записей %3.")
                    .arg(outType).arg(booksConverted).arg(versesConverted);
    return r;
}
