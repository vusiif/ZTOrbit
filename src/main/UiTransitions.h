#pragma once
#include <QDialog>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QMenu>

class QLabel;
class QCloseEvent;
class QShowEvent;

namespace vusiif::ztgui::ui {
bool enabled();
QPixmap capture(QWidget *widget);
void crossFade(QWidget *widget, const QPixmap &before, int duration = 200);
void setText(QLabel *label, const QString &text);
void setBusy(QWidget *page, bool busy);

class AnimatedMenu : public QMenu {
public:
    using QMenu::QMenu;
protected:
    void showEvent(QShowEvent *event) override;
};

// Close synchronously to release popup grabs/modality; fade an inert snapshot afterwards.
class AnimatedDialog : public QDialog {
public:
    explicit AnimatedDialog(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::Dialog);
    void done(int result) override;
protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    QPropertyAnimation m_fade;
    bool m_closing = false;
};

} // namespace vusiif::ztgui::ui
