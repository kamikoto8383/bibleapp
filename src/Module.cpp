#include <algorithm>
#include "Module.h"
#include <QRegularExpression>
#include <QSqlQuery>
#include <QHash>
#include <QCoreApplication>
#include <QFile>
#include <QSet>
#include <QSqlError>
#include <QSqlRecord>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QUuid>
#include <QDebug>
#include <QStandardPaths>

Module::Module(const QString &path)
    : m_path(path)
{
    m_connectionName = QString("mod_") + QUuid::createUuid().toString(QUuid::Id128);
    m_valid = load();
}

bool Module::load()
{
    QFile metaFile(m_path + "/module.json");
    if (!metaFile.exists() || !metaFile.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll(), &err);
    metaFile.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    QJsonObject obj = doc.object();
    m_name = obj.value("name").toString(QString::fromUtf8("Без названия"));
    m_description = obj.value("description").toString();
    m_type = obj.value("type").toString("bible");
    m_language = obj.value("language").toString("ru");

    if (!openDatabase())
        return false;

    QSqlQuery query(m_db);
    if (!query.exec("SELECT id, short_name, full_name, chapters FROM books ORDER BY id")) {
        qWarning() << "Failed to load books:" << query.lastError();
        return false;
    }

    m_books.clear();
    while (query.next()) {
        BookInfo book;
        book.id = query.value(0).toInt();
        book.shortName = query.value(1).toString();
        book.fullName = query.value(2).toString();
        book.chapters = query.value(3).toInt();
        m_books.append(book);
    }

    return !m_books.isEmpty();
}




QString Module::bookShortNameById(int id) const
{
    for (const auto &b : m_books) {
        if (b.id == id)
            return b.shortName;
    }
    return {};
}

static QString normalizeBookKey(QString s)
{
    s = s.toLower().trimmed().simplified();
    s.replace(QString::fromUtf8("ё"), QString::fromUtf8("е"));
    s.remove(QLatin1Char('.'));
    s.remove(QString::fromUtf8("«"));
    s.remove(QString::fromUtf8("»"));
    s.remove(QLatin1Char('('));
    s.remove(QLatin1Char(')'));
    const QStringList strip = {
        QString::fromUtf8("книга "), QString::fromUtf8("книги "),
        QString::fromUtf8("от "), QString::fromUtf8("св "),
        QString::fromUtf8("святого "), QString::fromUtf8("пророка "),
        QString::fromUtf8("апостола "),
    };
    for (const QString &p : strip) {
        if (s.startsWith(p))
            s = s.mid(p.size()).trimmed();
    }
    // Псалмы
    if (s.startsWith(QString::fromUtf8("псалом")) || s.startsWith(QString::fromUtf8("псалм"))
        || s == QString::fromUtf8("пс") || s.startsWith(QString::fromUtf8("пс ")))
        return QString::fromUtf8("псалтирь");
    return s;
}

/** Канонический ключ для сопоставления русских названий книг */
static QString canonicalBookKey(const QString &shortName, const QString &fullName)
{
    const QString a = normalizeBookKey(shortName);
    const QString b = normalizeBookKey(fullName);

    struct Alias { const char *key; const char *canon; };
    static const Alias aliases[] = {
        {"быт", "бытие"}, {"бытие", "бытие"},
        {"исх", "исход"}, {"исход", "исход"},
        {"лев", "левит"}, {"левит", "левит"},
        {"чис", "числа"}, {"числа", "числа"},
        {"втор", "второзаконие"}, {"второзаконие", "второзаконие"},
        {"нав", "исус навин"}, {"исус навин", "исус навин"}, {"иис нав", "исус навин"},
        {"суд", "судьи"}, {"судьи", "судьи"},
        {"руф", "руфь"}, {"руфь", "руфь"},
        {"1 цар", "1 царств"}, {"1цар", "1 царств"}, {"1 царств", "1 царств"},
        {"2 цар", "2 царств"}, {"2цар", "2 царств"}, {"2 царств", "2 царств"},
        {"3 цар", "3 царств"}, {"3цар", "3 царств"}, {"3 царств", "3 царств"},
        {"4 цар", "4 царств"}, {"4цар", "4 царств"}, {"4 царств", "4 царств"},
        {"1 пар", "1 паралипоменон"}, {"1пар", "1 паралипоменон"},
        {"2 пар", "2 паралипоменон"}, {"2пар", "2 паралипоменон"},
        {"езд", "ездра"}, {"ездра", "ездра"},
        {"неем", "неемия"}, {"неемия", "неемия"},
        {"есф", "есфирь"}, {"есфирь", "есфирь"},
        {"иов", "иов"},
        {"псалтирь", "псалтирь"}, {"пс", "псалтирь"},
        {"прит", "притчи"}, {"притчи", "притчи"},
        {"еккл", "екклесиаст"}, {"екклесиаст", "екклесиаст"},
        {"песн", "песнь песней"}, {"песнь песней", "песнь песней"},
        {"ис", "исаия"}, {"исаия", "исаия"}, {"исаии", "исаия"},
        {"иер", "иеремия"}, {"иеремия", "иеремия"},
        {"плач", "плач иеремии"}, {"плач иеремии", "плач иеремии"},
        {"иез", "иезекииль"}, {"иезекииль", "иезекииль"},
        {"дан", "даниил"}, {"даниил", "даниил"},
        {"ос", "осия"}, {"осия", "осия"},
        {"иоил", "иоиль"}, {"иоиль", "иоиль"},
        {"ам", "амос"}, {"амос", "амос"},
        {"авд", "авдий"}, {"авдий", "авдий"},
        {"ион", "иона"}, {"иона", "иона"},
        {"мих", "михей"}, {"михей", "михей"},
        {"наум", "наум"},
        {"авв", "аввакум"}, {"аввакум", "аввакум"},
        {"соф", "софония"}, {"софония", "софония"},
        {"агг", "аггей"}, {"аггей", "аггей"},
        {"зах", "захария"}, {"захария", "захария"},
        {"мал", "малахия"}, {"малахия", "малахия"},
        {"мф", "матфея"}, {"матфея", "матфея"}, {"матфей", "матфея"}, {"от матфея", "матфея"},
        {"мк", "марка"}, {"марка", "марка"}, {"марк", "марка"}, {"от марка", "марка"},
        {"лк", "луки"}, {"луки", "луки"}, {"лука", "луки"}, {"от луки", "луки"},
        // Евангелие от Иоанна — отдельный ключ от 1/2/3 Иоанна
        {"ин", "от иоанна"}, {"от иоанна", "от иоанна"}, {"евангелие от иоанна", "от иоанна"},
        {"деян", "деяния"}, {"деяния", "деяния"},
        {"рим", "римлянам"}, {"римлянам", "римлянам"},
        {"1 кор", "1 коринфянам"}, {"1кор", "1 коринфянам"},
        {"2 кор", "2 коринфянам"}, {"2кор", "2 коринфянам"},
        {"гал", "галатам"}, {"галатам", "галатам"},
        {"еф", "ефесянам"}, {"ефесянам", "ефесянам"},
        {"флп", "филиппийцам"}, {"филиппийцам", "филиппийцам"},
        {"кол", "колоссянам"}, {"колоссянам", "колоссянам"},
        {"1 фес", "1 фессалоникийцам"}, {"1фес", "1 фессалоникийцам"},
        {"2 фес", "2 фессалоникийцам"}, {"2фес", "2 фессалоникийцам"},
        {"1 тим", "1 тимофею"}, {"1тим", "1 тимофею"},
        {"2 тим", "2 тимофею"}, {"2тим", "2 тимофею"},
        {"тит", "титу"}, {"титу", "титу"},
        {"флм", "филимону"}, {"филимону", "филимону"},
        {"евр", "евреям"}, {"евреям", "евреям"},
        {"иак", "иакова"}, {"иакова", "иакова"},
        {"1 пет", "1 петра"}, {"1пет", "1 петра"}, {"1 петра", "1 петра"},
        {"2 пет", "2 петра"}, {"2пет", "2 петра"}, {"2 петра", "2 петра"},
        {"1 ин", "1 иоанна"}, {"1ин", "1 иоанна"}, {"1 иоанна", "1 иоанна"},
        {"1-е иоанна", "1 иоанна"}, {"1е иоанна", "1 иоанна"}, {"1-oe иоанна", "1 иоанна"},
        {"2 ин", "2 иоанна"}, {"2ин", "2 иоанна"}, {"2 иоанна", "2 иоанна"},
        {"2-е иоанна", "2 иоанна"}, {"2е иоанна", "2 иоанна"},
        {"3 ин", "3 иоанна"}, {"3ин", "3 иоанна"}, {"3 иоанна", "3 иоанна"},
        {"3-е иоанна", "3 иоанна"}, {"3е иоанна", "3 иоанна"},
        {"иуд", "иуды"}, {"иуды", "иуды"},
        {"откр", "откровение"}, {"откровение", "откровение"}, {"апокалипсис", "откровение"},
    };

    auto prepare = [](QString k) {
        k.replace(QString::fromUtf8("-е"), QStringLiteral(" "));
        k.replace(QString::fromUtf8("-oe"), QStringLiteral(" "));
        k.replace(QString::fromUtf8("е "), QStringLiteral(" "));
        return k.simplified();
    };

    auto lookup = [&](const QString &raw) -> QString {
        if (raw.isEmpty())
            return {};
        const QString key = prepare(raw);

        // 1) Точное совпадение, приоритет более длинному ключу
        QString best;
        int bestLen = -1;
        for (const auto &al : aliases) {
            const QString ck = QString::fromUtf8(al.key);
            if (key == ck && ck.size() > bestLen) {
                bestLen = ck.size();
                best = QString::fromUtf8(al.canon);
            }
        }
        if (!best.isEmpty())
            return best;

        // 2) startsWith "1 иоанна ..."
        for (const auto &al : aliases) {
            const QString ck = QString::fromUtf8(al.key);
            if (ck.size() >= 3 && key.startsWith(ck + QLatin1Char(' ')))
                return QString::fromUtf8(al.canon);
        }

        // 3) Голое «иоанна» без цифры в начале = Евангелие
        if (key == QString::fromUtf8("иоанна"))
            return QString::fromUtf8("от иоанна");

        return key;
    };

    // Нумерованные послания: смотрим short и full, предпочитаем то, что начинается с цифры
    const bool bNumbered = !b.isEmpty() && b.at(0).isDigit();
    const bool aNumbered = !a.isEmpty() && a.at(0).isDigit();
    if (bNumbered)
        return lookup(b);
    if (aNumbered)
        return lookup(a);

    QString cb = lookup(b);
    QString ca = lookup(a);
    if (!b.isEmpty() && cb != b)
        return cb;
    if (!a.isEmpty() && ca != a)
        return ca;
    if (!cb.isEmpty())
        return cb;
    if (!ca.isEmpty())
        return ca;
    return !b.isEmpty() ? b : a;
}

int Module::findBookIdMatching(const Module *other, int otherBookId) const
{
    if (!other || otherBookId < 1 || m_books.isEmpty())
        return -1;

    QString shortN, fullN;
    for (const auto &b : other->books()) {
        if (b.id == otherBookId) {
            shortN = b.shortName;
            fullN = b.fullName;
            break;
        }
    }
    if (shortN.isEmpty() && fullN.isEmpty())
        return -1;

    const QString want = canonicalBookKey(shortN, fullN);
    if (want.isEmpty())
        return -1;

    // Только точное каноническое совпадение
    for (const auto &b : m_books) {
        const QString got = canonicalBookKey(b.shortName, b.fullName);
        if (got == want)
            return b.id;
    }

    // Точное совпадение нормализованных имён (без contains)
    const QString wShort = normalizeBookKey(shortN);
    const QString wFull = normalizeBookKey(fullN);
    for (const auto &b : m_books) {
        const QString bs = normalizeBookKey(b.shortName);
        const QString bf = normalizeBookKey(b.fullName);
        if ((!wShort.isEmpty() && (bs == wShort || bf == wShort))
            || (!wFull.isEmpty() && (bs == wFull || bf == wFull)))
            return b.id;
    }

    return -1;
}

bool Module::isBibleText() const
{
    if (!m_valid)
        return false;
    const QString t = m_type.toLower().trimmed();
    if (t.isEmpty() || t == QLatin1String("bible") || t == QLatin1String("text")
        || t == QLatin1String("interlinear") || t == QLatin1String("biblequote"))
        return true;
    if (t.contains(QLatin1String("comment")) || t.contains(QLatin1String("коммент"))
        || t.contains(QLatin1String("dict")) || t.contains(QLatin1String("lexicon"))
        || t.contains(QLatin1String("словарь")) || t.contains(QLatin1String("notes")))
        return false;
    // по умолчанию считаем библейским текстом, если есть книги/главы
    return true;
}


bool Module::isDictionary() const
{
    if (!m_valid)
        return false;
    const QString t = m_type.toLower().trimmed();
    return t.contains(QLatin1String("dict"))
        || t.contains(QLatin1String("lexicon"))
        || t.contains(QString::fromUtf8("слов"))
        || t.contains(QLatin1String("concord"))
        || t.contains(QString::fromUtf8("симфон"))
        || t.contains(QString::fromUtf8("справ"));
}

bool Module::isCommentary() const
{
    if (!m_valid)
        return false;
    const QString t = m_type.toLower().trimmed();
    return t.contains(QLatin1String("comment"))
        || t.contains(QString::fromUtf8("коммент"))
        || t.contains(QLatin1String("notes"))
        || t.contains(QString::fromUtf8("толкован"));
}

bool Module::openDatabase() const
{
    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase::database(m_connectionName);
        return m_db.isOpen();
    }

    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        qWarning() << "QSQLITE driver not available";
        return false;
    }

    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(m_path + "/data.sqlite");
    if (!m_db.open()) {
        qWarning() << "Cannot open module database:" << m_db.lastError();
        return false;
    }
    return true;
}

QStringList Module::bookNames() const
{
    QStringList names;
    for (const auto &book : m_books)
        names << book.fullName;
    return names;
}

QString Module::getVerseText(int book, int chapter, int verse) const
{
    if (!openDatabase())
        return {};

    QSqlQuery query(m_db);
    query.prepare("SELECT text FROM verses WHERE book = ? AND chapter = ? AND verse = ?");
    query.addBindValue(book);
    query.addBindValue(chapter);
    query.addBindValue(verse);

    if (query.exec() && query.next())
        return query.value(0).toString();
    return {};
}



static QString greekLexiconPath()
{
    // 1) рядом с бинарником /share
    QStringList candidates;
    candidates << QCoreApplication::applicationDirPath() + "/greek_lexicon.sqlite";
    candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/greek_lexicon.sqlite";
    candidates << "/usr/share/bibleapp/greek_lexicon.sqlite";
    candidates << QStandardPaths::locate(QStandardPaths::AppDataLocation, "greek_lexicon.sqlite");
    for (const QString &p : candidates) {
        if (!p.isEmpty() && QFile::exists(p))
            return p;
    }
    return {};
}

static QString lookupFullLexicon(const QString &word)
{
    static bool tried = false;
    static QSqlDatabase db;
    static QString conn = QStringLiteral("greek_lex_conn");

    if (!tried) {
        tried = true;
        QString path = greekLexiconPath();
        if (path.isEmpty() || !QSqlDatabase::isDriverAvailable("QSQLITE"))
            return {};
        if (QSqlDatabase::contains(conn))
            db = QSqlDatabase::database(conn);
        else {
            db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(path);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (!db.open())
                return {};
        }
    }
    if (!db.isOpen())
        return {};

    QString w = word;
    QSqlQuery q(db);
    q.prepare("SELECT gloss FROM greek WHERE lemma = ? LIMIT 1");
    q.addBindValue(w);
    if (q.exec() && q.next())
        return q.value(0).toString();

    // нормализация без диакритики
    QString folded;
    for (QChar ch : w) {
        // упрощённо: оставляем буквы
        if (ch.isLetter())
            folded.append(ch.toLower());
    }
    q.prepare("SELECT gloss FROM greek WHERE lemma_norm = ? LIMIT 1");
    q.addBindValue(folded);
    if (q.exec() && q.next())
        return q.value(0).toString();

    return {};
}


static QString translateGreekWord(const QString &word)
{
    static QHash<QString, QString> ru;
    if (ru.isEmpty()) {
        auto add = [&](const char *g, const char *r) {
            ru.insert(QString::fromUtf8(g), QString::fromUtf8(r));
        };
        add("ὁ", "арт."); add("ἡ", "арт."); add("τό", "арт."); add("τὸ", "арт.");
        add("καί", "и"); add("καὶ", "и"); add("δέ", "же"); add("δὲ", "же");
        add("γάρ", "ибо"); add("γὰρ", "ибо"); add("ἀλλά", "но"); add("ἀλλὰ", "но"); add("ἀλλ’", "но");
        add("ὅτι", "что, ибо"); add("ἵνα", "чтобы"); add("οὐ", "не"); add("οὐκ", "не"); add("μή", "не"); add("μὴ", "не");
        add("ἐν", "в"); add("Ἐν", "в"); add("εἰς", "в, к"); add("ἐκ", "из"); add("πρὸς", "к, у"); add("πρός", "к, у");
        add("διά", "через"); add("διὰ", "через"); add("δι’", "через"); add("ἀπό", "от"); add("ἀπὸ", "от");
        add("θεός", "Бог"); add("θεὸς", "Бог"); add("θεοῦ", "Бога"); add("θεόν", "Бога"); add("θεὸν", "Бога");
        add("λόγος", "слово"); add("Ἰησοῦς", "Иисус"); add("Χριστός", "Христос"); add("Χριστοῦ", "Христа");
        add("πνεῦμα", "дух"); add("υἱός", "сын"); add("πατήρ", "отец"); add("πατρός", "отца");
        add("ἄνθρωπος", "человек"); add("κόσμος", "мир"); add("ζωή", "жизнь"); add("ζωὴ", "жизнь");
        add("φῶς", "свет"); add("σκοτία", "тьма"); add("ἀρχῇ", "начале"); add("ἀρχή", "начало");
        add("ἦν", "было"); add("ἐγένετο", "стало"); add("γέγονεν", "стало");
        add("αὐτοῦ", "его"); add("αὐτόν", "его"); add("αὐτὸν", "его"); add("αὐτῶν", "их");
        add("οὗτος", "сей"); add("πάντα", "всё"); add("χωρὶς", "без"); add("οὐδὲ", "ни");
        add("Ἰωάννης", "Иоанн"); add("μαρτυρίαν", "свидетельство"); add("φωτίζει", "просвещает");
        add("δόξαν", "славу"); add("χάριτος", "благодати"); add("ἀληθείας", "истины");
        add("μονογενὴς", "единородный"); add("ἐξηγήσατο", "явил");
    }

    QString w = word.trimmed();
    while (!w.isEmpty() && !w[0].isLetter())
        w = w.mid(1);
    while (!w.isEmpty() && !w.back().isLetter())
        w.chop(1);
    if (w.isEmpty())
        return {};

    if (ru.contains(w))
        return ru.value(w);

    for (auto it = ru.constBegin(); it != ru.constEnd(); ++it) {
        if (it.key().compare(w, Qt::CaseInsensitive) == 0)
            return it.value();
    }

    QString full = lookupFullLexicon(w);
    if (!full.isEmpty())
        return full;

    return QString::fromUtf8("—");
}

static QString annotateGreek(const QString &greek)
{
    // Формат подстрочника: ORIG¦GLOSS¦STRONG  (слова через пробел)
    QStringList parts = greek.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList out;
    bool interlinear = false;
    for (const QString &p : parts) {
        if (p.contains(QChar(0x00A6)) || p.contains(QLatin1Char('|'))) {
            interlinear = true;
            break;
        }
    }

    if (interlinear) {
        for (const QString &rawPart : parts) {
            QStringList fields = rawPart.split(QChar(0x00A6));
            if (fields.size() < 2)
                fields = rawPart.split(QLatin1Char('|'));
            if (fields.size() < 2) {
                out << rawPart.toHtmlEscaped() + " ";
                continue;
            }
            QString orig = fields.value(0).trimmed();
            QString gloss = fields.value(1).trimmed();
            QString strong = fields.value(2).trimmed().toUpper();

            if (gloss.isEmpty() && !strong.isEmpty())
                gloss = Module::lookupStrongGloss(strong);
            if (gloss.isEmpty()) {
                gloss = translateGreekWord(orig);
                if (gloss == QString::fromUtf8("—"))
                    gloss.clear();
            }

            QUrl href;
            href.setScheme(QStringLiteral("w"));
            href.setPath(QStringLiteral("/"));
            QUrlQuery uq;
            uq.addQueryItem(QStringLiteral("g"), orig);
            if (!gloss.isEmpty())
                uq.addQueryItem(QStringLiteral("m"), gloss);
            if (!strong.isEmpty())
                uq.addQueryItem(QStringLiteral("s"), strong);
            href.setQuery(uq);

            QString title = strong.isEmpty() ? gloss : (strong + " — " + gloss);
            out << QString(
                "<span style=\"display:inline-block; text-align:center; margin:0 6px 8px 0; vertical-align:top;\">"
                "<a href=\"%1\" title=\"%2\" style=\"color:#2d6a4f; text-decoration:none;\">"
                "<span style=\"display:block; font-size:11px; color:#5a4a3a; line-height:1.2;\">%3</span>"
                "<span style=\"display:block; font-size:16px; line-height:1.3;\">%4</span>"
                "<span style=\"display:block; font-size:10px; color:#887; line-height:1.1;\">%5</span>"
                "</a></span>"
            ).arg(href.toString(QUrl::FullyEncoded),
                  title.toHtmlEscaped(),
                  gloss.isEmpty() ? QString::fromUtf8("—") : gloss.toHtmlEscaped(),
                  orig.toHtmlEscaped(),
                  strong.toHtmlEscaped());
        }
        return out.join("");
    }

    for (const QString &rawPart : parts) {
        QString part = rawPart;
        QString trail;
        while (!part.isEmpty() && !part.back().isLetter()) {
            trail.prepend(part.back());
            part.chop(1);
        }
        QString lead;
        while (!part.isEmpty() && !part[0].isLetter()) {
            lead.append(part[0]);
            part = part.mid(1);
        }
        if (part.isEmpty()) {
            out << rawPart.toHtmlEscaped() + " ";
            continue;
        }
        QString meaning = translateGreekWord(part);
        if (meaning == QString::fromUtf8("—"))
            meaning.clear();
        QUrl href;
        href.setScheme(QStringLiteral("w"));
        href.setPath(QStringLiteral("/"));
        QUrlQuery uq;
        uq.addQueryItem(QStringLiteral("g"), part);
        if (!meaning.isEmpty())
            uq.addQueryItem(QStringLiteral("m"), meaning);
        href.setQuery(uq);
        QString safe = part.toHtmlEscaped();
        QString glossHtml = meaning.isEmpty()
            ? QString("<span style=\"color:#aaa; font-size:small;\"> —</span>")
            : QString("<span style=\"color:#555; font-size:small;\"> %1</span>").arg(meaning.toHtmlEscaped());
        out << QString(
            "%1<a href=\"%2\" title=\"%3\" style=\"color:#2d6a4f; text-decoration:none;\">"
            "<span style=\"white-space:nowrap;\">%4%5</span></a>%6 "
        ).arg(lead.toHtmlEscaped(),
              href.toString(QUrl::FullyEncoded),
              meaning.isEmpty() ? QString::fromUtf8("нет в словаре") : meaning.toHtmlEscaped(),
              safe, glossHtml, trail.toHtmlEscaped());
    }
    return out.join("");
}

static QString highlightSearchTerms(const QString &escapedText, const QString &query)
{
    if (query.trimmed().isEmpty() || escapedText.isEmpty())
        return escapedText;

    struct Span { int start; int end; };
    QList<Span> spans;

    auto addMatches = [&](const QString &term) {
        if (term.isEmpty())
            return;
        QRegularExpression re(QRegularExpression::escape(term),
                              QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(escapedText);
        while (it.hasNext()) {
            auto m = it.next();
            spans.append({(int)m.capturedStart(), (int)m.capturedEnd()});
        }
    };

    const QString q = query.trimmed().simplified();
    const QString phrase = q.toHtmlEscaped();

    // 1) Целая фраза целиком (приоритет)
    if (q.contains(QLatin1Char(' '))) {
        addMatches(phrase);
    }

    // 2) По словам — только если фраза не нашлась
    if (spans.isEmpty()) {
        const QStringList words = q.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
        for (const QString &w : words) {
            if (w.length() < 2)
                continue;
            addMatches(w.toHtmlEscaped());
        }
    }

    // 3) одно слово
    if (spans.isEmpty() && !q.contains(QLatin1Char(' ')) && q.length() >= 2)
        addMatches(phrase);

    if (spans.isEmpty())
        return escapedText;

    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) {
        return a.start < b.start;
    });
    QList<Span> merged;
    for (const Span &s : spans) {
        if (merged.isEmpty() || s.start > merged.last().end)
            merged.append(s);
        else if (s.end > merged.last().end)
            merged.last().end = s.end;
    }
    QString out;
    int pos = 0;
    for (const Span &s : merged) {
        if (s.start > pos)
            out += escapedText.mid(pos, s.start - pos);
        out += QStringLiteral(
            "<mark style=\"background:#ffe28a;color:#1a1000;padding:0 2px;"
            "border-radius:2px;font-weight:600;\">");
        out += escapedText.mid(s.start, s.end - s.start);
        out += QStringLiteral("</mark>");
        pos = s.end;
    }
    if (pos < escapedText.size())
        out += escapedText.mid(pos);
    return out;
}

/** Оставляет в сохранённом фрагменте только безопасные inline-теги. */
static QString sanitizeStoredVerseHtml(QString html)
{
    static const QRegularExpression reScript(
        QStringLiteral("<script[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reStyle(
        QStringLiteral("<style[^>]*>.*?</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reComment(
        QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reImg(
        QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);

    html.replace(reScript, QStringLiteral(" "));
    html.replace(reStyle, QStringLiteral(" "));
    html.replace(reComment, QStringLiteral(" "));
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

    // Оставляем только i/b/sup/sub/a/br
    static const QRegularExpression reOther(
        QStringLiteral("<(?!/?(?:i|b|sup|sub|a|br)\\b)[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(reOther, QStringLiteral(" "));

    static const QRegularExpression reEmptyI(
        QStringLiteral("<i>\\s*</i>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmptyB(
        QStringLiteral("<b>\\s*</b>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reEmptySup(
        QStringLiteral("<sup>\\s*</sup>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reEmptyI, QString());
    html.replace(reEmptyB, QString());
    html.replace(reEmptySup, QString());

    html.replace(QChar(0x00A0), QLatin1Char(' '));
    return html.trimmed();
}

QString Module::getChapterText(int book, int chapter, const QString &highlightQuery) const
{
    if (!openDatabase())
        return {};

    // Есть ли колонка text_orig (подстрочник)
    bool hasOrigCol = false;
    {
        QSqlQuery pr(m_db);
        if (pr.exec("PRAGMA table_info(verses)")) {
            while (pr.next()) {
                if (pr.value(1).toString() == QLatin1String("text_orig")) {
                    hasOrigCol = true;
                    break;
                }
            }
        }
    }

    QSqlQuery query(m_db);
    if (hasOrigCol)
        query.prepare("SELECT verse, text, text_orig FROM verses WHERE book = ? AND chapter = ? ORDER BY verse");
    else
        query.prepare("SELECT verse, text FROM verses WHERE book = ? AND chapter = ? ORDER BY verse");
    query.addBindValue(book);
    query.addBindValue(chapter);
    if (!query.exec()) {
        qWarning() << "getChapterText failed:" << book << chapter << query.lastError();
        return {};
    }

    QString result;
    const bool isCommentary = m_type.toLower().contains(QLatin1String("comment"))
        || m_type.toLower().contains(QString::fromUtf8("коммент"))
        || m_type.toLower().contains(QLatin1String("dict"))
        || m_type.toLower().contains(QString::fromUtf8("слов"));

    while (query.next()) {
        int v = query.value(0).toInt();
        QString text = query.value(1).toString();
        // Пустой стих (заполнитель после диапазона 1–6) — только номер
        if (text.trimmed().isEmpty() || text == QString(QChar(0x200B)))
            text.clear();

        if (!text.isEmpty() && text.contains(QLatin1Char('<')))
            text = sanitizeStoredVerseHtml(text);
        else if (!text.isEmpty())
            text = text.toHtmlEscaped();

        // Убираем ведущий номер стиха, если он остался в сохранённом тексте
        {
            static const QRegularExpression reLeadNum(
                QStringLiteral(R"(^\s*\d{1,3}[\.\)\:\*]?\s+)"));
            text.replace(reLeadNum, QString());
            static const QRegularExpression reLeadSup(
                QStringLiteral(R"(^\s*<sup\b[^>]*>\s*\d{1,3}\s*</sup>\s*)"),
                QRegularExpression::CaseInsensitiveOption);
            text.replace(reLeadSup, QString());
            static const QRegularExpression reJunkLead(
                QStringLiteral(R"(^[\*\†\‡\§]+\s*)"));
            text.replace(reJunkLead, QString());
            text = text.trimmed();
        }

        if (isCommentary)
            text.replace(QRegularExpression(QStringLiteral("\\n{2,}")),
                         QStringLiteral("\n"));
        text.replace(QChar('\n'), QStringLiteral("<br>"));

        if (!highlightQuery.isEmpty())
            text = highlightSearchTerms(text, highlightQuery);

        if (hasOrigCol) {
            const QString orig = query.value(2).toString();
            if (!orig.isEmpty()) {
                const QString numberHtml = QString(
                    "<a name='v%1'></a>"
                    "<a href='v://%2/%3/%1' style='color:#8a5a2b; text-decoration:none;' "
                    "title='Параллельные места'>"
                    "<sup style='font-weight:700; font-size:0.75em;'>%1</sup></a>&nbsp;")
                    .arg(v).arg(book).arg(chapter);
                result += QString(
                    "<div style='margin:6px 0 8px 0; padding-bottom:6px; "
                    "border-bottom:1px solid #e8dcc8;' id='v%1'>"
                    "%2"
                    "<div style='color:#2a1f14; line-height:1.55; text-align:justify;'>%3</div>"
                    "<div style='line-height:1.45; padding-left:0.3em; color:#3a3228; "
                    "font-size:0.95em; margin-top:4px;'>%4</div>"
                    "</div>")
                    .arg(v).arg(numberHtml).arg(text).arg(annotateGreek(orig));
                continue;
            }
        }

        QString numberHtml;
        if (isCommentary) {
            // В комментариях/словарях не показываем номера «псевдо-стихов»,
            // но оставляем якорь для навигации.
            numberHtml = QStringLiteral("<a name='v%1'></a>").arg(v);
        } else {
            numberHtml = QString(
                "<a name='v%1'></a>"
                "<a href='v://%2/%3/%1' style='color:#8a5a2b; text-decoration:none;' "
                "title='Параллельные места'>"
                "<sup style='font-weight:700; font-size:0.75em;'>%1</sup></a>&nbsp;")
                .arg(v).arg(book).arg(chapter);
        }

        result += QString(
            "<div id='v%1' style='"
            "margin:0 0 0.5em 0; "
            "line-height:1.55; "
            "text-align:justify; "
            "color:#2a1f14;'>"
            "%2%3"
            "</div>")
            .arg(v).arg(numberHtml).arg(text);
    }
    return result;
}

int Module::chapterCount(int book) const
{
    // Реальное число глав по стихам (не «дырявый» ChapterQty из ini)
    if (openDatabase()) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT COUNT(DISTINCT chapter) FROM verses WHERE book = ? AND length(trim(text)) > 0"));
        query.addBindValue(book);
        if (query.exec() && query.next()) {
            int n = query.value(0).toInt();
            if (n > 0)
                return n;
        }
        query.prepare(QStringLiteral("SELECT COALESCE(MAX(chapter),0) FROM verses WHERE book = ?"));
        query.addBindValue(book);
        if (query.exec() && query.next()) {
            int n = query.value(0).toInt();
            if (n > 0)
                return n;
        }
    }
    for (const auto &b : m_books) {
        if (b.id == book)
            return b.chapters;
    }
    return 0;
}

int Module::verseCount(int book, int chapter) const
{
    if (!openDatabase())
        return 0;

    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM verses WHERE book = ? AND chapter = ?");
    query.addBindValue(book);
    query.addBindValue(chapter);
    if (query.exec() && query.next())
        return query.value(0).toInt();
    return 0;
}

QVector<int> Module::chapterNumbers(int book) const
{
    QVector<int> out;
    if (openDatabase()) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT DISTINCT chapter FROM verses "
            "WHERE book = ? AND length(trim(text)) > 0 ORDER BY chapter"));
        query.addBindValue(book);
        if (query.exec()) {
            while (query.next())
                out.append(query.value(0).toInt());
        }
    }

    if (out.isEmpty()) {
        for (const auto &b : m_books) {
            if (b.id == book) {
                const int n = qMax(1, b.chapters);
                for (int c = 1; c <= n; ++c)
                    out.append(c);
                break;
            }
        }
    }
    if (out.isEmpty())
        out.append(1);
    return out;
}

QVector<int> Module::verseNumbers(int book, int chapter) const
{
    QVector<int> out;
    if (openDatabase()) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT DISTINCT verse FROM verses "
            "WHERE book = ? AND chapter = ? AND length(trim(text)) > 0 ORDER BY verse"));
        query.addBindValue(book);
        query.addBindValue(chapter);
        if (query.exec()) {
            while (query.next())
                out.append(query.value(0).toInt());
        }
    }
    if (out.isEmpty())
        out.append(1);
    return out;
}


QString Module::bookNameById(int id) const
{
    for (const auto &b : m_books)
        if (b.id == id) return b.fullName;
    return QString::number(id);
}


QString Module::bookAbbr(int bookId)
{
    static const char *abbr[] = {
        "",
        "Быт","Исх","Лев","Чис","Втор","Нав","Суд","Руф","1Цар","2Цар",
        "3Цар","4Цар","1Пар","2Пар","Езд","Неем","Есф","Иов","Пс","Прит",
        "Еккл","Песн","Ис","Иер","Плач","Иез","Дан","Ос","Иоил","Ам",
        "Авд","Ион","Мих","Наум","Авв","Соф","Агг","Зах","Мал",
        "Мф","Мк","Лк","Ин","Деян","Рим","1Кор","2Кор","Гал","Еф",
        "Флп","Кол","1Фес","2Фес","1Тим","2Тим","Тит","Флм","Евр","Иак",
        "1Пет","2Пет","1Ин","2Ин","3Ин","Иуд","Откр",
        // 67+ апокрифы / девтероканон
        "Тов","Иф","Прем","Сир","Вар","1Мак","2Мак","1Ен","Ман"
    };
    const int n = int(sizeof(abbr)/sizeof(abbr[0])) - 1;
    if (bookId < 1 || bookId > n)
        return QString::number(bookId);
    return QString::fromUtf8(abbr[bookId]);
}

QString Module::formatRef(int book, int chapter, int verseStart, int verseEnd)
{
    QString s = QStringLiteral("%1 %2:%3").arg(bookAbbr(book)).arg(chapter).arg(verseStart);
    if (verseEnd > verseStart)
        s += QStringLiteral("–%1").arg(verseEnd);
    return s;
}

QVector<CrossRef> Module::lookupCrossRefs(int book, int chapter, int verse)
{
    QVector<CrossRef> out;
    static QString conn = QStringLiteral("crossrefs_conn");
    QSqlDatabase db;
    if (QSqlDatabase::contains(conn)) {
        db = QSqlDatabase::database(conn);
    } else {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        QStringList candidates;
        candidates << QStringLiteral("/usr/share/bibleapp/crossrefs.sqlite");
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/../share/bibleapp/crossrefs.sqlite");
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/crossrefs.sqlite");
        candidates << QStringLiteral("/home/workdir/artifacts/bibleapp/resources/crossrefs.sqlite");
        for (const QString &p : candidates) {
            if (QFile::exists(p)) {
                db.setDatabaseName(p);
                break;
            }
        }
        if (db.databaseName().isEmpty() || !db.open())
            return out;
    }
    if (!db.isOpen())
        return out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT ref_book, ref_chapter, ref_verse_start, ref_verse_end, note "
        "FROM crossrefs WHERE book=? AND chapter=? AND verse=? "
        "ORDER BY ref_book, ref_chapter, ref_verse_start"));
    q.addBindValue(book);
    q.addBindValue(chapter);
    q.addBindValue(verse);
    if (!q.exec())
        return out;

    QSet<QString> seen;
    while (q.next()) {
        CrossRef cr;
        cr.refBook = q.value(0).toInt();
        cr.refChapter = q.value(1).toInt();
        cr.refVerseStart = q.value(2).toInt();
        cr.refVerseEnd = q.value(3).toInt();
        cr.note = q.value(4).toString();
        cr.display = formatRef(cr.refBook, cr.refChapter, cr.refVerseStart, cr.refVerseEnd);
        const QString key = QStringLiteral("%1:%2:%3:%4")
                                .arg(cr.refBook).arg(cr.refChapter)
                                .arg(cr.refVerseStart).arg(cr.refVerseEnd);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out.append(cr);
    }
    return out;
}

// Поиск по всем книгам модуля (вся таблица verses, без фильтра по book/chapter)

QVector<SearchHit> Module::search(const QString &query, int limit) const
{
    QVector<SearchHit> hits;
    if (!openDatabase() || query.trimmed().isEmpty())
        return hits;

    const QString qNorm = query.trimmed().simplified(); // сжатые пробелы
    const QStringList words = qNorm.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
    if (words.isEmpty())
        return hits;

    const bool isPhrase = words.size() >= 2;

    auto variantsOf = [](const QString &w) {
        QStringList v;
        v << w;
        v << w.toLower();
        v << w.toUpper();
        if (w.size() >= 1)
            v << (w.left(1).toUpper() + w.mid(1).toLower());
        v.removeDuplicates();
        return v;
    };

    QString sql = QStringLiteral("SELECT book, chapter, verse, text FROM verses WHERE 1=1");
    QVariantList binds;

    if (isPhrase) {
        // Фраза целиком: несколько регистровых вариантов
        const QStringList phraseVars = variantsOf(qNorm);
        // также вариант только lower для кириллицы
        QStringList likeVars = phraseVars;
        likeVars << qNorm.toLower();
        likeVars.removeDuplicates();
        sql += QStringLiteral(" AND (");
        for (int i = 0; i < likeVars.size(); ++i) {
            if (i) sql += QStringLiteral(" OR ");
            sql += QStringLiteral("text LIKE ?");
            binds << (QStringLiteral("%") + likeVars[i] + QStringLiteral("%"));
        }
        // запасной путь: все слова есть (если регистр/пробелы мешают LIKE фразы)
        sql += QStringLiteral(" OR (");
        for (int wi = 0; wi < words.size(); ++wi) {
            if (wi) sql += QStringLiteral(" AND ");
            sql += QStringLiteral("(");
            const QStringList vars = variantsOf(words[wi]);
            for (int i = 0; i < vars.size(); ++i) {
                if (i) sql += QStringLiteral(" OR ");
                sql += QStringLiteral("text LIKE ?");
                binds << (QStringLiteral("%") + vars[i] + QStringLiteral("%"));
            }
            sql += QStringLiteral(")");
        }
        sql += QStringLiteral("))");
    } else {
        for (const QString &w : words) {
            const QStringList vars = variantsOf(w);
            sql += QStringLiteral(" AND (");
            for (int i = 0; i < vars.size(); ++i) {
                if (i) sql += QStringLiteral(" OR ");
                sql += QStringLiteral("text LIKE ?");
                binds << (QStringLiteral("%") + vars[i] + QStringLiteral("%"));
            }
            sql += QStringLiteral(")");
        }
    }

    sql += QStringLiteral(" ORDER BY book, chapter, verse LIMIT ?");
    binds << (limit <= 0 ? 8000 : qMax(limit * 3, 200)); // запас под фильтр фразы

    QSqlQuery q(m_db);
    if (!q.prepare(sql))
        return hits;
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (!q.exec())
        return hits;

    static const QRegularExpression reTags(QStringLiteral("<[^>]+>"));
    // фраза с гибкими пробелами в plain-тексте
    QRegularExpression phraseRe;
    if (isPhrase) {
        QStringList esc;
        for (const QString &w : words)
            esc << QRegularExpression::escape(w);
        phraseRe.setPattern(esc.join(QStringLiteral("\\s+")));
        phraseRe.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    }

    while (q.next()) {
        QString raw = q.value(3).toString();
        QString plain = raw;
        plain.replace(reTags, QStringLiteral(" "));
        plain.replace(QChar(0x00A0), QLatin1Char(' '));
        plain = plain.simplified();

        if (isPhrase) {
            // обязательна целая фраза (слова подряд)
            if (!phraseRe.match(plain).hasMatch()
                && !plain.contains(qNorm, Qt::CaseInsensitive)) {
                continue;
            }
        } else {
            bool ok = true;
            for (const QString &w : words) {
                if (!plain.contains(w, Qt::CaseInsensitive)) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
        }

        SearchHit h;
        h.book = q.value(0).toInt();
        h.chapter = q.value(1).toInt();
        h.verse = q.value(2).toInt();
        h.bookName = bookNameById(h.book);

        // сниппет вокруг фразы
        QString snip = plain;
        int pos = plain.indexOf(qNorm, 0, Qt::CaseInsensitive);
        if (pos < 0 && isPhrase) {
            auto m = phraseRe.match(plain);
            if (m.hasMatch())
                pos = m.capturedStart();
        }
        if (pos >= 0) {
            int from = qMax(0, pos - 40);
            int len = qMin(plain.size() - from, qNorm.size() + 80);
            snip = plain.mid(from, len);
            if (from > 0)
                snip.prepend(QStringLiteral("…"));
            if (from + len < plain.size())
                snip.append(QStringLiteral("…"));
        } else if (snip.length() > 120) {
            snip = snip.left(117) + QStringLiteral("...");
        }
        h.snippet = snip;
        hits.append(h);
        if (limit > 0 && hits.size() >= limit)
            break;
    }
    return hits;
}


QString Module::lookupVerseStrongs(int book, int chapter, int verse)
{
    static bool tried = false;
    static QSqlDatabase db;
    static QString conn = QStringLiteral("verse_strongs_conn");
    if (!tried) {
        tried = true;
        QStringList candidates;
        candidates << "/usr/share/bibleapp/verse_strongs.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/verse_strongs.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/verse_strongs.sqlite";
        QString path;
        for (const QString &p : candidates) {
            if (QFile::exists(p)) { path = p; break; }
        }
        if (path.isEmpty() || !QSqlDatabase::isDriverAvailable("QSQLITE"))
            return {};
        if (QSqlDatabase::contains(conn))
            db = QSqlDatabase::database(conn);
        else {
            db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(path);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (!db.open())
                return {};
        }
    }
    if (!db.isOpen())
        return {};
    QSqlQuery q(db);
    q.prepare("SELECT strongs FROM verse_strongs WHERE book=? AND chapter=? AND verse=?");
    q.addBindValue(book);
    q.addBindValue(chapter);
    q.addBindValue(verse);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

static QSqlDatabase &strongsDb()
{
    static bool tried = false;
    static QSqlDatabase db;
    static QString conn = QStringLiteral("strongs_full_conn");
    if (!tried) {
        tried = true;
        QStringList candidates;
        candidates << "/usr/share/bibleapp/strongs_full.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/strongs_full.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/strongs_full.sqlite";
        // fallback: старый греческий лексикон
        candidates << "/usr/share/bibleapp/greek_lexicon.sqlite";
        QString path;
        for (const QString &p : candidates) {
            if (QFile::exists(p)) { path = p; break; }
        }
        if (path.isEmpty() || !QSqlDatabase::isDriverAvailable("QSQLITE"))
            return db;
        if (QSqlDatabase::contains(conn))
            db = QSqlDatabase::database(conn);
        else {
            db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(path);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            db.open();
        }
    }
    return db;
}

static QString normalizeStrongNum(QString s)
{
    s = s.trimmed().toUpper();
    if (s.startsWith(QLatin1Char('G')) || s.startsWith(QLatin1Char('H'))) {
        QString digits = s.mid(1);
        while (digits.startsWith(QLatin1Char('0')) && digits.size() > 1)
            digits = digits.mid(1);
        return s.left(1) + digits;
    }
    return s;
}

QString Module::lookupStrongGloss(const QString &strongNum)
{
    QSqlDatabase &db = strongsDb();
    if (!db.isOpen()) return {};
    QString full = normalizeStrongNum(strongNum);
    QSqlQuery q(db);
    // новая схема strongs_full
    q.prepare("SELECT gloss_ru, gloss_en FROM strongs WHERE num = ? LIMIT 1");
    q.addBindValue(full);
    if (q.exec() && q.next()) {
        QString ru = q.value(0).toString();
        if (!ru.isEmpty()) return ru;
        return q.value(1).toString();
    }
    // fallback старый greek_lexicon
    q.prepare("SELECT gloss FROM greek WHERE strongs = ? LIMIT 1");
    q.addBindValue(full);
    if (q.exec() && q.next())
        return q.value(0).toString();
    if (full.startsWith(QLatin1Char('G')) || full.startsWith(QLatin1Char('H'))) {
        q.prepare("SELECT gloss FROM greek WHERE strongs = ? LIMIT 1");
        q.addBindValue(full.mid(1));
        if (q.exec() && q.next())
            return q.value(0).toString();
    }
    return {};
}

QString Module::lookupStrongArticle(const QString &strongNum)
{
    QSqlDatabase &db = strongsDb();
    if (!db.isOpen()) return {};
    QString full = normalizeStrongNum(strongNum);
    QSqlQuery q(db);
    q.prepare("SELECT num, lemma, gloss_en, gloss_ru, def_en, deriv FROM strongs WHERE num = ? LIMIT 1");
    q.addBindValue(full);
    if (!(q.exec() && q.next()))
        return {};

    QString num = q.value(0).toString();
    QString lemma = q.value(1).toString();
    QString en = q.value(2).toString();
    QString ru = q.value(3).toString();
    QString defEn = q.value(4).toString();
    QString deriv = q.value(5).toString();

    QString html;
    html += QString::fromUtf8("<div style='line-height:1.35'>");
    html += QString::fromUtf8("<b style='font-size:14px'>%1</b>").arg(num.toHtmlEscaped());
    if (!lemma.isEmpty())
        html += QString::fromUtf8(" · <i>%1</i>").arg(lemma.toHtmlEscaped());
    if (!ru.isEmpty())
        html += QString::fromUtf8("<br><b>По-русски:</b> %1").arg(ru.toHtmlEscaped());
    if (!en.isEmpty())
        html += QString::fromUtf8("<br><b>Кратко (EN):</b> %1").arg(en.toHtmlEscaped());
    if (!deriv.isEmpty())
        html += QString::fromUtf8("<br><span style='color:#666'><b>Происх.:</b> %1</span>").arg(deriv.toHtmlEscaped());
    if (!defEn.isEmpty()) {
        QString d = defEn;
        if (d.length() > 700) d = d.left(700) + "...";
        html += QString::fromUtf8("<br><span style='color:#333; font-size:12px'>%1</span>").arg(d.toHtmlEscaped());
    }
    html += QString::fromUtf8("</div>");
    return html;
}

QString Module::lookupGloss(const QString &greekWord)
{
    return translateGreekWord(greekWord);
}

static QString stripGreekDiacritics(const QString &in)
{
    // NFD + убрать combining marks (в т.ч. iota subscript U+0345)
    QString nfd = in.normalized(QString::NormalizationForm_D);
    QString out;
    for (QChar ch : nfd) {
        if (ch.category() == QChar::Mark_NonSpacing || ch.category() == QChar::Mark_SpacingCombining)
            continue;
        // final sigma -> sigma
        if (ch == QChar(0x03C2))
            ch = QChar(0x03C3);
        out.append(ch);
    }
    return out;
}

QString Module::lookupThayer(const QString &greekWord)
{
    static bool tried = false;
    static QSqlDatabase db;
    static QString conn = QStringLiteral("thayer_lex_conn");

    if (!tried) {
        tried = true;
        QStringList candidates;
        candidates << "/usr/share/bibleapp/thayer_lexicon.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/../share/bibleapp/thayer_lexicon.sqlite";
        candidates << QCoreApplication::applicationDirPath() + "/thayer_lexicon.sqlite";
        QString path;
        for (const QString &p : candidates) {
            if (QFile::exists(p)) { path = p; break; }
        }
        if (path.isEmpty() || !QSqlDatabase::isDriverAvailable("QSQLITE"))
            return {};
        if (QSqlDatabase::contains(conn))
            db = QSqlDatabase::database(conn);
        else {
            db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(path);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (!db.open())
                return {};
        }
    }
    if (!db.isOpen())
        return {};

    QString w = greekWord.trimmed();
    while (!w.isEmpty() && !w[0].isLetter()) w = w.mid(1);
    while (!w.isEmpty() && !w.back().isLetter()) w.chop(1);
    if (w.isEmpty()) return {};

    QStringList variants;
    variants << w;
    QString stripped = stripGreekDiacritics(w);
    if (stripped != w)
        variants << stripped;
    variants << w.toLower();
    variants << stripped.toLower();

    QSqlQuery q(db);
    for (const QString &v : variants) {
        q.prepare("SELECT text FROM entries WHERE lemma = ? LIMIT 1");
        q.addBindValue(v);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }
    for (const QString &v : variants) {
        q.prepare("SELECT text FROM entries WHERE lemma_normalized = ? LIMIT 1");
        q.addBindValue(v);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }
    // без диакритики через replace на стороне SQL сложно — перебор LIKE по stripped
    q.prepare("SELECT lemma, text FROM entries WHERE lemma_normalized LIKE ? LIMIT 20");
    q.addBindValue(stripped.toLower() + "%");
    if (q.exec()) {
        while (q.next()) {
            QString lem = q.value(0).toString();
            if (stripGreekDiacritics(lem).compare(stripped, Qt::CaseInsensitive) == 0)
                return q.value(1).toString();
        }
    }
    return {};
}

bool Module::createTestModule(const QString &targetDir)
{
    QDir dir(targetDir);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        qWarning() << "QSQLITE not available, cannot create test module";
        return false;
    }

    QJsonObject meta;
    meta["name"] = QString::fromUtf8("Синодальный перевод (тестовый)");
    meta["description"] = QString::fromUtf8("Тестовый модуль с несколькими главами");
    meta["type"] = "bible";
    meta["language"] = "ru";
    meta["version"] = "0.3";

    QFile metaFile(targetDir + "/module.json");
    if (!metaFile.open(QIODevice::WriteOnly))
        return false;
    metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    metaFile.close();

    QString connName = QString("create_test_") + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(targetDir + "/data.sqlite");
        if (!db.open()) {
            QSqlDatabase::removeDatabase(connName);
            return false;
        }

        QSqlQuery q(db);
        q.exec("CREATE TABLE IF NOT EXISTS books (id INTEGER PRIMARY KEY, short_name TEXT, full_name TEXT, chapters INTEGER)");
        q.exec("CREATE TABLE IF NOT EXISTS verses (book INTEGER, chapter INTEGER, verse INTEGER, text TEXT, PRIMARY KEY(book, chapter, verse))");
        q.exec("CREATE INDEX IF NOT EXISTS idx_verses_book_chapter ON verses(book, chapter)");

        q.exec("DELETE FROM books");
        q.exec("DELETE FROM verses");

        q.exec("INSERT INTO books VALUES (1, 'Быт', 'Бытие', 1)");
        q.exec("INSERT INTO books VALUES (43, 'Ин', 'От Иоанна', 1)");
        q.exec("INSERT INTO books VALUES (19, 'Пс', 'Псалтирь', 1)");

        struct V { int b, c, v; const char *t; };
        const V verses[] = {
            {1, 1, 1, "В начале сотворил Бог небо и землю."},
            {1, 1, 2, "Земля же была безвидна и пуста, и тьма над бездною, и Дух Божий носился над водою."},
            {1, 1, 3, "И сказал Бог: да будет свет. И стал свет."},
            {1, 1, 4, "И увидел Бог свет, что он хорош, и отделил Бог свет от тьмы."},
            {1, 1, 5, "И назвал Бог свет днем, а тьму ночью. И был вечер, и было утро: день один."},
            {1, 1, 6, "И сказал Бог: да будет твердь посреди воды, и да отделяет она воду от воды."},
            {1, 1, 7, "И создал Бог твердь, и отделил воду, которая под твердью, от воды, которая над твердью. И стало так."},
            {1, 1, 8, "И назвал Бог твердь небом. И был вечер, и было утро: день второй."},
            {1, 1, 9, "И сказал Бог: да соберется вода, которая под небом, в одно место, и да явится суша. И стало так."},
            {1, 1, 10, "И назвал Бог сушу землею, а собрание вод назвал морями. И увидел Бог, что это хорошо."},
            {43, 1, 1, "В начале было Слово, и Слово было у Бога, и Слово было Бог."},
            {43, 1, 2, "Оно было в начале у Бога."},
            {43, 1, 3, "Все чрез Него начало быть, и без Него ничто не начало быть, что начало быть."},
            {43, 1, 4, "В Нем была жизнь, и жизнь была свет человеков."},
            {43, 1, 5, "И свет во тьме светит, и тьма не объяла его."},
            {43, 1, 6, "Был человек, посланный от Бога; имя ему Иоанн."},
            {43, 1, 7, "Он пришел для свидетельства, чтобы свидетельствовать о Свете, дабы все уверовали чрез него."},
            {43, 1, 8, "Он не был свет, но был послан, чтобы свидетельствовать о Свете."},
            {43, 1, 9, "Был Свет истинный, Который просвещает всякого человека, приходящего в мир."},
            {43, 1, 10, "В мире был, и мир чрез Него начал быть, и мир Его не познал."},
            {43, 1, 11, "Пришел к своим, и свои Его не приняли."},
            {43, 1, 12, "А тем, которые приняли Его, верующим во имя Его, дал власть быть чадами Божиими."},
            {19, 1, 1, "Блажен муж, который не ходит на совет нечестивых и не стоит на пути грешных и не сидит в собрании развратителей,"},
            {19, 1, 2, "но в законе Господа воля его, и о законе Его размышляет он день и ночь!"},
            {19, 1, 3, "И будет он как дерево, посаженное при потоках вод, которое приносит плод свой во время свое, и лист которого не вянет; и во всем, что он ни делает, успеет."},
            {19, 1, 4, "Не так — нечестивые; но они — как прах, возметаемый ветром."},
            {19, 1, 5, "Потому не устоят нечестивые на суде, и грешники — в собрании праведных."},
            {19, 1, 6, "Ибо знает Господь путь праведных, а путь нечестивых погибнет."},
        };

        q.prepare("INSERT INTO verses (book, chapter, verse, text) VALUES (?, ?, ?, ?)");
        for (const auto &v : verses) {
            q.addBindValue(v.b);
            q.addBindValue(v.c);
            q.addBindValue(v.v);
            q.addBindValue(QString::fromUtf8(v.t));
            if (!q.exec())
                qWarning() << q.lastError();
        }

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return true;
}


bool Module::createInterlinearModule(const QString &targetDir)
{
    QDir dir(targetDir);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    if (!QSqlDatabase::isDriverAvailable("QSQLITE"))
        return false;

    QJsonObject meta;
    meta["name"] = QString::fromUtf8("Подстрочник НЗ (греч. + рус.)");
    meta["description"] = QString::fromUtf8("Русский текст сверху, греческий под ним. Тестовый модуль (Ин 1).");
    meta["type"] = "interlinear";
    meta["language"] = "ru-gr";
    meta["version"] = "0.5";

    QFile metaFile(targetDir + "/module.json");
    if (!metaFile.open(QIODevice::WriteOnly))
        return false;
    metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    metaFile.close();

    QString connName = QString("create_il_") + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(targetDir + "/data.sqlite");
        if (!db.open()) {
            QSqlDatabase::removeDatabase(connName);
            return false;
        }

        QSqlQuery q(db);
        q.exec("CREATE TABLE IF NOT EXISTS books (id INTEGER PRIMARY KEY, short_name TEXT, full_name TEXT, chapters INTEGER)");
        q.exec("CREATE TABLE IF NOT EXISTS verses (book INTEGER, chapter INTEGER, verse INTEGER, text TEXT, text_orig TEXT, PRIMARY KEY(book, chapter, verse))");
        q.exec("CREATE INDEX IF NOT EXISTS idx_verses_book_chapter ON verses(book, chapter)");
        q.exec("DELETE FROM books");
        q.exec("DELETE FROM verses");

        q.exec("INSERT INTO books VALUES (43, 'Ин', 'От Иоанна', 1)");

        // text = русский, text_orig = греческий (общественное достояние, Nestle-era / стандартный текст)
        struct Row { int v; const char *ru; const char *gr; };
        const Row rows[] = {
            {1, "В начале было Слово, и Слово было у Бога, и Слово было Бог.",
                "Ἐν ἀρχῇ ἦν ὁ λόγος, καὶ ὁ λόγος ἦν πρὸς τὸν θεόν, καὶ θεὸς ἦν ὁ λόγος."},
            {2, "Оно было в начале у Бога.",
                "οὗτος ἦν ἐν ἀρχῇ πρὸς τὸν θεόν."},
            {3, "Все чрез Него начало быть, и без Него ничто не начало быть, что начало быть.",
                "πάντα δι’ αὐτοῦ ἐγένετο, καὶ χωρὶς αὐτοῦ ἐγένετο οὐδὲ ἕν ὃ γέγονεν."},
            {4, "В Нем была жизнь, и жизнь была свет человеков.",
                "ἐν αὐτῷ ζωὴ ἦν, καὶ ἡ ζωὴ ἦν τὸ φῶς τῶν ἀνθρώπων."},
            {5, "И свет во тьме светит, и тьма не объяла его.",
                "καὶ τὸ φῶς ἐν τῇ σκοτίᾳ φαίνει, καὶ ἡ σκοτία αὐτὸ οὐ κατέλαβεν."},
            {6, "Был человек, посланный от Бога; имя ему Иоанн.",
                "ἐγένετο ἄνθρωπος ἀπεσταλμένος παρὰ θεοῦ, ὄνομα αὐτῷ Ἰωάννης."},
            {7, "Он пришел для свидетельства, чтобы свидетельствовать о Свете, дабы все уверовали чрез него.",
                "οὗτος ἦλθεν εἰς μαρτυρίαν, ἵνα μαρτυρήσῃ περὶ τοῦ φωτός, ἵνα πάντες πιστεύσωσιν δι’ αὐτοῦ."},
            {8, "Он не был свет, но был послан, чтобы свидетельствовать о Свете.",
                "οὐκ ἦν ἐκεῖνος τὸ φῶς, ἀλλ’ ἵνα μαρτυρήσῃ περὶ τοῦ φωτός."},
            {9, "Был Свет истинный, Который просвещает всякого человека, приходящего в мир.",
                "ἦν τὸ φῶς τὸ ἀληθινὸν ὃ φωτίζει πάντα ἄνθρωπον ἐρχόμενον εἰς τὸν κόσμον."},
            {10, "В мире был, и мир чрез Него начал быть, и мир Его не познал.",
                "ἐν τῷ κόσμῳ ἦν, καὶ ὁ κόσμος δι’ αὐτοῦ ἐγένετο, καὶ ὁ κόσμος αὐτὸν οὐκ ἔγνω."},
            {11, "Пришел к своим, и свои Его не приняли.",
                "εἰς τὰ ἴδια ἦλθεν, καὶ οἱ ἴδιοι αὐτὸν οὐ παρέλαβον."},
            {12, "А тем, которые приняли Его, верующим во имя Его, дал власть быть чадами Божиими,",
                "ὅσοι δὲ ἔλαβον αὐτόν, ἔδωκεν αὐτοῖς ἐξουσίαν τέκνα θεοῦ γενέσθαι, τοῖς πιστεύουσιν εἰς τὸ ὄνομα αὐτοῦ."},
            {13, "которые ни от крови, ни от хотения плоти, ни от хотения мужа, но от Бога родились.",
                "οἳ οὐκ ἐξ αἱμάτων οὐδὲ ἐκ θελήματος σαρκὸς οὐδὲ ἐκ θελήματος ἀνδρὸς ἀλλ’ ἐκ θεοῦ ἐγεννήθησαν."},
            {14, "И Слово стало плотию и обитало с нами, полное благодати и истины; и мы видели славу Его, славу как Единородного от Отца.",
                "καὶ ὁ λόγος σὰρξ ἐγένετο καὶ ἐσκήνωσεν ἐν ἡμῖν, καὶ ἐθεασάμεθα τὴν δόξαν αὐτοῦ, δόξαν ὡς μονογενοῦς παρὰ πατρός, πλήρης χάριτος καὶ ἀληθείας."},
            {15, "Иоанн свидетельствует о Нем и, восклицая, говорит: Сей был Тот, о Котором я сказал, что Идущий за мною стал впереди меня, потому что был прежде меня.",
                "Ἰωάννης μαρτυρεῖ περὶ αὐτοῦ καὶ κέκραγεν λέγων· οὗτος ἦν ὃν εἶπον· ὁ ὀπίσω μου ἐρχόμενος ἔμπροσθέν μου γέγονεν, ὅτι πρῶτός μου ἦν."},
            {16, "И от полноты Его все мы приняли и благодать на благодать,",
                "ὅτι ἐκ τοῦ πληρώματος αὐτοῦ ἡμεῖς πάντες ἐλάβομεν καὶ χάριν ἀντὶ χάριτος."},
            {17, "ибо закон дан чрез Моисея; благодать же и истина произошли чрез Иисуса Христа.",
                "ὅτι ὁ νόμος διὰ Μωϋσέως ἐδόθη, ἡ χάρις καὶ ἡ ἀλήθεια διὰ Ἰησοῦ Χριστοῦ ἐγένετο."},
            {18, "Бога не видел никто никогда; Единородный Сын, сущий в недре Отчем, Он явил.",
                "θεὸν οὐδεὶς ἑώρακεν πώποτε· μονογενὴς θεὸς ὁ ὢν εἰς τὸν κόλπον τοῦ πατρὸς ἐκεῖνος ἐξηγήσατο."},
        };

        q.prepare("INSERT INTO verses (book, chapter, verse, text, text_orig) VALUES (43, 1, ?, ?, ?)");
        for (const auto &r : rows) {
            q.addBindValue(r.v);
            q.addBindValue(QString::fromUtf8(r.ru));
            q.addBindValue(QString::fromUtf8(r.gr));
            q.exec();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return true;
}
