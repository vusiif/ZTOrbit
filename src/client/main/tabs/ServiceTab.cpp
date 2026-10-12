#include "ServiceTab.h"
#include "../MainWindow.h"
#include "../UiTransitions.h"
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace vusiif::ztgui::ui {
ServiceTab::ServiceTab(MainWindow *mainWindow, QWidget *parent)
    : QWidget(parent), m_main(mainWindow)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->setSpacing(14);
    auto *title = new QLabel(QStringLiteral("ZeroTier"));
    title->setObjectName("serviceTitle");
    layout->addWidget(title);
    auto *summary = new QHBoxLayout;
    auto *identity = new QVBoxLayout;
    identity->setSpacing(4);
    m_node = new QLabel(QStringLiteral("—"));
    m_node->setObjectName("nodeId");
    m_node->setCursor(Qt::PointingHandCursor);
    m_node->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_node->setOpenExternalLinks(false);
    connect(m_node, &QLabel::linkActivated, this, [](const QString &id) { QApplication::clipboard()->setText(id); });
    identity->addWidget(m_node);
    m_version = new QLabel;
    m_version->setObjectName("nodeVersion");
    identity->addWidget(m_version);
    summary->addLayout(identity);
    summary->addStretch();
    auto *actions = new QHBoxLayout;
    actions->setSpacing(0);
    m_action = new QPushButton;
    m_action->setObjectName("serviceAction");
    m_action->setMinimumWidth(110);
    m_action->setFixedHeight(36);
    m_dropdown = new QToolButton;
    m_dropdown->setObjectName("serviceDropdown");
    m_dropdown->setArrowType(Qt::DownArrow);
    m_dropdown->setFixedWidth(32);
    m_dropdown->setFixedHeight(36);
    m_dropdown->setAccessibleName(QStringLiteral("更多服务操作"));
    m_menu = new AnimatedMenu(this);
    m_menu->setObjectName("serviceMenu");
    connect(m_dropdown, &QToolButton::clicked, this, [this] {
        m_menu->popup(m_dropdown->mapToGlobal(QPoint(0, m_dropdown->height())));
    });
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (!m_status.known) return;
        if (!m_status.exists) packageAction(false);
        else changeService(m_status.running ? "stop" : "start");
    });
    actions->addWidget(m_action);
    actions->addWidget(m_dropdown);
    summary->addLayout(actions);
    layout->addLayout(summary);
    m_statusWarning = new QLabel(QStringLiteral("服务异常，请尝试重启。"));
    m_statusWarning->setObjectName("serviceStatusWarning");
    m_statusWarning->setWordWrap(true);
    layout->addWidget(m_statusWarning);
    m_statusWarning->hide();
    m_feedback = new QLabel;
    m_feedback->setObjectName("serviceFeedback");
    m_feedback->setWordWrap(true);
    layout->addWidget(m_feedback);
    m_feedback->hide();
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &ServiceTab::updateInstallation);
    timer->start();
    updateInstallation();
}

void ServiceTab::setNodeInfo(const QJsonObject &object)
{
    const QString id = object["address"].toString();
    m_node->setText(id.isEmpty() ? QStringLiteral("—")
        : QStringLiteral("<a href=\"%1\" style=\"color: %2; text-decoration: none\">%1</a>")
            .arg(id.toHtmlEscaped(), palette().color(QPalette::WindowText).name()));
    const QString version = object["version"].toString();
    m_version->setText(version.isEmpty() ? QString() : "v" + version);
    // Match the final field of `zerotier-cli info`; TCP fallback takes priority
    // over the online flag, including when the node is otherwise online.
    const QString status = object["tcpFallbackActive"].toBool() ? QStringLiteral("TUNNELED")
        : object["online"].toBool() ? QStringLiteral("ONLINE") : QStringLiteral("OFFLINE");
    m_tunneled = status != QStringLiteral("OFFLINE") && status != QStringLiteral("ONLINE");
    updateWarning();
}

void ServiceTab::setApiAvailability(bool available)
{
    m_apiAvailable = available;
    updateWarning();
}

void ServiceTab::updateWarning()
{
    const bool unavailable = m_status.known && m_status.running && !m_apiAvailable;
    m_statusWarning->setText(unavailable
#ifdef Q_OS_WIN
        ? QStringLiteral("服务已运行但本地接口未响应。请重启服务，ZTGUI 会检查并避开 Windows 保留端口。")
#else
        ? QStringLiteral("服务已运行但本地接口未响应，请检查服务日志与访问权限。")
#endif
        : QStringLiteral("服务异常，请尝试重启。"));
    m_statusWarning->setVisible(unavailable || (m_apiAvailable && m_tunneled));
}

void ServiceTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateInstallation();
}

void ServiceTab::updateInstallation()
{
    if (m_busy) return;
    const auto status = m_main->serviceStatus();
    const bool changed = status.exists != m_status.exists || status.running != m_status.running || status.known != m_status.known;
    m_status = status;
    updateWarning();
    if (!status.known) { vusiif::ztgui::ui::setText(m_feedback, QStringLiteral("无法读取服务状态，请检查管理员权限。")); m_feedback->show(); }
    else if (m_feedback->text() == QStringLiteral("无法读取服务状态，请检查管理员权限。")) { vusiif::ztgui::ui::setText(m_feedback, {}); m_feedback->hide(); }
    // Never replace live menu actions underneath the pointer.
    if (changed || m_menu->actions().isEmpty()) updateButtons();
}

void ServiceTab::updateButtons()
{
    m_action->setText(!m_status.exists ? QStringLiteral("安装")
        : m_status.running ? QStringLiteral("停止") : QStringLiteral("启动"));
    m_action->setEnabled(!m_busy && m_status.known);
    m_dropdown->setVisible(m_status.exists);
    m_dropdown->setEnabled(!m_busy && m_status.known);
    m_menu->clear();
    if (!m_status.exists) return;
    if (m_status.running) m_menu->addAction(QStringLiteral("重启"), this, [this] { changeService("restart"); });
    m_menu->addAction(QStringLiteral("卸载"), this, [this] { packageAction(true); });
}

void ServiceTab::changeService(const QString &action)
{
    if (m_busy) return;
    m_busy = true;
    updateButtons();
    vusiif::ztgui::ui::setBusy(this, true);
    vusiif::ztgui::ui::setText(m_feedback, {});
    m_feedback->hide();
    m_main->runService(action, [this](bool ok) {
        m_busy = false;
        vusiif::ztgui::ui::setBusy(this, false);
        updateInstallation();
        updateButtons();
        vusiif::ztgui::ui::setText(m_feedback, ok ? QString() : QStringLiteral("操作失败，请查看 logs 文件夹中的本次启动日志。"));
        m_feedback->setVisible(!ok);
    });
}

void ServiceTab::packageAction(bool uninstall)
{
    if (m_busy) return;
#ifdef Q_OS_MACOS
    const QString winget;
#else
    const QString winget = QStandardPaths::findExecutable("winget");
    if (winget.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法使用 winget"), QStringLiteral("未找到 winget，请安装 Windows 应用安装程序后重试。"));
        return;
    }
#endif
    const QString title = uninstall ? QStringLiteral("卸载 ZeroTier") : QStringLiteral("安装 ZeroTier");
    const QString text = uninstall
        ? QStringLiteral("卸载 ZeroTier，并删除 %1 中的全部数据（包括节点身份和网络配置），确定继续？").arg(vusiif::ztgui::zerotier::dataDirectory())
#ifdef Q_OS_MACOS
        : QStringLiteral("从 ZeroTier 官方下载并安装 macOS 安装包，是否继续？");
#else
        : QStringLiteral("使用 winget 下载并安装 ZeroTier，并接受软件包与来源协议，是否继续？");
#endif
    QMessageBox confirmation(QMessageBox::Question, title, text,
                             QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setDefaultButton(QMessageBox::No);
    confirmation.setButtonText(QMessageBox::Yes, uninstall ? QStringLiteral("卸载并删除数据") : QStringLiteral("继续安装"));
    confirmation.setButtonText(QMessageBox::No, QStringLiteral("取消"));
    if (confirmation.exec() != QMessageBox::Yes) return;
    m_busy = true;
    updateButtons();
    vusiif::ztgui::ui::setBusy(this, true);
    vusiif::ztgui::ui::setText(m_feedback, {});
    m_feedback->hide();
    m_main->runElevatedScript(title, vusiif::ztgui::zerotier::packageScript(uninstall, winget), [this, uninstall](bool ok) {
        m_busy = false;
        vusiif::ztgui::ui::setBusy(this, false);
        updateInstallation();
        updateButtons();
        vusiif::ztgui::ui::setText(m_feedback, ok ? QString() : QStringLiteral("操作失败，请查看 logs 文件夹中的本次启动日志。"));
        m_feedback->setVisible(!ok);

    });
}

} // namespace vusiif::ztgui::ui
