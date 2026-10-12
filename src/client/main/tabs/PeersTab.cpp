#include "PeersTab.h"
#include "../ZeroTierClient.h"
#include "../UiTransitions.h"
#include <QApplication>
#include <QClipboard>
#include <algorithm>
#include <QJsonArray>
#include <QHeaderView>
#include <QShowEvent>
#include <QSettings>

namespace vusiif::ztgui::ui {
PeersTab::PeersTab(MainWindow *mainWindow, QWidget *parent)
    : DataTable("peers", {"名称", "角色", "连接", "IP 地址", "延迟（ms）"}, parent, true, false)
{
    Q_UNUSED(mainWindow)
    setBadgeColumns({1, 2});
    tree()->setSortingEnabled(false);
    tree()->header()->setSortIndicatorShown(false);
    if (!QSettings().contains("tables/peers/header-v3")) {
        tree()->setColumnWidth(1, 95);
        tree()->setColumnWidth(2, 95);
        tree()->setColumnWidth(3, 185);
        tree()->setColumnWidth(4, 115);
    }
    tree()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree(), &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = tree()->itemAt(position);
        if (!item) return;
        tree()->clearSelection();
        tree()->setCurrentItem(item);
        item->setSelected(true);
        const QString id = selectedId();
        AnimatedMenu menu(this);
        menu.setObjectName("peerMenu");
        menu.addAction(id, this, [id] { QApplication::clipboard()->setText(id); });
        menu.addAction(QStringLiteral("修改备注"), this, &DataTable::editRemark);
        menu.exec(tree()->viewport()->mapToGlobal(position));
    });
    m_poller = new vusiif::ztgui::zerotier::JsonPoller(this, "listpeers");
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::availabilityChanged, this, &DataTable::setAvailability);
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::updated, this, [this](const QJsonDocument &document) {
        QList<TableRow> rows;
        for (const auto &peer : document.array()) {
            const auto object = peer.toObject();
            const QString role = object["role"].toString();
            const int latency = object["latency"].toInt(-1);
            if (role == "LEAF" && latency < 0) continue;
            bool direct = false;
            QStringList addresses;
            for (const auto &path : object["paths"].toArray()) {
                const auto connection = path.toObject();
                if (connection["active"].toBool()) direct = true;
                QString address = connection["address"].toString().section('/', 0, 0);
                if (address.startsWith('[') && address.endsWith(']')) address = address.mid(1, address.size() - 2);
                if (!address.isEmpty() && !addresses.contains(address)) addresses << address;
            }
            const QString id = object["address"].toString();
            rows.append({id, {vusiif::ztgui::zerotier::translate(role),
                latency < 0 ? QStringLiteral("不可达") : vusiif::ztgui::zerotier::translate(direct ? "DIRECT" : "RELAY"),
                addresses.isEmpty() ? QStringLiteral("—") : addresses.join(", "),
                latency < 0 ? QStringLiteral("—") : QString::number(latency)}, object});
        }
        const auto rank = [](const TableRow &row) {
            const QString role = row.details["role"].toString();
            const QString connection = row.cells.value(1);
            return (role == "PLANET" ? 10 : 0) + (connection == QStringLiteral("直链") ? 0
                : connection == QStringLiteral("中转") ? 1 : 2);
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const TableRow &a, const TableRow &b) { return rank(a) < rank(b); });
        setRows(rows);
    });
}

void PeersTab::showEvent(QShowEvent *event)
{
    DataTable::showEvent(event);
    m_poller->refresh();
}

} // namespace vusiif::ztgui::ui
