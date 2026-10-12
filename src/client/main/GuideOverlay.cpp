#include "GuideOverlay.h"
#include <QScrollArea>
#include <QScrollBar>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QKeyEvent>
#include <QTimer>
#include <QApplication>
namespace vusiif::ztgui::ui {
GuideOverlay::GuideOverlay(QWidget *parent, QScrollArea *scroll, QList<Step> steps)
    : QWidget(parent), m_scroll(scroll), m_steps(std::move(steps)),
      m_scrollPosition(scroll->verticalScrollBar()->value()) {
    setObjectName("beginnerGuideOverlay");
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(parent->rect());
    m_card = new QWidget(this);
    m_card->setObjectName("guideCard");
    m_card->setStyleSheet("#guideCard { background: #f6f7f9; color: #18232d; border: 1px solid #b4bdc6; border-radius: 10px; } QLabel { color: #18232d; background: transparent; }");
    auto *layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(18, 16, 18, 16);
    m_count = new QLabel; m_count->setObjectName("guideStepCount");
    m_text = new QLabel; m_text->setWordWrap(true); m_text->setTextFormat(Qt::PlainText);
    m_text->setObjectName("guideText");
    layout->addWidget(m_count); layout->addWidget(m_text);
    auto *row = new QHBoxLayout;
    auto *skip = new QPushButton(QStringLiteral("跳过")); skip->setObjectName("guideSkip");
    m_previous = new QPushButton(QStringLiteral("上一步")); m_previous->setObjectName("guidePrevious");
    m_next = new QPushButton; m_next->setObjectName("guideNext");
    row->addWidget(skip); row->addStretch(); row->addWidget(m_previous); row->addWidget(m_next);
    layout->addLayout(row);
    connect(skip, &QPushButton::clicked, this, &GuideOverlay::finish);
    connect(m_previous, &QPushButton::clicked, this, [this] { advance(-1); });
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_step + 1 == m_steps.size()) finish(); else advance(1);
    });
    parent->installEventFilter(this);
    qApp->installEventFilter(this);
    scroll->viewport()->installEventFilter(this);
    for (const auto &step : m_steps) if (step.target) step.target->installEventFilter(this);
    connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { reposition(); });
    show(); raise(); setFocus(); advance(0);
}
void GuideOverlay::advance(int change) {
    if (m_steps.isEmpty()) { finish(); return; }
    m_step = qBound(0, m_step + change, int(m_steps.size()) - 1);
    m_count->setText(QStringLiteral("新手指南 · %1 / %2").arg(m_step + 1).arg(m_steps.size()));
    m_text->setText(m_steps[m_step].text);
    m_previous->setEnabled(m_step > 0);
    m_next->setText(m_step + 1 == m_steps.size() ? QStringLiteral("完成") : QStringLiteral("下一步"));
    QTimer::singleShot(0, this, [this] {
        if (auto *target = m_steps[m_step].target.data()) m_scroll->ensureWidgetVisible(target, 12, 30);
        reposition();
    });
}
void GuideOverlay::reposition() {
    if (m_steps.isEmpty() || !m_scroll) return;
    setGeometry(parentWidget()->rect());
    QWidget *target = m_steps[m_step].target;
    while (target && !target->isVisible()) target = target->parentWidget();
    m_highlight = target ? QRect(mapFromGlobal(target->mapToGlobal(QPoint())), target->size()).adjusted(-5,-5,5,5) : QRect();
    const QRect viewport(mapFromGlobal(m_scroll->viewport()->mapToGlobal(QPoint())), m_scroll->viewport()->size());
    m_highlight = m_highlight.intersected(viewport).intersected(rect());
    m_card->setFixedWidth(qMin(420, qMax(1, width() - 32)));
    m_card->adjustSize();
    int top = m_highlight.bottom() + 16;
    if (top + m_card->height() > height() - 16) top = m_highlight.top() - m_card->height() - 16;
    top = qBound(16, top, qMax(16, height() - m_card->height() - 16));
    int left = qBound(16, m_highlight.left(), qMax(16, width() - m_card->width() - 16));
    m_card->move(left, top); update();
}
void GuideOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath shade; shade.addRect(rect());
    if (!m_highlight.isEmpty()) shade.addRoundedRect(m_highlight, 7, 7);
    shade.setFillRule(Qt::OddEvenFill);
    painter.fillPath(shade, QColor(70, 74, 78, 185));
    painter.setPen(QPen(QColor("#69bfff"), 3)); painter.setBrush(Qt::NoBrush);
    if (!m_highlight.isEmpty()) painter.drawRoundedRect(m_highlight, 7, 7);
}
void GuideOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) finish();
    else if (event->key() == Qt::Key_Left) advance(-1);
    else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Return) m_next->click();
    else QWidget::keyPressEvent(event);
}
bool GuideOverlay::focusNextPrevChild(bool next) {
    QList<QWidget *> choices;
    for (auto *button : m_card->findChildren<QPushButton *>())
        if (button->isEnabled() && button->isVisible()) choices.append(button);
    if (choices.isEmpty()) { setFocus(); return true; }
    int index = choices.indexOf(qApp->focusWidget());
    index = index < 0 ? (next ? 0 : choices.size() - 1)
        : (index + (next ? 1 : choices.size() - 1)) % choices.size();
    choices[index]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
}
bool GuideOverlay::eventFilter(QObject *object, QEvent *event) {
    if (!isVisible()) return false;
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::Shortcut) {
        event->accept(); return true;
    }
    bool tracked = object == parentWidget() || (m_scroll && object == m_scroll->viewport());
    for (const auto &step : m_steps) tracked |= object == step.target.data();
    if (tracked && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        QTimer::singleShot(0, this, [this] { reposition(); });
    return false;
}
void GuideOverlay::finish() {
    if (m_scroll) m_scroll->verticalScrollBar()->setValue(m_scrollPosition);
    hide(); emit finished(); close();
}
}
