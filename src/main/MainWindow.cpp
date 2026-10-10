#include "MainWindow.h"
#include "AppLog.h"
#include "AppStyle.h"
#include "UiTransitions.h"
#include "DataTable.h"
#include "ZeroTierClient.h"
#include "MacPlatform.h"
#include "plugins/PluginRegistry.h"
#include "tabs/ServiceTab.h"
#include "tabs/PeersTab.h"
#include "tabs/NetworkTab.h"
#include "tabs/MoonTab.h"

#include <QCloseEvent>
#include <QApplication>
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
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QSignalBlocker>
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

namespace {
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_plugins = new ZtGui::Plugins::PluginRegistry(this);
    setupUi();
    ZeroTier::startServiceMonitor(this);
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    if (!ZeroTier::executable().isEmpty()) QTimer::singleShot(0, this, [this] {
        auto *page = m_pages->currentWidget();
        UiTransitions::setBusy(page, true);
        runElevatedScript(QStringLiteral("授权读取 ZeroTier"), ZeroTier::wrapScript({}), [page](bool) {
            UiTransitions::setBusy(page, false);
        });
    });
#endif
}

MainWindow::~MainWindow()
{
    // Release plugin widgets and callbacks before the main window's UI teardown.
    disconnect(m_plugins, nullptr, this, nullptr);
    delete m_plugins;
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
    plugins->setEnabled(false);
    settingsLayout->addWidget(plugins);
    auto *about = new QPushButton(QStringLiteral("关于"));
    about->setObjectName("aboutButton");
    settingsLayout->addWidget(about);
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
        auto *apiVersion = new QLabel(QStringLiteral("API %1").arg(QString::fromLatin1(ZtGui::Plugins::ApiVersion)));
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
    auto *poller = new JsonPoller(this, "info", true);
    connect(poller, &JsonPoller::updated, this, [online, service, nodeInfo](const QJsonDocument &document) {
        *nodeInfo = document.object();
        const bool connected = (*nodeInfo)["online"].toBool();
        online->setStyleSheet(connected ? "background: #25a773; border-radius: 5px;" : "background: #d95059; border-radius: 5px;");
        online->setAccessibleName(connected ? QStringLiteral("在线") : QStringLiteral("离线"));
        online->setProperty("online", connected);
        service->setNodeInfo(*nodeInfo);
    });
    connect(poller, &JsonPoller::availabilityChanged, this, [online](bool available, const QString &) {
        if (!available) {
            online->setStyleSheet("background: #d95059; border-radius: 5px;");
            online->setAccessibleName(QStringLiteral("离线"));
            online->setProperty("online", false);
        }
    });
    const auto changeTheme = [this, theme, service, nodeInfo](const QString &value) {
        const auto before = UiTransitions::capture(centralWidget());
        QSettings().setValue("appearance/theme", value);
        applyAppStyle(*qApp);
        theme->sync();
        service->setNodeInfo(*nodeInfo);
        UiTransitions::crossFade(centralWidget(), before, 220);
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
    connect(m_plugins, &ZtGui::Plugins::PluginRegistry::menusChanged, this, &MainWindow::rebuildNavigation);
    connect(m_plugins, &ZtGui::Plugins::PluginRegistry::pageCreationFailed, this,
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
        builtin->setData(Qt::UserRole, QString::fromLatin1(ZtGui::Plugins::ZeroTierMenuId));
        for (const auto &menu : m_plugins->menus()) {
            auto *item = new QListWidgetItem(menu.icon, menu.title, m_navigation);
            item->setData(Qt::UserRole, menu.id);
            item->setToolTip(menu.toolTip);
            if (menu.id == m_activeMenuId) selected = m_navigation->count() - 1;
        }
        m_navigation->setCurrentRow(selected);
    }
    activateNavigation(m_navigation->item(selected)->data(Qt::UserRole).toString());
}

void MainWindow::activateNavigation(const QString &menuId)
{
    QWidget *page = m_zeroTierPage;
    if (menuId != QLatin1String(ZtGui::Plugins::ZeroTierMenuId)) {
        const auto result = m_plugins->page(menuId, m_pages);
        if (!result.result.ok()) {
            const QSignalBlocker blocker(m_navigation);
            for (int row = 0; row < m_navigation->count(); ++row) {
                if (m_navigation->item(row)->data(Qt::UserRole).toString() == m_activeMenuId) {
                    m_navigation->setCurrentRow(row);
                    break;
                }
            }
            return;
        }
        page = result.page;
        if (m_pages->indexOf(page) < 0) m_pages->addWidget(page);
    }
    m_activeMenuId = menuId;
    if (m_pages->currentWidget() == page) return;
    const auto before = UiTransitions::capture(m_pages);
    m_pages->setCurrentWidget(page);
    UiTransitions::crossFade(m_pages, before);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/sidebar", m_horizontal->saveState());
    QMainWindow::closeEvent(event);
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
    const QString operation = ZeroTier::serviceCommands(action);
    if (operation.isEmpty()) { if (finished) finished(false); return; }
    const QString script = ZeroTier::wrapScript(operation);
    runElevatedScript(QStringLiteral("ZeroTier 服务：%1").arg(action), script, finished);
}

void MainWindow::runElevatedScript(const QString &label, const QString &script,
                                   std::function<void(bool)> finished)
{
#ifdef Q_OS_WIN
    const auto invocation = ZeroTier::preparePowerShell(script);
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
    const QString payload = "/bin/sh -c " + MacPlatform::quote(script) + "\nstatus=$?\n"
        "/bin/sh -c " + MacPlatform::quote("set -eu\n" + ZeroTier::sessionRefreshScript())
        + "\nsession_status=$?\n[ \"$status\" -ne 0 ] || status=$session_status\n"
        "exit \"$status\"\n";
    const QByteArray bytes = payload.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.flush()) { if (finished) finished(false); return; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.close();
    auto *process = new QProcess(this);
    const QString command = "/bin/sh " + MacPlatform::quote(path) + " 2>&1";
    const QString appleScript = "do shell script " + MacPlatform::appleScriptString(command) + " with administrator privileges";
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
    AppLog::write(text);
}
