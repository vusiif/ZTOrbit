#include "MoonTab.h"
#include "../MainWindow.h"
#include "../ZeroTierClient.h"
#include "../UiTransitions.h"
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace vusiif::ztgui::ui {
namespace {
QString moonDirectory() { return vusiif::ztgui::zerotier::moonDirectory(); }
QString moonId(const QString &value)
{
    return QRegularExpression("^[0-9a-fA-F]{1,16}$").match(value).hasMatch()
        ? value.toLower().rightJustified(16, '0') : value;
}

class DeleteConfirmation : public AnimatedDialog {
public:
    DeleteConfirmation(QWidget *parent, const QString &name) : AnimatedDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    {
        setObjectName("moonDeleteConfirmation");
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
        setModal(true);
        setGeometry(QRect(parent->mapToGlobal(QPoint(0, 0)), parent->size()));
        parent->installEventFilter(this);
        auto *layout = new QVBoxLayout(this);
        layout->addStretch();
        auto *card = new QFrame;
        card->setObjectName("confirmCard");
        card->setFixedWidth(430);
        auto *content = new QVBoxLayout(card);
        content->setContentsMargins(24, 24, 24, 24);
        content->setSpacing(18);
        auto *title = new QLabel(QStringLiteral("删除 Moon"));
        title->setObjectName("sectionTitle");
        content->addWidget(title);
        auto *message = new QLabel(QStringLiteral("确定删除“%1”？\n删除后重启服务。此操作无法撤销。").arg(name));
        message->setWordWrap(true);
        message->setTextFormat(Qt::PlainText);
        content->addWidget(message);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new QPushButton(QStringLiteral("取消"));
        cancel->setObjectName("cancelDelete");
        auto *remove = new QPushButton(QStringLiteral("删除并重启"));
        remove->setObjectName("confirmDelete");
        buttons->addWidget(cancel);
        buttons->addWidget(remove);
        content->addLayout(buttons);
        layout->addWidget(card, 0, Qt::AlignHCenter);
        layout->addStretch();
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(remove, &QPushButton::clicked, this, &QDialog::accept);
        cancel->setDefault(true);
        cancel->setFocus();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(65, 70, 76, 155));
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
            setGeometry(QRect(parentWidget()->mapToGlobal(QPoint(0, 0)), parentWidget()->size()));
        return QDialog::eventFilter(object, event);
    }
};
}

MoonTab::MoonTab(MainWindow *mainWindow, QWidget *parent)
    : DataTable("moons", {"名称"}, parent, true, false), m_main(mainWindow)
{
    setAcceptDrops(true);
    m_add = new QPushButton(QStringLiteral("添加Moon文件"));
    m_add->setObjectName("addMoonButton");
    bodyLayout()->addWidget(m_add, 0, Qt::AlignLeft);
    connect(m_add, &QPushButton::clicked, this, [this] { addMoon(); });
    tree()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree(), &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = tree()->itemAt(position);
        if (!item || m_busy) return;
        tree()->clearSelection();
        tree()->setCurrentItem(item);
        item->setSelected(true);
        const QString id = selectedId();
        AnimatedMenu menu(this);
        menu.setObjectName("moonMenu");
        menu.addAction(QStringLiteral("备注"), this, &DataTable::renameSelected);
        menu.addAction(QStringLiteral("详细"), this, &DataTable::showDetails);
        menu.addSeparator();
        auto *remove = new QWidgetAction(&menu);
        remove->setText(QStringLiteral("删除"));
        auto *button = new QPushButton(QStringLiteral("删除"));
        button->setObjectName("deleteMoonAction");
        button->setStyleSheet("QPushButton { color: #e45260; text-align: left; border: none; background: transparent; padding: 8px 24px; } QPushButton:hover { background: palette(highlight); }");
        remove->setDefaultWidget(button);
        menu.addAction(remove);
        const auto requestDelete = [this, &menu, id] { menu.close(); deleteMoon(id); };
        connect(button, &QPushButton::clicked, &menu, requestDelete);
        connect(remove, &QAction::triggered, &menu, requestDelete);
        menu.exec(tree()->viewport()->mapToGlobal(position));
    });
    connect(tree(), &QTreeWidget::itemDoubleClicked, this, [this] { showDetails(); });
    m_poller = new vusiif::ztgui::zerotier::JsonPoller(this, "listmoons");
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::availabilityChanged, this, &DataTable::setAvailability);
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::updated, this, [this](const QJsonDocument &document) {
        m_document = document;
        refreshFiles();
    });
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] { if (isVisible() && !m_busy) refreshFiles(); });
    timer->start();
    refreshFiles();
}

void MoonTab::refreshFiles()
{
    QHash<QString, QJsonObject> metadata;
    for (const auto &moon : m_document.array()) {
        const auto object = moon.toObject();
        metadata.insert(moonId(object["id"].toString()), object);
    }
    m_files.clear();
    QList<TableRow> rows;
    for (const auto &file : QDir(moonDirectory()).entryInfoList(QDir::Files, QDir::Name)) {
        if (file.suffix().compare("moon", Qt::CaseInsensitive) != 0) continue;
        const QString id = file.completeBaseName();
        m_files.insert(id, file.absoluteFilePath());
        auto object = metadata.value(moonId(id));
        if (!object.contains("id")) object.insert("id", moonId(id));
        object.insert("moonFile", file.absoluteFilePath());
        rows.append({id, {}, object});
    }
    setRows(rows);
}

void MoonTab::showEvent(QShowEvent *event)
{
    DataTable::showEvent(event);
    refreshFiles();
    m_poller->refresh();
}

bool MoonTab::acceptsFiles(const QMimeData *mime) const
{
    if (m_busy || !mime->hasUrls() || mime->urls().isEmpty()) return false;
    for (const auto &url : mime->urls()) {
        const QFileInfo file(url.toLocalFile());
        if (!url.isLocalFile() || !file.isFile() || file.suffix().compare("moon", Qt::CaseInsensitive) != 0) return false;
    }
    return true;
}

void MoonTab::importDrop(const QMimeData *mime)
{
    if (!acceptsFiles(mime)) return;
    QStringList sources;
    for (const auto &url : mime->urls()) sources << url.toLocalFile();
    importFiles(sources);
}

void MoonTab::dragEnterEvent(QDragEnterEvent *event)
{
    if (acceptsFiles(event->mimeData())) event->acceptProposedAction();
    else event->ignore();
}

void MoonTab::dropEvent(QDropEvent *event)
{
    if (!acceptsFiles(event->mimeData())) { event->ignore(); return; }
    event->acceptProposedAction();
    importDrop(event->mimeData());
}

void MoonTab::addMoon(QWidget *dialogParent)
{
    if (m_busy) return;
    const QStringList sources = QFileDialog::getOpenFileNames(dialogParent ? dialogParent : this,
        QStringLiteral("选择 Moon 文件"), {}, "Moon (*.moon)");
    if (!sources.isEmpty()) importFiles(sources, {}, dialogParent);
}

void MoonTab::importFiles(const QStringList &sources, std::function<void(bool)> finished, QWidget *dialogParent)
{
    if (m_busy) { if (finished) finished(false); return; }
    const QPointer<QWidget> messageParent(dialogParent ? dialogParent : this);
    QList<QPair<QString, QString>> copies;
    QHash<QString, QString> submitted;
    for (const auto &source : sources) {
        const QFileInfo file(source);
        if (!file.isFile() || file.suffix().compare("moon", Qt::CaseInsensitive) != 0) continue;
        const QString destination = QDir(vusiif::ztgui::zerotier::moonWriteDirectory()).filePath(file.fileName());
        if (file.absoluteFilePath().compare(destination, Qt::CaseInsensitive) == 0) continue;
        const QString key = file.fileName().toLower();
        if (submitted.contains(key) || QFileInfo::exists(QDir(moonDirectory()).filePath(file.fileName()))) {
            if (QMessageBox::question(messageParent, QStringLiteral("替换 Moon 文件"),
                QStringLiteral("%1 已存在，确定替换并重启服务？").arg(file.fileName()), QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) continue;
        }
        submitted.insert(key, source);
        copies.append({source, destination});
    }
    if (copies.isEmpty()) { if (finished) finished(false); return; }
    m_busy = true;
    emit busyChanged(true);
    m_add->setEnabled(false);
    vusiif::ztgui::ui::setBusy(this, true);
    const QString script = vusiif::ztgui::zerotier::moonImportScript(copies);
    m_main->runElevatedScript(QStringLiteral("添加 Moon 并重启服务"), script, [this, finished, messageParent](bool ok) {
        m_busy = false;
        emit busyChanged(false);
        m_add->setEnabled(true);
        vusiif::ztgui::ui::setBusy(this, false);
        refreshFiles();
        if (!ok) QMessageBox::warning(messageParent ? messageParent.data() : this, QStringLiteral("添加 Moon"), QStringLiteral("文件复制或服务启动未完成，请查看 logs 文件夹中的本次启动日志。"));
        m_poller->refresh();
        if (finished) finished(ok);
    });
}

void MoonTab::deleteMoon(const QString &id)
{
    if (m_busy || !m_files.contains(id)) return;
    const QString path = m_files.value(id);
    const auto items = tree()->selectedItems();
    DeleteConfirmation confirmation(window(), items.size() == 1 ? items.first()->text(0) : id);
    if (confirmation.exec() != QDialog::Accepted) return;
    m_busy = true;
    emit busyChanged(true);
    m_add->setEnabled(false);
    vusiif::ztgui::ui::setBusy(this, true);
    const QString script = vusiif::ztgui::zerotier::moonDeleteScript(path);
    m_main->runElevatedScript(QStringLiteral("删除 Moon 并重启服务"), script, [this, id, path](bool ok) {
        m_busy = false;
        emit busyChanged(false);
        m_add->setEnabled(true);
        vusiif::ztgui::ui::setBusy(this, false);
        if (!QFileInfo::exists(path)) QSettings().remove("names/moons/" + id);
        refreshFiles();
        if (!ok) QMessageBox::warning(this, QStringLiteral("删除 Moon"), QStringLiteral("文件删除或服务启动未完成，请查看 logs 文件夹中的本次启动日志。"));
        m_poller->refresh();
    });
}

} // namespace vusiif::ztgui::ui
