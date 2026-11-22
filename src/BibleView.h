#ifndef BIBLEVIEW_H
#define BIBLEVIEW_H

#include <QWidget>
#include "Module.h"

class QComboBox;
class QSplitter;
class QTextBrowser;
class WordInfoWindow;
class QLabel;
class QPushButton;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QFontComboBox;

class BibleView : public QWidget
{
    Q_OBJECT
public:
    explicit BibleView(QWidget *parent = nullptr);

    void setModule(Module *module);
    Module *module() const { return m_module; }
    int currentBook() const { return m_currentBook; }
    int currentChapter() const { return m_currentChapter; }
    int currentVerse() const { return m_currentVerse; }

    void setFontSize(int points);
    int fontSize() const { return m_fontSize; }
    void setFontFamily(const QString &family);
    QString fontFamily() const { return m_fontFamily; }
    void setSettingsSaveBlocked(bool blocked) { m_blockSettingsSave = blocked; }
    bool settingsSaveBlocked() const { return m_blockSettingsSave; }
    QSplitter *contentSplitter() const { return m_contentSplitter; }
    void restoreContentSplitter();
    void setActiveHighlight(bool active);
    bool isActiveHighlight() const { return m_activeHighlight; }

    // Синхронный скролл: доля 0.0–1.0
    double scrollRatio() const;
    void setScrollRatio(double ratio);
    QTextBrowser *textBrowser() const { return m_textView; }

signals:
    void requestLoadModule();
    void verseChanged(int book, int chapter, int verse);
    void scrollRatioChanged(double ratio);

public slots:
    void goTo(int book, int chapter, int verse = 1);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onBookChanged(int index);
    void onChapterChanged(int chapter);
    void onVerseChanged(int index);
    void refillVerseCombo(int selectVerse = 1);
    void fillChapterCombo(int book, int selectChapter);
    void fillVerseCombo(int book, int chapter, int selectVerse);
    void onSearch();
    void onSearchResultClicked();
    void onClearSearchResults();
    void onFontBigger();
    void onFontSmaller();
    void populateFontCombo();
    void onPrevChapter();
    void onNextChapter();
    void onAddBookmark();
    void onTextContextMenu(const QPoint &pos);
    void addBookmarkWithLabel(const QString &label, const QString &snippet = QString());
    void revealTextSnippet(const QString &snippet);
    void scrollToSearchMatch(const QString &query, int verse);
    void revealParallelVerse(int book, int chapter, int verse);
    void onBookmarkClicked();
    void onRemoveBookmark();
    void onClearBookmarks();
    void onAnchorClicked(const QUrl &url);

private:
    void loadChapter();
    static QString highlightTerms(const QString &escapedText, const QString &query);
    void applyTextStyle();
    void showParallelForCurrent();
    void showParallelsForVerse(int book, int chapter, int verse);
    void loadBookmarks();
    void saveBookmarks();
    void refreshBookmarkList();
    QString bookmarkKey() const;

    Module *m_module = nullptr;
    int m_fontSize = 13;
    bool m_activeHighlight = false;
    QString m_fontFamily = QStringLiteral("Noto Serif");
    int m_currentBook = 0;
    int m_currentChapter = 1;
    int m_currentVerse = 1;
    bool m_blockSettingsSave = false;
    QString m_lastSearchQuery;
    int m_highlightVerse = 0;
    QString m_pendingSnippet;

    QComboBox *m_bookCombo = nullptr;
    QComboBox *m_chapterCombo = nullptr;
    QComboBox *m_verseCombo = nullptr;
    QLabel *m_chapterLabel = nullptr;
    QLabel *m_moduleLabel = nullptr;
    QLabel *m_activeBanner = nullptr;
    QTextBrowser *m_textView = nullptr;
    QSplitter *m_contentSplitter = nullptr;
    QPushButton *m_loadBtn = nullptr;
    QPushButton *m_prevBtn = nullptr;
    QPushButton *m_nextBtn = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_searchResults = nullptr;
    QListWidget *m_parallelList = nullptr;
    QListWidget *m_bookmarkList = nullptr;
    QLabel *m_fontLabel = nullptr;
    QFontComboBox *m_fontCombo = nullptr;
    QTabWidget *m_sideTabs = nullptr;
    WordInfoWindow *m_wordWindow = nullptr;
    void showWordInfo(const QString &html);
};

#endif
