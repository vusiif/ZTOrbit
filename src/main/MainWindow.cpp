#include "MainWindow.h"
#include "AppLog.h"
#include "AppStyle.h"
#include "UiTransitions.h"
#include "DataTable.h"
#include "ZeroTierClient.h"
#include "MacPlatform.h"
#include "plugins/PluginRegistry.h"
#include "plugins/PluginLoader.h"
#include "tabs/ServiceTab.h"
#include "tabs/PeersTab.h"
#include "tabs/NetworkTab.h"
#include "tabs/MoonTab.h"

#include <QCloseEvent>
#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QPainter>
#include <QPushButton>
#include <QStyleHints>
#include <QJsonObject>
#include <QVariantAnimation>
#include <QPointer>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QDir>
#include <QLabel>
#include <QHeaderView>
#include <QListWidget>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QTreeWidget>
#include <QSettings>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QSplitter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace vusiif::ztgui::ui {
namespace {
class PluginList : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QTreeWidget::mouseReleaseEvent(event);
        if (event->button() != Qt::RightButton) return;
        const QPoint position = event->position().toPoint();
        const QPersistentModelIndex index(indexAt(position));
        if (!index.isValid()) return;
        // Qt's active-popup mouse path skips automatic context-menu generation.
        // Wait until this release has finished so the new popup cannot consume it.
        QTimer::singleShot(0, this, [this, position, index] {
            if (isVisible() && index.isValid() && indexAt(position) == index)
                emit customContextMenuRequested(position);
        });
    }
};

class SettingsButton : public QPushButton {
public:
    SettingsButton() { setCursor(Qt::PointingHandCursor); }
protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(rect().center());
        painter.setPen(QPen(palette().color(QPalette::WindowText), 3, Qt::SolidLine, Qt::SquareCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(0, 0), 7, 7);
        for (int i = 0; i < 8; ++i) {
            painter.drawLine(QPointF(0, -7), QPointF(0, -11));
            painter.rotate(45);
        }
    }
};

class ThemeSwitch : public QAbstractButton {
public:
    explicit ThemeSwitch(QWidget *parent = nullptr) : QAbstractButton(parent)
    {
        setObjectName("themeSwitch");
        setAccessibleName(QStringLiteral("切换浅色或深色主题"));
        setFixedSize(68, 34);
        setCursor(Qt::PointingHandCursor);
        setCheckable(true);
        m_position = qApp->property("darkTheme").toBool() ? 1.0 : 0.0;
        setChecked(m_position > 0.5);
        m_motion.setDuration(180);
        m_motion.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_motion, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_position = value.toReal(); update();
        });
    }
    void sync()
    {
        const bool dark = qApp->property("darkTheme").toBool();
        setChecked(dark);
        m_motion.stop();
        m_motion.setStartValue(m_position);
        m_motion.setEndValue(dark ? 1.0 : 0.0);
        m_motion.start();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        const bool dark = qApp->property("darkTheme").toBool();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.setBrush(dark ? QColor("#354e61") : QColor("#e3eff8"));
        painter.drawRoundedRect(QRectF(1, 1, 66, 32), 16, 16);
        if (dark) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#e1ebf3"));
            painter.drawEllipse(QPointF(17, 17), 7, 7);
            painter.setBrush(QColor("#354e61"));
            painter.drawEllipse(QPointF(20, 14), 6, 6);
        } else {
            painter.setPen(QPen(QColor("#925b0d"), 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(51, 17), 4, 4);
            painter.save(); painter.translate(51, 17);
            for (int i = 0; i < 8; ++i) { painter.drawLine(QPointF(0, -7), QPointF(0, -9)); painter.rotate(45); }
            painter.restore();
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark ? QColor("#9cceee") : QColor("#ffffff"));
        painter.drawEllipse(QPointF(17 + 34 * m_position, 17), 12, 12);
    }
private:
    qreal m_position = 0;
    QVariantAnimation m_motion;
};
}

MainWindow::MainWindow(QWidget *parent, const QString &pluginDirectory) : QMainWindow(parent)
{
    m_plugins = new vusiif::ztgui::plugins::PluginRegistry(this);
    setupUi();
#ifdef ZEROTIER_TESTING
    if (!pluginDirectory.isEmpty())
#endif
    {
        m_pluginLoader = new vusiif::ztgui::plugins::PluginLoader;
        for (const auto &message : m_pluginLoader->scan(pluginDirectory.isEmpty()
                ? QCoreApplication::applicationDirPath() + "/plugins" : pluginDirectory, *m_plugins))
            appendOutput(message + '\n');
    }
    vusiif::ztgui::zerotier::startServiceMonitor(this);
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    if (!vusiif::ztgui::zerotier::executable().isEmpty()) QTimer::singleShot(0, this, [this] {
        auto *page = m_pages->currentWidget();
        vusiif::ztgui::ui::setBusy(page, true);
        runElevatedScript(QStringLiteral("授权读取 ZeroTier"), vusiif::ztgui::zerotier::wrapScript({}), [page](bool) {
            vusiif::ztgui::ui::setBusy(page, false);
        });
    });
#endif
}

MainWindow::~MainWindow()
{
    // Release plugin widgets and callbacks before the main window's UI teardown.
    m_navigating = true;
    disconnect(m_navigation, nullptr, this, nullptr);
    disconnect(m_plugins, nullptr, this, nullptr);
    // Loader waits for creation tasks, removes pages and runs onDestroy while
    // the registry and host services are still valid.
    delete m_pluginLoader;
    m_pluginLoader = nullptr;
    delete m_plugins;
    m_plugins = nullptr;
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("ZTGUI"));
    resize(1120, 720);
    setMinimumSize(760, 480);
    setAcceptDrops(true);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    m_horizontal = new QSplitter(Qt::Horizontal);
    m_horizontal->setObjectName("mainSplitter");
    m_horizontal->setChildrenCollapsible(false);
    m_horizontal->setHandleWidth(7);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setMinimumWidth(130);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(10, 14, 10, 10);
    auto *brandRow = new QHBoxLayout;
    auto *online = new QLabel;
    online->setObjectName("onlineIndicator");
    online->setFixedSize(10, 10);
    online->setStyleSheet("background: #d95059; border-radius: 5px;");
    online->setAccessibleName(QStringLiteral("离线"));
    auto *brand = new QLabel(QStringLiteral("ZTGUI"));
    brand->setObjectName("brand");
    brandRow->addWidget(online);
    brandRow->addWidget(brand);
    brandRow->addStretch();
    auto *version = new QLabel(QStringLiteral("v" ZEROTIER_GUI_VERSION));
    version->setObjectName("sidebarVersion");
    brandRow->addWidget(version, 0, Qt::AlignBottom);
    sideLayout->addLayout(brandRow);
    m_navigation = new QListWidget;
    m_navigation->setObjectName("navigation");
    m_navigation->setMinimumWidth(0);
    m_navigation->setTextElideMode(Qt::ElideNone);
    enableDragScrolling(m_navigation);
    sideLayout->addWidget(m_navigation, 1);
    auto *settingsButton = new SettingsButton;
    settingsButton->setObjectName("settingsButton");
    settingsButton->setAccessibleName(QStringLiteral("设置"));
    settingsButton->setFixedSize(40, 40);
    sideLayout->addWidget(settingsButton, 0, Qt::AlignLeft);
    auto *settings = new AnimatedDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    settings->setObjectName("settingsPopup");
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(16, 16, 16, 16);
    settingsLayout->setSpacing(12);
    auto *themeRow = new QHBoxLayout;
    themeRow->addWidget(new QLabel(QStringLiteral("主题")));
    themeRow->addStretch();
    auto *theme = new ThemeSwitch;
    themeRow->addWidget(theme);
    settingsLayout->addLayout(themeRow);
    auto *system = new QCheckBox(QStringLiteral("跟随系统"));
    system->setObjectName("followSystemTheme");
    system->setChecked(QSettings().value("appearance/theme", "system").toString() == "system");
    settingsLayout->addWidget(system);
    auto *plugins = new QPushButton(QStringLiteral("插件"));
    plugins->setObjectName("pluginsButton");
    settingsLayout->addWidget(plugins);
    auto *about = new QPushButton(QStringLiteral("关于"));
    about->setObjectName("aboutButton");
    settingsLayout->addWidget(about);
    auto *pluginDialog = new AnimatedDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    pluginDialog->setObjectName("pluginsPopup");
    pluginDialog->setWindowTitle(QStringLiteral("插件"));
    pluginDialog->resize(520, 360);
    auto *pluginLayout = new QVBoxLayout(pluginDialog);
    pluginLayout->setContentsMargins(20, 20, 20, 20);
    pluginLayout->setSpacing(12);
    auto *pluginTitle = new QLabel(QStringLiteral("插件"));
    pluginTitle->setObjectName("sectionTitle");
    pluginLayout->addWidget(pluginTitle);
    auto *pluginList = new PluginList;
    pluginList->setObjectName("pluginList");
    pluginList->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("版本"), QStringLiteral("状态")});
    pluginList->setRootIsDecorated(false);
    pluginList->setAlternatingRowColors(true);
    pluginList->header()->resizeSection(0, 220);
    pluginList->header()->resizeSection(1, 90);
    pluginList->header()->setStretchLastSection(true);
    enableDragScrolling(pluginList);
    pluginLayout->addWidget(pluginList, 1);
    auto *emptyPlugins = new QLabel(QStringLiteral("尚未加载插件"));
    emptyPlugins->setObjectName("emptyPluginsHint");
    pluginLayout->addWidget(emptyPlugins);
    auto *pluginHint = new QLabel(QStringLiteral("增减插件后需重启程序。"));
    pluginHint->setObjectName("sidebarHint");
    pluginLayout->addWidget(pluginHint);
    auto *closePlugins = new QPushButton(QStringLiteral("关闭"));
    closePlugins->setObjectName("closePluginsButton");
    pluginLayout->addWidget(closePlugins, 0, Qt::AlignRight);
    connect(closePlugins, &QPushButton::clicked, pluginDialog, &QDialog::reject);
    const auto refreshPlugins = [this, pluginList, emptyPlugins] {
        pluginList->clear();
        for (const auto &info : m_plugins->plugins()) {
            QString state;
            switch (m_plugins->preparationState(info.id)) {
            case vusiif::ztgui::plugins::PreparationState::Ready: state = QStringLiteral("已就绪"); break;
            case vusiif::ztgui::plugins::PreparationState::Preparing: state = QStringLiteral("初始化中"); break;
            case vusiif::ztgui::plugins::PreparationState::Failed: state = QStringLiteral("初始化失败"); break;
            }
            auto *item = new QTreeWidgetItem(pluginList, {info.name, info.version, state});
            item->setData(0, Qt::UserRole, info.id);
            item->setToolTip(0, info.id);
            item->setToolTip(2, m_plugins->preparationMessage(info.id));
        }
        emptyPlugins->setVisible(pluginList->topLevelItemCount() == 0);
    };
    connect(m_plugins, &vusiif::ztgui::plugins::PluginRegistry::menusChanged, pluginDialog, refreshPlugins);
    pluginList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pluginList, &QTreeWidget::customContextMenuRequested, pluginDialog,
            [this, pluginList, pluginDialog](const QPoint &position) {
        for (auto *existing : pluginDialog->findChildren<QMenu *>("pluginContextMenu", Qt::FindDirectChildrenOnly))
            if (existing->isVisible()) return;
        auto *item = pluginList->itemAt(position);
        if (!item) return;
        pluginList->setCurrentItem(item);
        vusiif::ztgui::api::PluginInfo info {item->data(0, Qt::UserRole).toString(), item->text(0), item->text(1)};
        bool loaded = false;
        for (const auto &registered : m_plugins->plugins()) {
            if (registered.id == info.id) { info = registered; loaded = true; break; }
        }
        auto *menu = new AnimatedMenu(pluginDialog);
        menu->setObjectName("pluginContextMenu");
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        auto *load = menu->addAction(loaded ? QStringLiteral("卸载") : QStringLiteral("加载"));
        load->setObjectName("togglePluginLoadAction");
        load->setEnabled(false);
        auto *aboutPlugin = menu->addAction(QStringLiteral("关于"));
        aboutPlugin->setObjectName("aboutPluginAction");
        connect(aboutPlugin, &QAction::triggered, pluginDialog, [pluginDialog, info, loaded] {
            // Native menu dismissal may also hide its enclosing Qt::Popup.
            // Restore the list after the menu releases its grab, then open the child.
            QTimer::singleShot(0, pluginDialog, [pluginDialog, info, loaded] {
                if (!pluginDialog->isVisible()) pluginDialog->show();
                auto *dialog = new AnimatedDialog(pluginDialog, Qt::Popup | Qt::FramelessWindowHint);
                dialog->setObjectName("pluginAboutPopup");
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowTitle(QStringLiteral("关于插件"));
                dialog->setFixedWidth(420);
                auto *layout = new QVBoxLayout(dialog);
                layout->setContentsMargins(20, 20, 20, 20);
                layout->setSpacing(16);
                auto *title = new QLabel(QStringLiteral("关于插件"));
                title->setObjectName("sectionTitle");
                layout->addWidget(title);
                auto *fields = new QFormLayout;
                fields->setSpacing(12);
                const auto addField = [fields](const QString &caption, const QString &text, const char *name) {
                    auto *value = new QLabel(text);
                    value->setObjectName(name);
                    value->setTextFormat(Qt::PlainText);
                    value->setWordWrap(true);
                    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
                    fields->addRow(caption, value);
                };
                addField(QStringLiteral("名称"), info.name, "pluginAboutName");
                addField(QStringLiteral("插件 ID"), info.id, "pluginAboutId");
                addField(QStringLiteral("版本"), info.version, "pluginAboutVersion");
                addField(QStringLiteral("API 版本"), QStringLiteral("%1.%2").arg(info.apiMajor).arg(info.apiMinor), "pluginAboutApiVersion");
                addField(QStringLiteral("加载状态"), loaded ? QStringLiteral("已加载") : QStringLiteral("未加载"), "pluginAboutLoadState");
                layout->addLayout(fields);
                auto *close = new QPushButton(QStringLiteral("关闭"));
                close->setObjectName("closePluginAboutButton");
                layout->addWidget(close, 0, Qt::AlignRight);
                connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
                connect(pluginDialog, &QDialog::finished, dialog, &QDialog::reject);
                dialog->adjustSize();
                dialog->move(pluginDialog->mapToGlobal(pluginDialog->rect().center()) - dialog->rect().center());
                dialog->show();
            });
        });
        menu->popup(pluginList->viewport()->mapToGlobal(position));
    });
    connect(plugins, &QPushButton::clicked, this, [this, settings, pluginDialog, refreshPlugins] {
        settings->close();
        refreshPlugins();
        pluginDialog->move(mapToGlobal(rect().center()) - pluginDialog->rect().center());
        pluginDialog->show();
    });
    connect(settingsButton, &QPushButton::clicked, this, [settings, settingsButton, theme] {
        if (settings->isVisible()) { settings->close(); return; }
        theme->sync();
        settings->adjustSize();
        settings->move(settingsButton->mapToGlobal(QPoint(settingsButton->width(), -settings->height())));
        settings->show();
    });
    connect(about, &QPushButton::clicked, this, [this, settings] {
        settings->close();
        auto *dialog = new AnimatedDialog(this, Qt::Tool);
        dialog->setObjectName("aboutDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("关于"));
        auto *content = new QVBoxLayout(dialog);
        content->setContentsMargins(24, 24, 24, 24);
        auto *title = new QLabel(QStringLiteral("ZTGUI"));
        title->setObjectName("sectionTitle");
        content->addWidget(title);
        auto *applicationVersion = new QLabel(QStringLiteral("v%1").arg(QStringLiteral(ZEROTIER_GUI_VERSION)));
        applicationVersion->setObjectName("applicationVersion");
        content->addWidget(applicationVersion);
        auto *apiVersion = new QLabel(QStringLiteral("API %1").arg(QString::fromLatin1(vusiif::ztgui::api::ApiVersion)));
        apiVersion->setObjectName("pluginApiVersion");
        content->addWidget(apiVersion);
        content->addWidget(new QLabel(QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()))));
        auto *close = new QPushButton(QStringLiteral("关闭"));
        content->addWidget(close, 0, Qt::AlignRight);
        connect(close, &QPushButton::clicked, dialog, &QDialog::close);
        dialog->show();
    });
    m_pages = new QStackedWidget;
    m_pages->setObjectName("pages");
    m_pages->setMinimumWidth(460);
    auto *scroll = new QScrollArea;
    scroll->setObjectName("zeroTierScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    content->setObjectName("zeroTierContent");
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(24, 20, 24, 24);
    contentLayout->setSpacing(24);
    auto *service = new ServiceTab(this);
    contentLayout->addWidget(service);
    auto *networks = new NetworkTab(this);
    networks->setContentSized(true);
    contentLayout->addWidget(networks);
    auto *peersContainer = new QWidget;
    auto *peersLayout = new QVBoxLayout(peersContainer);
    peersLayout->setContentsMargins(0, 0, 0, 0);
    peersLayout->setSpacing(8);
    auto *peersTitle = new QLabel(QStringLiteral("成员"));
    peersTitle->setObjectName("sectionTitle");
    peersLayout->addWidget(peersTitle);
    auto *peers = new PeersTab(this);
    peers->setContentSized(true);
    peersLayout->addWidget(peers);
    contentLayout->addWidget(peersContainer);
    contentLayout->addStretch();
    scroll->setWidget(content);
    m_pages->addWidget(scroll);
    m_zeroTierPage = scroll;
    auto nodeInfo = std::make_shared<QJsonObject>();
    // Keep node status current throughout the scrollable page.
    auto *poller = new vusiif::ztgui::zerotier::JsonPoller(this, "info", true);
    connect(poller, &vusiif::ztgui::zerotier::JsonPoller::updated, this, [online, service, nodeInfo](const QJsonDocument &document) {
        *nodeInfo = document.object();
        const bool connected = (*nodeInfo)["online"].toBool();
        online->setStyleSheet(connected ? "background: #25a773; border-radius: 5px;" : "background: #d95059; border-radius: 5px;");
        online->setAccessibleName(connected ? QStringLiteral("在线") : QStringLiteral("离线"));
        online->setProperty("online", connected);
        service->setNodeInfo(*nodeInfo);
    });
    connect(poller, &vusiif::ztgui::zerotier::JsonPoller::availabilityChanged, this, [online](bool available, const QString &) {
        if (!available) {
            online->setStyleSheet("background: #d95059; border-radius: 5px;");
            online->setAccessibleName(QStringLiteral("离线"));
            online->setProperty("online", false);
        }
    });
    const auto changeTheme = [this, theme, service, nodeInfo](const QString &value) {
        const auto before = vusiif::ztgui::ui::capture(centralWidget());
        QSettings().setValue("appearance/theme", value);
        vusiif::ztgui::ui::applyAppStyle(*qApp);
        theme->sync();
        service->setNodeInfo(*nodeInfo);
        vusiif::ztgui::ui::crossFade(centralWidget(), before, 220);
    };
    connect(theme, &QAbstractButton::clicked, this, [theme, system, changeTheme] {
        const QSignalBlocker blocker(system);
        system->setChecked(false);
        changeTheme(theme->isChecked() ? "dark" : "light");
    });
    connect(system, &QCheckBox::toggled, this, [theme, changeTheme](bool follow) {
        changeTheme(follow ? "system" : theme->isChecked() ? "dark" : "light");
    });
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [theme, service, nodeInfo] {
        theme->sync();
        service->setNodeInfo(*nodeInfo);
    });
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int index) {
        if (index < 0) return;
        activateNavigation(m_navigation->item(index)->data(Qt::UserRole).toString());
    });
    connect(m_plugins, &vusiif::ztgui::plugins::PluginRegistry::menusChanged, this, &MainWindow::rebuildNavigation);
    connect(m_plugins, &vusiif::ztgui::plugins::PluginRegistry::pageCreationFailed, this,
            [this](const QString &id, const QString &error) {
        appendOutput(QStringLiteral("插件页面 %1：%2\n").arg(id, error));
    });
    rebuildNavigation();
    m_horizontal->addWidget(sidebar);
    m_horizontal->addWidget(m_pages);
    m_horizontal->setStretchFactor(0, 0);
    m_horizontal->setStretchFactor(1, 1);
    m_horizontal->setSizes({190, 900});
    const auto splitterState = QSettings().value("window/sidebar").toByteArray();
    if (!splitterState.isEmpty()) m_horizontal->restoreState(splitterState);
    layout->addWidget(m_horizontal, 1);
    setCentralWidget(central);
    const auto geometry = QSettings().value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
}

void MainWindow::rebuildNavigation()
{
    int selected = 0;
    {
        const QSignalBlocker blocker(m_navigation);
        m_navigation->clear();
        auto *builtin = new QListWidgetItem(QStringLiteral("ZeroTier"), m_navigation);
        builtin->setData(Qt::UserRole, QString::fromLatin1(vusiif::ztgui::api::ZeroTierMenuId));
        for (const auto &menu : m_plugins->menus()) {
            auto *item = new QListWidgetItem(menu.icon, menu.title, m_navigation);
            item->setData(Qt::UserRole, menu.id);
            item->setToolTip(menu.toolTip);
            if (m_plugins->preparationState(menu.pluginId) != vusiif::ztgui::plugins::PreparationState::Ready) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                item->setToolTip(m_plugins->preparationMessage(menu.pluginId));
            }
            if (menu.id == m_activeMenuId) selected = m_navigation->count() - 1;
        }
        m_navigation->setCurrentRow(selected);
    }
    activateNavigation(m_navigation->item(selected)->data(Qt::UserRole).toString());
}

void MainWindow::activateNavigation(const QString &menuId)
{
    const auto restoreSelection = [this] {
        const QSignalBlocker blocker(m_navigation);
        for (int row = 0; row < m_navigation->count(); ++row) {
            if (m_navigation->item(row)->data(Qt::UserRole).toString() == m_activeMenuId) {
                m_navigation->setCurrentRow(row);
                break;
            }
        }
    };
    if (m_navigating) { restoreSelection(); return; }
    const QScopedValueRollback<bool> guard(m_navigating, true);
    QWidget *page = m_zeroTierPage;
    if (menuId != QLatin1String(vusiif::ztgui::api::ZeroTierMenuId)) {
        const auto result = m_plugins->page(menuId, m_pages);
        if (!result.result.ok()) {
            restoreSelection();
            return;
        }
        page = result.page;
        if (m_pages->indexOf(page) < 0) m_pages->addWidget(page);
    }
    const bool changed = m_pages->currentWidget() != page;
    const auto before = changed ? vusiif::ztgui::ui::capture(m_pages) : QPixmap{};
    const auto activated = m_plugins->activateMenu(menuId, [this, &menuId, page, &restoreSelection] {
        m_activeMenuId = menuId;
        m_pages->setCurrentWidget(page);
        restoreSelection();
    });
    if (!activated.ok()) { restoreSelection(); return; }
    if (changed) vusiif::ztgui::ui::crossFade(m_pages, before);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_pluginLoader && !m_shutdownReady) {
        event->ignore();
        if (m_closing) return;
        m_closing = true;
        m_navigating = true;
        centralWidget()->setEnabled(false);
        setAcceptDrops(false);
        statusBar()->setObjectName("shutdownStatus");
        updateShutdownMessage();
        connect(m_pluginLoader, &vusiif::ztgui::plugins::PluginLoader::shutdownProgress, this,
                [this](const QStringList &pending) { m_shutdownPending = pending; updateShutdownMessage(); });
        connect(m_pluginLoader, &vusiif::ztgui::plugins::PluginLoader::shutdownFinished, this, [this] {
            m_shutdownReady = true;
            // Close after the loader's completion signal has fully returned.
            QTimer::singleShot(0, this, [this] { close(); });
        });
        QTimer::singleShot(5000, this, [this] {
            if (!m_shutdownReady) { m_shutdownSlow = true; updateShutdownMessage(); }
        });
        m_pluginLoader->requestShutdown();
        return;
    }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/sidebar", m_horizontal->saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::updateShutdownMessage()
{
    QString message = m_shutdownSlow ? QStringLiteral("等待时间较长，正在等待插件初始化结束")
                                    : QStringLiteral("正在退出，正在停止插件初始化…");
    if (!m_shutdownPending.isEmpty()) message += QStringLiteral("：") + m_shutdownPending.join(QStringLiteral("、"));
    statusBar()->showMessage(message);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    auto *moon = findChild<MoonTab *>();
    if (moon && moon->isVisible() && moon->acceptsFiles(event->mimeData())) event->acceptProposedAction();
    else event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    auto *moon = findChild<MoonTab *>();
    if (moon && moon->isVisible() && moon->acceptsFiles(event->mimeData())) {
        event->acceptProposedAction();
        moon->importDrop(event->mimeData());
    } else event->ignore();
}

void MainWindow::runService(const QString &action, std::function<void(bool)> finished)
{
    const QString operation = vusiif::ztgui::zerotier::serviceCommands(action);
    if (operation.isEmpty()) { if (finished) finished(false); return; }
    const QString script = vusiif::ztgui::zerotier::wrapScript(operation);
    runElevatedScript(QStringLiteral("ZeroTier 服务：%1").arg(action), script, finished);
}

void MainWindow::runElevatedScript(const QString &label, const QString &script,
                                   std::function<void(bool)> finished)
{
#ifdef Q_OS_WIN
    const auto invocation = vusiif::ztgui::zerotier::preparePowerShell(script);
    if (!invocation.directory->isValid()) {
        appendOutput(QStringLiteral("无法创建操作日志\n"));
        if (finished) finished(false);
        return;
    }
    const QString parameters = invocation.arguments.join(' ');
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        appendOutput(label + QStringLiteral("：未执行，提权被取消或启动失败\n"));
        if (finished) finished(false);
        return;
    }
    appendOutput(label + QStringLiteral("：执行中…\n"));
    auto handle = std::shared_ptr<void>(info.hProcess, [](void *value) { CloseHandle(value); });
    auto *timer = new QTimer(this);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this, timer, handle, label, invocation, finished] {
        if (WaitForSingleObject(handle.get(), 0) != WAIT_OBJECT_0) return;
        timer->stop();
        DWORD code = 1;
        GetExitCodeProcess(handle.get(), &code);
        QFile output(invocation.logPath);
        if (output.open(QIODevice::ReadOnly)) {
            // Detect UTF-8 and UTF-16 logs from supported PowerShell versions.
            QTextStream stream(&output);
            const QString text = stream.readAll().trimmed();
            if (!text.isEmpty()) appendOutput(text + '\n');
        }
        appendOutput(label + (code == 0 ? QStringLiteral("：完成\n") : QStringLiteral("：失败（退出码 %1）\n").arg(code)));
        if (finished) finished(code == 0);
        timer->deleteLater();
    });
    timer->start();
#elif defined(Q_OS_MACOS)
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/ztgui-operation-XXXXXX");
    if (!directory->isValid()) { if (finished) finished(false); return; }
    const QString path = directory->filePath("operation.sh");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { if (finished) finished(false); return; }
    // Preserve the operation result while refreshing the private session after partial failures.
    const QString payload = "/bin/sh -c " + vusiif::ztgui::platform::quote(script) + "\nstatus=$?\n"
        "/bin/sh -c " + vusiif::ztgui::platform::quote("set -eu\n" + vusiif::ztgui::zerotier::sessionRefreshScript())
        + "\nsession_status=$?\n[ \"$status\" -ne 0 ] || status=$session_status\n"
        "exit \"$status\"\n";
    const QByteArray bytes = payload.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.flush()) { if (finished) finished(false); return; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.close();
    auto *process = new QProcess(this);
    const QString command = "/bin/sh " + vusiif::ztgui::platform::quote(path) + " 2>&1";
    const QString appleScript = "do shell script " + vusiif::ztgui::platform::appleScriptString(command) + " with administrator privileges";
    auto completed = std::make_shared<bool>(false);
    const auto complete = [this, process, directory, finished, completed, label](bool ok, const QString &output) {
        if (*completed) return;
        *completed = true;
        if (!output.trimmed().isEmpty()) appendOutput(output.trimmed());
        appendOutput(label + (ok ? QStringLiteral("：完成") : QStringLiteral("：失败或授权被取消")));
        if (finished) finished(ok);
        process->deleteLater();
    };
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [process, complete](int code, QProcess::ExitStatus status) {
            complete(code == 0 && status == QProcess::NormalExit,
                QString::fromUtf8(process->readAllStandardOutput() + process->readAllStandardError()));
        });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, QStringLiteral("无法启动 macOS 授权程序。"));
    });
    appendOutput(label + QStringLiteral("：等待系统授权"));
    process->start("/usr/bin/osascript", {"-e", appleScript});
#else
    Q_UNUSED(script)
    appendOutput(label + QStringLiteral("：此操作仅支持 Windows\n"));
    if (finished) finished(false);
#endif
}

void MainWindow::appendOutput(const QString &text)
{
    vusiif::ztgui::util::log::write(text);
}

} // namespace vusiif::ztgui::ui
