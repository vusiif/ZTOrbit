#include "UiTransitions.h"
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QVariantAnimation>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
class OperationOverlay : public QWidget {
public:
    explicit OperationOverlay(QWidget *page) : QWidget(page)
    {
        setObjectName("operationOverlay");
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(page->rect());
        page->installEventFilter(this);
        m_spinner.setInterval(40);
        connect(&m_spinner, &QTimer::timeout, this, [this] {
            m_angle = (m_angle + 18) % 360;
            update();
        });
        m_fade.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_opacity = value.toReal();
            update();
        });
        connect(&m_fade, &QVariantAnimation::finished, this, [this] {
            if (!m_busy) hide();
        });
        hide();
    }
    void setBusy(bool busy)
    {
        if (m_busy == busy) return;
        m_busy = busy;
        setProperty("busy", busy);
        m_fade.stop();
        setAttribute(Qt::WA_TransparentForMouseEvents, !busy);
        if (busy) {
            m_restoreEnabled = parentWidget()->isEnabled();
            auto *focus = QApplication::focusWidget();
            m_focus = focus && parentWidget()->isAncestorOf(focus) ? focus : nullptr;
            // Disable keyboard, mouse and drop actions throughout this page only.
            parentWidget()->setEnabled(false);
            setGeometry(parentWidget()->rect());
            show();
            raise();
            if (UiTransitions::enabled() && isVisible()) m_spinner.start();
        } else {
            parentWidget()->setEnabled(m_restoreEnabled);
            if (m_focus && m_focus->isVisible() && m_focus->isEnabled()) m_focus->setFocus(Qt::OtherFocusReason);
            m_focus.clear();
        }
        if (!UiTransitions::enabled() || !isVisible()) {
            m_opacity = busy ? 1.0 : 0.0;
            if (!busy) hide();
            update();
            return;
        }
        m_fade.setDuration(busy ? 180 : 160);
        m_fade.setStartValue(m_opacity);
        m_fade.setEndValue(busy ? 1.0 : 0.0);
        m_fade.start();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        const bool dark = qApp->property("darkTheme").toBool();
        QPainter painter(this);
        painter.setOpacity(m_opacity);
        painter.fillRect(rect(), dark ? QColor(22, 26, 30, 165) : QColor(100, 106, 112, 110));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(rect().center());
        painter.rotate(m_angle);
        // Twelve rounded lines with a fading tail form the rotating indicator.
        const QColor color = dark ? QColor("#e1ebf3") : QColor("#ffffff");
        for (int i = 0; i < 12; ++i) {
            QColor stroke(color);
            stroke.setAlpha(255 - i * 18);
            painter.setPen(QPen(stroke, 3.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(0, -13), QPointF(0, -24));
            painter.rotate(-30);
        }
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
        return QWidget::eventFilter(object, event);
    }
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (m_busy && UiTransitions::enabled()) m_spinner.start();
    }
    void hideEvent(QHideEvent *event) override
    {
        m_spinner.stop();
        QWidget::hideEvent(event);
    }
private:
    QVariantAnimation m_fade;
    QTimer m_spinner;
    QPointer<QWidget> m_focus;
    qreal m_opacity = 0.0;
    int m_angle = 0;
    bool m_busy = false;
    bool m_restoreEnabled = true;
};

class SnapshotFade : public QWidget {
public:
    SnapshotFade(QWidget *parent, const QPixmap &snapshot, int duration)
        : QWidget(parent), m_snapshot(snapshot)
    {
        setObjectName("uiTransitionOverlay");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(parent->rect());
        parent->installEventFilter(this);
        m_fade.setDuration(duration);
        m_fade.setStartValue(1.0);
        m_fade.setEndValue(0.0);
        m_fade.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_opacity = value.toReal();
            update();
        });
        connect(&m_fade, &QVariantAnimation::finished, this, [this] { dismiss(); });
        show();
        raise();
        m_fade.start();
    }
    void dismiss()
    {
        m_fade.stop();
        setObjectName({});
        hide();
        deleteLater();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setOpacity(m_opacity);
        painter.drawPixmap(QPoint(0, 0), m_snapshot);
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Hide)) dismiss();
        return QWidget::eventFilter(object, event);
    }
private:
    QPixmap m_snapshot;
    QVariantAnimation m_fade;
    qreal m_opacity = 1.0;
};
}

bool UiTransitions::enabled()
{
    return true;
}

QPixmap UiTransitions::capture(QWidget *widget)
{
    // A snapshot containing a loading indicator would freeze it during a cross-fade.
    for (auto *overlay : widget->findChildren<QWidget *>("operationOverlay"))
        if (overlay->isVisible()) return {};
    return enabled() && widget->isVisible() && !widget->size().isEmpty() ? widget->grab() : QPixmap();
}

void UiTransitions::crossFade(QWidget *widget, const QPixmap &before, int duration)
{
    // A new snapshot already contains any partially blended previous transition.
    for (auto *child : widget->findChildren<QWidget *>("uiTransitionOverlay", Qt::FindDirectChildrenOnly))
        static_cast<SnapshotFade *>(child)->dismiss();
    if (!before.isNull() && enabled() && widget->isVisible()) new SnapshotFade(widget, before, duration);
}

void UiTransitions::setText(QLabel *label, const QString &text)
{
    if (label->text() == text) return;
    const QPixmap before = capture(label);
    label->setText(text);
    crossFade(label, before, 160);
}

void UiTransitions::setBusy(QWidget *page, bool busy)
{
    auto *overlay = static_cast<OperationOverlay *>(page->findChild<QWidget *>("operationOverlay", Qt::FindDirectChildrenOnly));
    if (!overlay && busy) overlay = new OperationOverlay(page);
    if (overlay) overlay->setBusy(busy);
}

AnimatedDialog::AnimatedDialog(QWidget *parent, Qt::WindowFlags flags)
    : QDialog(parent, flags)
{
    // Effects render into a transparent pixmap; paint ordinary dialog backgrounds explicitly.
    setAutoFillBackground(true);
    m_opacity = new QGraphicsOpacityEffect(this);
    m_opacity->setOpacity(1.0);
    setGraphicsEffect(m_opacity);
    m_fade.setTargetObject(m_opacity);
    m_fade.setPropertyName("opacity");
    m_fade.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_fade, &QPropertyAnimation::finished, this, [this] {
        if (m_closing) {
            setEnabled(true);
            QDialog::done(m_pendingResult);
        }
    });
}

void AnimatedDialog::showEvent(QShowEvent *event)
{
    m_fade.stop();
    m_closing = false;
    setEnabled(true);
    m_opacity->setOpacity(UiTransitions::enabled() ? 0.0 : 1.0);
    QDialog::showEvent(event);
    if (UiTransitions::enabled()) {
        m_fade.setDuration(180);
        m_fade.setStartValue(0.0);
        m_fade.setEndValue(1.0);
        m_fade.start();
    }
}

void AnimatedDialog::done(int result)
{
    if (m_closing) return;
    if (!isVisible() || !UiTransitions::enabled()) {
        m_closing = true;
        m_fade.stop();
        QDialog::done(result);
        return;
    }
    m_closing = true;
    m_pendingResult = result;
    m_fade.stop();
    m_fade.setDuration(140);
    m_fade.setStartValue(m_opacity->opacity());
    m_fade.setEndValue(0.0);
    m_fade.start();
}

void AnimatedDialog::closeEvent(QCloseEvent *event)
{
    event->ignore();
    done(QDialog::Rejected);
}

void AnimatedMenu::showEvent(QShowEvent *event)
{
    QMenu::showEvent(event);
    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(150);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}
