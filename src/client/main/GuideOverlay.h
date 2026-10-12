#pragma once
#include <QWidget>
#include <QPointer>
#include <QList>
#include <QRect>
class QScrollArea;
class QLabel;
class QPushButton;
namespace vusiif::ztgui::ui {
class GuideOverlay final : public QWidget {
    Q_OBJECT
public:
    struct Step { QPointer<QWidget> target; QString text; };
    GuideOverlay(QWidget *parent, QScrollArea *scroll, QList<Step> steps);
signals:
    void finished();
protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
    bool focusNextPrevChild(bool next) override;
private:
    QPointer<QScrollArea> m_scroll;
    QList<Step> m_steps;
    int m_step = 0;
    int m_scrollPosition;
    QWidget *m_card;
    QLabel *m_count, *m_text;
    QPushButton *m_previous, *m_next;
    QRect m_highlight;
    void advance(int change);
    void reposition();
    void finish();
};
}
