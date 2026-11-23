#include "ModuleManagerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QMetaObject>
#include <QDir>
#include <QColor>
#include <QBrush>
#include <QPalette>
#include <QFont>
#include <QSize>
#include <QProgressBar>
#include <QDialogButtonBox>
#include <QTextEdit>

ModuleManagerDialog::ModuleManagerDialog(ModuleManager *manager, QWidget *parent,
                                         const QString &currentModuleName)
    : QDialog(parent)
    , m_manager(manager)
    , m_currentModuleName(currentModuleName)
{
    setWindowTitle(tr("Менеджер модулей"));
    resize(560, 420);

    // Фон как у окон текста (#f7f0e0), чёрный текст — без белого
    const QColor paper(0xf7, 0xf0, 0xe0);
    const QColor black(0x00, 0x00, 0x00);
    const QColor sel(0xe0, 0xd0, 0xa0);
    {
        QPalette pal = this->palette();
        for (int gi = 0; gi < 3; ++gi) {
            const auto g = static_cast<QPalette::ColorGroup>(gi);
            pal.setColor(g, QPalette::Window, paper);
            pal.setColor(g, QPalette::Base, paper);
            pal.setColor(g, QPalette::AlternateBase, paper);
            pal.setColor(g, QPalette::Text, black);
            pal.setColor(g, QPalette::WindowText, black);
            pal.setColor(g, QPalette::ButtonText, black);
            pal.setColor(g, QPalette::BrightText, black);
            pal.setColor(g, QPalette::Button, QColor(0xef, 0xe6, 0xd2));
            pal.setColor(g, QPalette::Highlight, sel);
            pal.setColor(g, QPalette::HighlightedText, black);
        }
        setPalette(pal);
        setAutoFillBackground(true);
    }

    setStyleSheet(QStringLiteral(
        "QDialog {"
        "  background-color: #f7f0e0;"
        "  color: #000000;"
        "}"
        "QLabel {"
        "  background-color: #f7f0e0;"
        "  color: #000000;"
        "}"
        "QListWidget {"
        "  background-color: #f7f0e0;"
        "  color: #000000;"
        "  border: 1px solid #c4b090;"
        "  outline: 0;"
        "  font-size: 13px;"
        "}"
        "QListWidget::item {"
        "  background-color: #f7f0e0;"
        "  color: #000000;"
        "  padding: 10px 12px;"
        "  margin: 0px;"
        "  min-height: 22px;"
        "  border: none;"
        "  border-bottom: 1px solid #e0d4bc;"
        "}"
        "QListWidget::item:selected,"
        "QListWidget::item:selected:active,"
        "QListWidget::item:selected:!active {"
        "  background-color: #e8d4a8;"
        "  color: #000000;"
        "}"
        "QListWidget::item:hover {"
        "  background-color: #efe4cc;"
        "  color: #000000;"
        "}"
        "QPushButton {"
        "  background-color: #efe6d2;"
        "  color: #000000;"
        "  border: 1px solid #c4b090;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "}"
        "QPushButton:hover { background-color: #e8dcc4; color: #000000; }"
        "QPushButton:pressed { background-color: #dccfb0; color: #000000; }"
    ));

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Установленные модули:"), this));

    m_list = new QListWidget(this);
    m_list->setAutoFillBackground(true);
    m_list->setSpacing(2);
    m_list->setUniformItemSizes(false);
    {
        QPalette lp = m_list->palette();
        for (int gi = 0; gi < 3; ++gi) {
            const auto g = static_cast<QPalette::ColorGroup>(gi);
            lp.setColor(g, QPalette::Base, paper);
            lp.setColor(g, QPalette::Text, black);
            lp.setColor(g, QPalette::WindowText, black);
            lp.setColor(g, QPalette::Window, paper);
            lp.setColor(g, QPalette::HighlightedText, black);
            lp.setColor(g, QPalette::Highlight, sel);
        }
        m_list->setPalette(lp);
    }
    layout->addWidget(m_list, 1);

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    auto *btnLayout = new QHBoxLayout;

    m_convertBtn = new QPushButton(tr("m.biblequote"), this);
    m_myBibleBtn = new QPushButton(tr("m.mybible"), this);
    m_myBibleBtn->setToolTip(tr("Импорт любого модуля MyBible (.SQLite3): Библия, словарь, симфония, комментарий"));
    m_openBtn = new QPushButton(tr("Открыть выбранный"), this);
    m_deleteBtn = new QPushButton(tr("Удалить модуль"), this);
    auto *closeBtn = new QPushButton(tr("Закрыть"), this);

    btnLayout->addWidget(m_convertBtn);
    btnLayout->addWidget(m_myBibleBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_openBtn);
    btnLayout->addWidget(m_deleteBtn);
    btnLayout->addWidget(closeBtn);

    layout->addLayout(btnLayout);

    connect(m_convertBtn, &QPushButton::clicked, this, &ModuleManagerDialog::onConvert);
    connect(m_myBibleBtn, &QPushButton::clicked, this, &ModuleManagerDialog::onConvertMyBible);
    connect(m_openBtn, &QPushButton::clicked, this, &ModuleManagerDialog::onOpenSelected);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ModuleManagerDialog::onDeleteSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &ModuleManagerDialog::onSelectionChanged);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { onOpenSelected(); });

    refreshList();
}

void ModuleManagerDialog::refreshList()
{
    m_list->clear();
    m_manager->loadModules();

    QListWidgetItem *currentItem = nullptr;
    for (int i = 0; i < m_manager->modules().size(); ++i) {
        Module *m = m_manager->moduleAt(i);
        auto *item = new QListWidgetItem(m->name(), m_list);
        item->setData(Qt::UserRole, i);
        item->setToolTip(m->description());
        // Бумажный фон + чёрный текст
        item->setData(Qt::ForegroundRole, QBrush(QColor(0, 0, 0)));
        item->setData(Qt::BackgroundRole, QBrush(QColor(0xf7, 0xf0, 0xe0)));
        item->setForeground(QBrush(QColor(0, 0, 0)));
        item->setBackground(QBrush(QColor(0xf7, 0xf0, 0xe0)));
        item->setSizeHint(QSize(0, 36));

        if (!m_currentModuleName.isEmpty() && m->name() == m_currentModuleName) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            item->setData(Qt::BackgroundRole, QBrush(QColor(0xe8, 0xc8, 0x78)));
            item->setData(Qt::ForegroundRole, QBrush(QColor(0, 0, 0)));
            item->setBackground(QBrush(QColor(0xe8, 0xc8, 0x78)));
            item->setForeground(QBrush(QColor(0, 0, 0)));
            item->setToolTip(tr("Сейчас открыт в этом окне") + QStringLiteral("\n") + m->description());
            item->setText(QString::fromUtf8("●  ") + m->name() + tr("  — открыт"));
            currentItem = item;
        }
    }

    if (currentItem) {
        m_list->setCurrentItem(currentItem);
        m_list->scrollToItem(currentItem);
    }

    if (m_list->count() == 0) {
        m_infoLabel->setText(tr("Нет установленных модулей. Создайте тестовый или сконвертируйте модуль из «Цитаты из Библии»."));
    } else if (!m_currentModuleName.isEmpty()) {
        m_infoLabel->setText(tr("Модулей: %1. Сейчас открыт: «%2»")
                                 .arg(m_list->count()).arg(m_currentModuleName));
    } else {
        m_infoLabel->setText(tr("Модулей: %1").arg(m_list->count()));
    }
    onSelectionChanged();
}

void ModuleManagerDialog::onSelectionChanged()
{
    const bool has = !m_list->selectedItems().isEmpty();
    m_openBtn->setEnabled(has);
    if (m_deleteBtn)
        m_deleteBtn->setEnabled(has);
}

void ModuleManagerDialog::onOpenSelected()
{
    auto items = m_list->selectedItems();
    if (items.isEmpty())
        return;

    int index = items.first()->data(Qt::UserRole).toInt();
    Module *m = m_manager->moduleAt(index);
    if (m) {
        emit moduleSelected(m);
        accept();
    }
}

void ModuleManagerDialog::onConvert()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        tr("Выберите папку модуля «Цитаты из Библии»"));
    if (dir.isEmpty())
        return;

    // ---- Окно процесса конвертации (только здесь виден прогресс) ----
    QDialog stepDlg(this);
    stepDlg.setWindowTitle(tr("Конвертер модулей"));
    stepDlg.setModal(true);
    stepDlg.setMinimumSize(520, 420);
    auto *lay = new QVBoxLayout(&stepDlg);

    auto *title = new QLabel(tr("<b>Конвертация модуля BibleQuote</b>"), &stepDlg);
    title->setTextFormat(Qt::RichText);
    lay->addWidget(title);

    auto makeStep = [&](const QString &caption) -> QLabel * {
        auto *lb = new QLabel(QString::fromUtf8("○  %1").arg(caption), &stepDlg);
        lb->setWordWrap(true);
        lay->addWidget(lb);
        return lb;
    };

    QLabel *step1 = makeStep(tr("1. Анализ модуля…"));
    QLabel *step2 = makeStep(tr("2. Конвертация текста…"));
    QLabel *step3 = makeStep(tr("3. Аудит результата…"));
    QLabel *step4 = makeStep(tr("4. Исправления…"));
    QLabel *step5 = makeStep(tr("5. Итог"));

    auto *bar = new QProgressBar(&stepDlg);
    bar->setRange(0, 100);
    bar->setValue(0);
    lay->addWidget(bar);

    auto *detail = new QLabel(tr("Ожидание…"), &stepDlg);
    detail->setWordWrap(true);
    detail->setStyleSheet(QStringLiteral("color:#666;"));
    lay->addWidget(detail);

    auto *log = new QTextEdit(&stepDlg);
    log->setReadOnly(true);
    log->setFont(QFont(QStringLiteral("Monospace"), 9));
    log->setMinimumHeight(140);
    lay->addWidget(log, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &stepDlg);
    buttons->button(QDialogButtonBox::Close)->setEnabled(false);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &stepDlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &stepDlg, &QDialog::accept);
    lay->addWidget(buttons);

    auto setStep = [](QLabel *lb, const QString &text, const QString &state) {
        // state: wait | run | ok | fail | skip
        QString mark = QString::fromUtf8("○");
        QString color = QStringLiteral("#444");
        if (state == QLatin1String("run")) {
            mark = QString::fromUtf8("►");
            color = QStringLiteral("#1a5fb4");
        } else if (state == QLatin1String("ok")) {
            mark = QString::fromUtf8("✓");
            color = QStringLiteral("#2ec27e");
        } else if (state == QLatin1String("fail")) {
            mark = QString::fromUtf8("✗");
            color = QStringLiteral("#e01b24");
        } else if (state == QLatin1String("skip")) {
            mark = QString::fromUtf8("–");
            color = QStringLiteral("#888");
        }
        lb->setText(QString::fromUtf8("<span style='color:%1; font-weight:600;'>%2</span>  %3")
                        .arg(color, mark, text.toHtmlEscaped()));
        lb->setTextFormat(Qt::RichText);
    };

    stepDlg.show();
    QCoreApplication::processEvents();

    // ===== 1. АНАЛИЗ =====
    setStep(step1, tr("1. Анализ модуля…"), QStringLiteral("run"));
    detail->setText(tr("Читаю INI, список книг и HTML…"));
    bar->setValue(5);
    QCoreApplication::processEvents();

    auto analysis = m_converter.analyze(dir);
    if (!analysis.success) {
        setStep(step1, tr("1. Анализ модуля — невозможно"), QStringLiteral("fail"));
        setStep(step2, tr("2. Конвертация — пропущена"), QStringLiteral("skip"));
        setStep(step3, tr("3. Аудит — пропущен"), QStringLiteral("skip"));
        setStep(step4, tr("4. Исправления — пропущены"), QStringLiteral("skip"));
        setStep(step5, tr("5. Итог: конвертировать невозможно"), QStringLiteral("fail"));
        detail->setText(tr("Анализ не пройден."));
        log->setPlainText(analysis.message);
        bar->setValue(100);
        buttons->button(QDialogButtonBox::Close)->setEnabled(true);
        stepDlg.exec();
        return;
    }
    setStep(step1, tr("1. Анализ модуля — готов"), QStringLiteral("ok"));
    log->append(analysis.analysisReport.isEmpty() ? analysis.message : analysis.analysisReport);
    bar->setValue(15);
    QCoreApplication::processEvents();

    QString outName = analysis.detectedName;
    // безопасное имя папки
    outName.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    QString outPath = m_manager->modulesPath() + "/" + outName;

    // ===== 2. КОНВЕРТАЦИЯ =====
    setStep(step2, tr("2. Конвертация текста…"), QStringLiteral("run"));
    detail->setText(tr("Извлекаю главы и стихи…"));
    QCoreApplication::processEvents();

    QElapsedTimer timer;
    timer.start();

    QMetaObject::Connection conn = connect(
        &m_converter, &Converter::progress, this,
        [&](int current, int total, const QString &status) {
            if (total > 0) {
                int pct = 15 + qBound(0, (current * 55) / total, 55); // 15..70
                bar->setValue(pct);
                detail->setText(tr("Книга %1 из %2 — %3").arg(current).arg(total).arg(status));
            } else {
                detail->setText(status);
            }
            QCoreApplication::processEvents();
        });

    auto result = m_converter.convert(dir, outPath);
    disconnect(conn);

    if (result.versesConverted <= 0 && !result.success) {
        setStep(step2, tr("2. Конвертация — ошибка"), QStringLiteral("fail"));
        setStep(step3, tr("3. Аудит — пропущен"), QStringLiteral("skip"));
        setStep(step4, tr("4. Исправления — пропущены"), QStringLiteral("skip"));
        setStep(step5, tr("5. Итог: конвертировать невозможно"), QStringLiteral("fail"));
        detail->setText(tr("Не удалось извлечь текст."));
        log->setPlainText(result.message);
        bar->setValue(100);
        buttons->button(QDialogButtonBox::Close)->setEnabled(true);
        stepDlg.exec();
        return;
    }
    setStep(step2,
            tr("2. Конвертация — %1 книг, %2 стихов")
                .arg(result.booksConverted).arg(result.versesConverted),
            QStringLiteral("ok"));
    bar->setValue(75);
    QCoreApplication::processEvents();

    // ===== 3. АУДИТ (уже выполнен внутри convert, показываем) =====
    setStep(step3, tr("3. Аудит результата…"), QStringLiteral("run"));
    detail->setText(tr("Проверяю книги, стихи и образцы текста…"));
    QCoreApplication::processEvents();

    if (!result.auditReport.isEmpty())
        log->append(QStringLiteral("\n") + result.auditReport);

    if (result.auditErrors > 0) {
        setStep(step3, tr("3. Аудит — найдены ошибки (%1)").arg(result.auditErrors),
                QStringLiteral("fail"));
    } else if (result.auditWarnings > 0) {
        setStep(step3, tr("3. Аудит — есть замечания (%1)").arg(result.auditWarnings),
                QStringLiteral("ok"));
    } else {
        setStep(step3, tr("3. Аудит — без замечаний"), QStringLiteral("ok"));
    }
    bar->setValue(88);
    QCoreApplication::processEvents();

    // ===== 4. ИСПРАВЛЕНИЯ (зарезервировано; сейчас — фиксация статуса) =====
    setStep(step4, tr("4. Исправления…"), QStringLiteral("run"));
    detail->setText(tr("Проверяю необходимость правок…"));
    QCoreApplication::processEvents();

    if (result.auditErrors > 0) {
        setStep(step4, tr("4. Исправления — требуется ручная проверка исходника"),
                QStringLiteral("fail"));
    } else if (result.auditWarnings > 0) {
        setStep(step4, tr("4. Исправления — замечания зафиксированы"),
                QStringLiteral("ok"));
    } else {
        setStep(step4, tr("4. Исправления не требуются"), QStringLiteral("ok"));
    }
    bar->setValue(95);
    QCoreApplication::processEvents();

    // ===== 5. ИТОГ =====
    if (result.success && result.auditErrors == 0) {
        setStep(step5, tr("5. Итог: модуль готов"), QStringLiteral("ok"));
        detail->setText(tr("Можно открыть модуль в программе."));
        bar->setValue(100);
        // Акцент на только что установленном модуле
        if (!result.detectedName.isEmpty())
            m_currentModuleName = result.detectedName;
        refreshList();
    } else if (result.versesConverted > 0 && result.auditErrors == 0) {
        setStep(step5, tr("5. Итог: модуль готов (с замечаниями)"), QStringLiteral("ok"));
        detail->setText(tr("Модуль сохранён, но есть замечания аудита."));
        bar->setValue(100);
        if (!result.detectedName.isEmpty())
            m_currentModuleName = result.detectedName;
        refreshList();
    } else {
        setStep(step5, tr("5. Итог: конвертировать невозможно"), QStringLiteral("fail"));
        detail->setText(tr("Исправьте исходный модуль и повторите попытку."));
        bar->setValue(100);
    }

    buttons->button(QDialogButtonBox::Close)->setEnabled(true);
    stepDlg.exec();
}

void ModuleManagerDialog::onDeleteSelected()
{
    auto items = m_list->selectedItems();
    if (items.isEmpty())
        return;

    int index = items.first()->data(Qt::UserRole).toInt();
    Module *m = m_manager->moduleAt(index);
    if (!m)
        return;

    const auto ans = QMessageBox::question(
        this,
        tr("Удаление модуля"),
        tr("Удалить модуль «%1» с диска?\nЭто действие нельзя отменить.").arg(m->name()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (ans != QMessageBox::Yes)
        return;

    const QString deletedName = m->name();
    if (m_manager->removeModule(index)) {
        emit moduleDeleted(deletedName);
        if (m_currentModuleName == deletedName)
            m_currentModuleName.clear();
        refreshList();
        m_infoLabel->setText(tr("Модуль удалён."));
    } else {
        QMessageBox::warning(this, tr("Удаление"),
            tr("Не удалось удалить модуль. Возможно, это встроенный модуль без права на удаление."));
    }
}

void ModuleManagerDialog::onConvertMyBible()
{
    const QString file = QFileDialog::getOpenFileName(
        this,
        tr("Выберите модуль MyBible"),
        QString(),
        tr("MyBible SQLite (*.SQLite3 *.sqlite3 *.sqlite *.db);;Все файлы (*)")
    );
    if (file.isEmpty())
        return;

    QProgressDialog prog(tr("Импорт MyBible…"), tr("Отмена"), 0, 100, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    prog.setValue(5);

    connect(&m_converter, &Converter::progress, this, [&](int current, int total, const QString &status) {
        if (total > 0)
            prog.setValue(10 + (current * 80) / total);
        prog.setLabelText(status);
        QCoreApplication::processEvents();
    });

    const QString outDir = m_manager->modulesPath();
    auto result = m_converter.convertMyBible(file, outDir);

    prog.setValue(100);
    if (!result.success) {
        QMessageBox::warning(this, tr("m.mybible"),
                             result.message.isEmpty() ? tr("Импорт не удался.") : result.message);
        return;
    }

    QMessageBox::information(
        this, tr("m.mybible"),
        tr("Модуль «%1» установлен.\nКниг (букв): %2\nСтатей: %3")
            .arg(result.detectedName)
            .arg(result.booksConverted)
            .arg(result.versesConverted));

    if (!result.detectedName.isEmpty())
        m_currentModuleName = result.detectedName;
    refreshList();
}


