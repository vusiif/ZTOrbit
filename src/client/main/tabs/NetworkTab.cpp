#include "NetworkTab.h"
#include "MoonTab.h"
#include "../NetworkBundle.h"
#include "../MainWindow.h"
#include "../ZeroTierClient.h"
#include "../NetworkMembers.h"
#include "../NetworkMemberRow.h"
#include <ztprotocol/NetworkAddress.h>
#include "../UiTransitions.h"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <memory>
#include <QVBoxLayout>

namespace vusiif::ztgui::ui {
NetworkTab::NetworkTab(MainWindow *mainWindow, QWidget *parent)
    : DataTable("networks", {"名称", "网络 ID", "状态", "分配的 IP"}, parent, true, false), m_main(mainWindow)
{
    setBadgeColumns({2});
    auto *title = new QLabel(QStringLiteral("网络"));
    title->setObjectName("sectionTitle");
    bodyLayout()->insertWidget(0, title);
    if (!QSettings().contains("tables/networks/header-v3")) {
        tree()->setColumnWidth(1, 185);
        tree()->setColumnWidth(2, 120);
    }
    tree()->setRootIsDecorated(true);
    tree()->setIndentation(16);
    tree()->setExpandsOnDoubleClick(false);
    m_membersTimer.setInterval(2000);
    connect(&m_membersTimer, &QTimer::timeout, this, &NetworkTab::refreshMembers);
    m_membersTimer.start();
    connect(tree(), &QTreeWidget::itemExpanded, this, [this] { refreshMembers(); });
    connect(tree(), &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        const auto id = item->data(0, Qt::UserRole).toString();
        delete m_memberRequests.take(id).data();
    });
    auto *joinRow = new QWidget;
    joinRow->setObjectName("joinNetworkRow");
    auto *joinLayout = new QHBoxLayout(joinRow);
    joinLayout->setContentsMargins(8, 6, 8, 6);
    m_join = new QPushButton(QStringLiteral("加入网络"));
    m_join->setObjectName("joinNetworkButton");
    joinLayout->addWidget(m_join);
    joinLayout->addStretch();
    setInlineAction(joinRow);
    auto *moons = new QPushButton(QStringLiteral("中转站"));
    moons->setObjectName("moonsButton");
    bodyLayout()->addWidget(moons, 0, Qt::AlignRight);
    connect(m_join, &QPushButton::clicked, this, &NetworkTab::showJoinDialog);
    connect(moons, &QPushButton::clicked, this, &NetworkTab::showMoons);
    connect(tree(), &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item->parent() || item == inlineActionItem()) return;
        if (!item->childCount()) {
            auto *child = new QTreeWidgetItem(item);
            child->setFlags(Qt::NoItemFlags);
            child->setFirstColumnSpanned(true);
        }
        item->setExpanded(!item->isExpanded());
    });
    tree()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree(), &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = tree()->itemAt(position);
        if (!item || item == inlineActionItem() || m_operating) return;
        if (item->parent()) {
            if (auto *row = qobject_cast<NetworkMemberRow *>(tree()->itemWidget(item, 0)))
                emit row->customContextMenuRequested(row->mapFromGlobal(tree()->viewport()->mapToGlobal(position)));
            return;
        }
        tree()->clearSelection();
        tree()->setCurrentItem(item);
        item->setSelected(true);
        const QString id = selectedId();
        const auto object = item->data(0, Qt::UserRole + 1).toJsonObject();
        AnimatedMenu menu(this);
        menu.setObjectName("networkMenu");
        menu.addAction(QStringLiteral("修改备注"), this, &DataTable::editRemark);
        menu.addAction(QStringLiteral("复制ID"), this, [id] { QApplication::clipboard()->setText(id); });
        QStringList addresses;
        for (const auto &address : object["assignedAddresses"].toArray()) {
            const QString ip = address.toString().section('/', 0, 0);
            if (!ip.isEmpty()) addresses.append(ip);
        }
        auto *copyIp = menu.addAction(QStringLiteral("本机ip"), this, [addresses] {
            QApplication::clipboard()->setText(addresses.join("\n"));
        });
        copyIp->setEnabled(!addresses.isEmpty());
        const QString moon = vusiif::ztgui::zerotier::matchingMoonFile(id);
        if (!moon.isEmpty()) menu.addAction(QStringLiteral("导出中转站"), this, [this, id, moon] { exportMoon(id, moon); });
        menu.addAction(QStringLiteral("退出网络"), this, [this, id] {
            if (QMessageBox::question(this, QStringLiteral("退出网络"), QStringLiteral("退出网络 %1？").arg(id),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) operate("leave", id);
        });
        menu.exec(tree()->viewport()->mapToGlobal(position));
    });
    m_poller = new vusiif::ztgui::zerotier::JsonPoller(this, "listnetworks");
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::availabilityChanged, this, &DataTable::setAvailability);
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::updated, this, [this](const QJsonDocument &document) {
        QList<TableRow> rows;
        for (const auto &network : document.array()) {
            const auto object = network.toObject();
            const QString id = object["nwid"].toString(object["id"].toString());
            QStringList addresses;
            for (const auto &address : object["assignedAddresses"].toArray()) addresses << address.toString();
            rows.append({id, {id, vusiif::ztgui::zerotier::translate(object["status"].toString()),
                addresses.isEmpty() ? QStringLiteral("—") : addresses.join(", ")}, object});
        }
        setRows(rows);
        for (const auto &id : m_memberRequests.keys())
            if (!networkItem(id)) delete m_memberRequests.take(id).data();
        for (int i = 0; i < tree()->topLevelItemCount(); ++i)
            if (tree()->topLevelItem(i) != inlineActionItem())
                tree()->topLevelItem(i)->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    });
}

QTreeWidgetItem *NetworkTab::networkItem(const QString &id) const
{
    for (int i = 0; i < tree()->topLevelItemCount(); ++i) {
        auto *item = tree()->topLevelItem(i);
        if (item != inlineActionItem() && item->data(0, Qt::UserRole).toString() == id) return item;
    }
    return nullptr;
}

void NetworkTab::refreshMembers()
{
    if (!isVisible()) return;
    for (int i = 0; i < tree()->topLevelItemCount(); ++i) {
        auto *item = tree()->topLevelItem(i);
        if (item == inlineActionItem() || !item->isExpanded()) continue;
        const QString id = item->data(0, Qt::UserRole).toString();
        if (m_memberRequests.value(id)) continue;
        const auto server = protocol::networkServer(item->data(0, Qt::UserRole + 1).toJsonObject());
        auto *context = new QObject(this);
        m_memberRequests.insert(id, context);
        zerotier::requestMembers(context, server, id, [this, id, context, server](const util::JsonResult &result) {
            m_memberRequests.remove(id);
            context->deleteLater();
            auto *current = networkItem(id);
            if (!current || !current->isExpanded()) return;
            if (protocol::networkServer(current->data(0, Qt::UserRole + 1).toJsonObject()) != server) return;
            if (!result.ok()) {
                // Retain existing members and editors during a transient failure.
                for (int j = current->childCount() - 1; j >= 0; --j)
                    if (current->child(j)->data(0, Qt::UserRole).toString().isEmpty()) delete current->child(j);
                auto *row = new QTreeWidgetItem(current);
                row->setFirstColumnSpanned(true);
                row->setText(0, QStringLiteral("成员列表不可用：%1（2 秒后重试）").arg(result.error));
                row->setFlags(Qt::ItemIsEnabled);
                row->setToolTip(0, row->text(0));
            } else updateMembers(current, result.document.array());
            setContentSized(true);
        });
    }
}

void NetworkTab::updateMembers(QTreeWidgetItem *network, const QJsonArray &members)
{
    QHash<QString, QTreeWidgetItem *> existing;
    for (int i = network->childCount() - 1; i >= 0; --i) {
        auto *item = network->child(i);
        const QString id = item->data(0, Qt::UserRole).toString();
        if (id.isEmpty()) delete item;
        else existing.insert(id, item);
    }
    QSet<QString> seen;
    for (const auto &value : members) {
        const auto member = value.toObject();
        const QString id = member.value("nodeId").toString().toLower();
        if (!QRegularExpression("^[0-9a-f]{10}$").match(id).hasMatch() || seen.contains(id)) continue;
        seen.insert(id);
        auto *item = existing.take(id);
        if (!item) {
            item = new QTreeWidgetItem(network);
            item->setFirstColumnSpanned(true);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setData(0, Qt::UserRole, id);
            item->setSizeHint(0, QSize(0, 44));
            auto *row = new NetworkMemberRow(id);
            tree()->setItemWidget(item, 0, row);
            connect(row, &NetworkMemberRow::remarkChanged, this, [this](const QString &memberId) {
                for (int n = 0; n < tree()->topLevelItemCount(); ++n) {
                    auto *network = tree()->topLevelItem(n);
                    for (int m = 0; m < network->childCount(); ++m) {
                        auto *child = network->child(m);
                        if (child->data(0, Qt::UserRole).toString() != memberId) continue;
                        if (auto *memberRow = qobject_cast<NetworkMemberRow *>(tree()->itemWidget(child, 0))) {
                            memberRow->updateMember(child->data(0, Qt::UserRole + 1).toJsonObject());
                            child->setText(0, memberRow->memberName());
                        }
                    }
                }
            });
        }
        item->setData(0, Qt::UserRole + 1, member);
        auto *row = qobject_cast<NetworkMemberRow *>(tree()->itemWidget(item, 0));
        row->updateMember(member);
        item->setText(0, row->memberName());
    }
    for (auto *item : existing) delete item;
    if (!network->childCount()) {
        auto *empty = new QTreeWidgetItem(network);
        empty->setFirstColumnSpanned(true);
        empty->setFlags(Qt::ItemIsEnabled);
        empty->setText(0, QStringLiteral("暂无网络成员"));
    }
}

void NetworkTab::showEvent(QShowEvent *event)
{
    DataTable::showEvent(event);
    m_poller->refresh();
}

void NetworkTab::showJoinDialog()
{
    if (m_operating) return;
    if (m_joinDialog) { m_joinDialog->raise(); m_joinDialog->activateWindow(); return; }
    m_joinDialog = new AnimatedDialog(this);
    m_joinDialog->setObjectName("joinNetworkDialog");
    m_joinDialog->setWindowTitle(QStringLiteral("加入网络"));
    m_joinDialog->setAttribute(Qt::WA_DeleteOnClose);
    m_joinDialog->setWindowModality(Qt::WindowModal);
    m_joinDialog->resize(420, 170);
    auto *layout = new QVBoxLayout(m_joinDialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(20);
    m_input = new QLineEdit;
    m_input->setObjectName("networkIdInput");
    m_input->setPlaceholderText(QStringLiteral("16 位网络 ID"));
    m_input->setMaxLength(16);
    layout->addWidget(m_input);
    auto *actions = new QHBoxLayout;
    auto *import = new QPushButton(QStringLiteral("导入中转站"));
    import->setObjectName("importMoonButton");
    auto *confirm = new QPushButton(QStringLiteral("确认"));
    confirm->setObjectName("confirmJoinButton");
    confirm->setAutoDefault(false);
    import->setAutoDefault(false);
    actions->addWidget(import);
    actions->addStretch();
    actions->addWidget(confirm);
    layout->addLayout(actions);
    connect(import, &QPushButton::clicked, this, &NetworkTab::importMoon);
    const auto join = [this] { if (m_input) operate("join", m_input->text().trimmed().toLower()); };
    connect(confirm, &QPushButton::clicked, this, join);
    connect(m_input, &QLineEdit::returnPressed, this, join);
    m_joinDialog->show();
    m_input->setFocus();
}

MoonTab *NetworkTab::moonTab()
{
    if (!m_moonDialog) {
        m_moonDialog = new AnimatedDialog(this, Qt::Tool);
        m_moonDialog->setObjectName("moonDialog");
        m_moonDialog->setWindowTitle(QStringLiteral("中转站"));
        m_moonDialog->resize(480, 400);
        auto *layout = new QVBoxLayout(m_moonDialog);
        m_moon = new MoonTab(m_main, m_moonDialog);
        layout->addWidget(m_moon);
        connect(m_moon, &MoonTab::busyChanged, this, [this](bool busy) {
            if (m_joinDialog) vusiif::ztgui::ui::setBusy(m_joinDialog, busy);
        });
    }
    return m_moon;
}

void NetworkTab::showMoons()
{
    moonTab();
    m_moonDialog->show();
    m_moonDialog->raise();
    m_moonDialog->activateWindow();
}

void NetworkTab::exportMoon(const QString &networkId, const QString &path)
{
    QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("导出中转站"),
        networkId + ".ztbundle", QStringLiteral("中转站配置包 (*.ztbundle)"));
    if (destination.isEmpty()) return;
    if (!destination.endsWith(".ztbundle", Qt::CaseInsensitive)) destination += ".ztbundle";
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法读取 Moon 文件。"));
        return;
    }
    const QByteArray contents = source.read(4 * 1024 * 1024 + 1);
    const QByteArray bundle = vusiif::ztgui::zerotier::bundle::encode({networkId, QFileInfo(path).fileName(), contents});
    QSaveFile output(destination);
    if (source.error() != QFileDevice::NoError || bundle.isEmpty() || !output.open(QIODevice::WriteOnly)
        || output.write(bundle) != bundle.size() || !output.commit())
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法导出中转站配置包。"));
}

void NetworkTab::importMoon()
{
    if (m_operating) return;
    QWidget *parent = m_joinDialog ? static_cast<QWidget *>(m_joinDialog.data()) : this;
    const QString source = QFileDialog::getOpenFileName(parent, QStringLiteral("导入中转站"), {},
        QStringLiteral("中转站配置 (*.ztbundle *.moon);;中转站配置包 (*.ztbundle);;Moon (*.moon)"));
    importConfigurationFile(source);
}

void NetworkTab::importConfigurationFile(const QString &source)
{
    if (m_operating || source.isEmpty()) return;
    QWidget *parent = m_joinDialog ? static_cast<QWidget *>(m_joinDialog.data()) : this;
    if (source.endsWith(".moon", Qt::CaseInsensitive)) {
        moonTab()->importFiles({source}, {}, parent);
        return;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(parent, QStringLiteral("导入失败"), QStringLiteral("无法读取配置包。"));
        return;
    }
    vusiif::ztgui::zerotier::bundle::Configuration configuration;
    QString error;
    const QByteArray bytes = input.read(vusiif::ztgui::zerotier::bundle::MaxFileSize + 1);
    if (input.error() != QFileDevice::NoError || !vusiif::ztgui::zerotier::bundle::decode(bytes, configuration, error)) {
        QMessageBox::warning(parent, QStringLiteral("导入失败"), error.isEmpty() ? QStringLiteral("配置包读取失败。") : error);
        return;
    }
    // Keep the decoded, signed Moon bytes alive until the elevated copy finishes.
    auto temporary = std::make_shared<QTemporaryDir>();
    const QString moonPath = temporary->filePath(configuration.moonFileName);
    QFile moon(moonPath);
    if (!temporary->isValid() || !moon.open(QIODevice::WriteOnly)
        || moon.write(configuration.moonContents) != configuration.moonContents.size() || !moon.flush()) {
        QMessageBox::warning(parent, QStringLiteral("导入失败"), QStringLiteral("无法创建临时 Moon 文件。"));
        return;
    }
    moon.close();
    if (m_input) m_input->setText(configuration.networkId);
    m_operating = true;
    moonTab()->importFiles({moonPath}, [this, temporary, id = configuration.networkId](bool ok) {
        m_operating = false;
        if (!ok) return;
        operate("join", id, true);
    }, parent);
}

void NetworkTab::operate(const QString &action, const QString &id, bool moonConfigured)
{
    if (m_operating) return;
    if (!QRegularExpression("^[0-9a-fA-F]{16}$").match(id).hasMatch()) {
        QMessageBox::warning(m_joinDialog ? static_cast<QWidget *>(m_joinDialog.data()) : this,
            QStringLiteral("网络 ID"), QStringLiteral("请输入完整的 16 位十六进制网络 ID。"));
        return;
    }
    m_operating = true;
    // Capture the dialog's enabled state before disabling its parent page.
    if (m_joinDialog) vusiif::ztgui::ui::setBusy(m_joinDialog, true);
    vusiif::ztgui::ui::setBusy(this, true);
    const QPointer<AnimatedDialog> submittedDialog = m_joinDialog;
    vusiif::ztgui::zerotier::command(this, {action, id}, [this, action, submittedDialog, moonConfigured](bool ok, const QString &output) {
        m_operating = false;
        vusiif::ztgui::ui::setBusy(this, false);
        if (submittedDialog) vusiif::ztgui::ui::setBusy(submittedDialog, false);
        m_main->appendOutput(output + '\n');
        if (!ok) QMessageBox::warning(submittedDialog ? static_cast<QWidget *>(submittedDialog.data()) : this,
            QStringLiteral("操作失败"), (moonConfigured ? QStringLiteral("中转站已配置，但加入网络失败。可再次点击确认重试。\n") : QString())
                + (output.isEmpty() ? QStringLiteral("请检查服务和管理员权限。") : output));
        else {
            if (action == "join" && submittedDialog) submittedDialog->accept();
            m_poller->refresh();
        }
    });
}

} // namespace vusiif::ztgui::ui
