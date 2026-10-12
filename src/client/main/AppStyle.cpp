#include "AppStyle.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyle>
#include <QWidget>
#include <QProxyStyle>

namespace {
class CleanFocusStyle : public QProxyStyle {
public:
    CleanFocusStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == PE_FrameFocusRect || element == PE_FrameDefaultButton) return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};
}

void vusiif::ztgui::ui::applyAppStyle(QApplication &app)
{
    static bool installed = false;
    if (!installed) { app.setStyle(new CleanFocusStyle); installed = true; }
    static bool connected = false;
    if (!connected) {
        QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
            if (QSettings().value("appearance/theme", "system").toString() == "system") vusiif::ztgui::ui::applyAppStyle(app);
        });
        connected = true;
    }
    const QString theme = QSettings().value("appearance/theme", "system").toString();
    const bool dark = theme == "dark" || (theme == "system" && app.styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    app.setProperty("darkTheme", dark);
    const QString window = dark ? "#202830" : "#f4f7fa";
    const QString base = dark ? "#27323c" : "#ffffff";
    const QString alternate = dark ? "#2c3944" : "#f1f6fa";
    const QString text = dark ? "#e1ebf3" : "#20394c";
    const QString side = dark ? "#253746" : "#eaf1f7";
    const QString border = dark ? "#455b6c" : "#b9cbd8";
    const QString highlight = dark ? "#365772" : "#d8eaf6";
    const QString accent = dark ? "#9cceee" : "#21486b";
    const QString muted = dark ? "#a9bdcb" : "#607687";
    const QString hover = dark ? "#354e61" : "#e3eff8";
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(window));
    palette.setColor(QPalette::WindowText, QColor(text));
    palette.setColor(QPalette::Base, QColor(base));
    palette.setColor(QPalette::AlternateBase, QColor(alternate));
    palette.setColor(QPalette::Text, QColor(text));
    palette.setColor(QPalette::Button, QColor(side));
    palette.setColor(QPalette::ButtonText, QColor(text));
    palette.setColor(QPalette::Highlight, QColor(highlight));
    palette.setColor(QPalette::HighlightedText, QColor(text));
    palette.setColor(QPalette::ToolTipBase, QColor(base));
    palette.setColor(QPalette::ToolTipText, QColor(text));
    palette.setColor(QPalette::Light, QColor(border));
    palette.setColor(QPalette::Mid, QColor(border));
    palette.setColor(QPalette::Dark, QColor(window));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(muted));
    app.setPalette(palette);
#ifdef Q_OS_MACOS
    const QString fontFamily = "\"PingFang SC\", \"Helvetica Neue\"";
#else
    const QString fontFamily = "\"Microsoft YaHei UI\", \"Segoe UI\"";
#endif
    app.setStyleSheet(QStringLiteral(R"CSS(
        QWidget { font-family: %12; font-size: 13px; }
        QWidget#sidebar { background: %1; border-radius: 6px; }
        QLabel#brand, QLabel#serviceTitle { font-size: 24px; font-weight: 600; color: %2; }
        QLabel#nodeId { font-size: 28px; font-weight: 600; }
        QLabel#nodeVersion { font-size: 14px; color: %3; }
        QLabel#serviceStatusWarning { color: %10; }
        QPushButton#settingsButton { font-size: 24px; border: none; background: transparent; }
        QPushButton#settingsButton:hover { background: %9; }
        QDialog#settingsPopup, QDialog#pluginsPopup, QDialog#pluginAboutPopup { background: %6; border: 1px solid %5; border-radius: 8px; }
        QPushButton#serviceAction { border-top-right-radius: 0; border-bottom-right-radius: 0; }
        QToolButton#serviceDropdown { border-top-left-radius: 0; border-bottom-left-radius: 0; padding: 0; }
        QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: %2; }
        QLabel#sidebarHint, QLabel#sidebarVersion, QLabel#tableStatus, QLabel#moonEmptyHint { color: %3; font-size: 12px; }
        QListWidget#navigation { background: transparent; border: none; }
        QListWidget#navigation::item { padding: 12px 8px; border-radius: 4px; }
        QListWidget#navigation::item:selected { background: %4; }
        QTreeWidget { border: 1px solid %5; background: %6; alternate-background-color: %7; }
        QTreeWidget::item:selected { background: %4; }
        QTreeWidget#pluginList::item { min-height: 30px; }
        QFrame#networkMemberRow { background: %7; border: none; border-left: 2px solid %5; }
        QFrame#networkMemberRow:hover { background: %9; }
        QLabel#networkMemberName { font-weight: 500; }
        QLabel#networkMemberIp { color: %3; }
        QScrollArea#zeroTierScroll { border: none; background: transparent; }
        QWidget#zeroTierContent { background: %11; }
        QHeaderView::section { background: %1; color: %8; padding: 9px 8px; border: none; border-right: 1px solid %5; }
        QPushButton, QToolButton, QComboBox { color: %8; background: %1; border: 1px solid %5; border-radius: 4px; padding: 6px 12px; }
        QPushButton:hover, QToolButton:hover, QComboBox:hover { background: %9; border-color: %2; }
        QPushButton:pressed, QToolButton:pressed { background: %4; }
        QPushButton:focus, QToolButton:focus, QComboBox:focus { border-color: %2; }
        QPushButton:disabled { color: %3; background: %7; border-color: %5; }
        QMessageBox QPushButton { min-width: 70px; }
        QLineEdit, QPlainTextEdit, QTextEdit { background: %6; color: %8; border: 1px solid %5; border-radius: 4px; padding: 7px; }
        QSplitter::handle { background: %5; }
        QSplitter::handle:hover { background: %2; }
        QMenu { background: %6; color: %8; border: 1px solid %5; padding: 5px; }
        QMenu::item { padding: 8px 24px; border-radius: 3px; }
        QMenu::item:selected { background: %4; }
        QMenu::item:disabled { color: %3; }
        QPushButton#confirmDelete { color: %10; border-color: %10; }
        QFrame#confirmCard { background: %6; border: 1px solid %5; border-radius: 8px; }
        QComboBox QAbstractItemView { background: %6; color: %8; selection-background-color: %4; }
        QToolTip { background: %6; color: %8; border: 1px solid %5; }
    )CSS").arg(side, accent, muted, highlight, border, base, alternate, text, hover,
                  dark ? "#ff9898" : "#b92c38").arg(window).arg(fontFamily));
    for (auto *widget : QApplication::allWidgets()) widget->update();
}
