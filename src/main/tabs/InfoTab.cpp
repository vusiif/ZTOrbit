#include "InfoTab.h"
#include "../ZeroTierClient.h"
#include <QShowEvent>

namespace vusiif::ztgui::ui {
InfoTab::InfoTab(MainWindow *mainWindow, QWidget *parent)
    : DataTable("info", {"信息", "值"}, parent, false)
{
    Q_UNUSED(mainWindow)
    tree()->setColumnWidth(0, 210);
    tree()->setColumnWidth(1, 420);
    m_poller = new vusiif::ztgui::zerotier::JsonPoller(this, "info", true);
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::availabilityChanged, this, &DataTable::setAvailability);
    connect(m_poller, &vusiif::ztgui::zerotier::JsonPoller::updated, this, [this](const QJsonDocument &document) {
        const auto object = document.object();
        QList<TableRow> rows;
        for (const QString &key : {QString("address"), QString("online"), QString("version"), QString("tcpFallbackActive")})
            rows.append({key, {vusiif::ztgui::zerotier::translate(key), vusiif::ztgui::zerotier::jsonText(object[key])}, object});
        setRows(rows);
    });
}

void InfoTab::showEvent(QShowEvent *event)
{
    DataTable::showEvent(event);
    m_poller->refresh();
}

} // namespace vusiif::ztgui::ui
