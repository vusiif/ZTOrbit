#include "NetworkMemberRow.h"
#include "UiTransitions.h"
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QStackedWidget>

namespace vusiif::ztgui::ui {
NetworkMemberRow::NetworkMemberRow(const QString &id, QWidget *parent)
    : QFrame(parent), m_id(id.toLower())
{
    setObjectName("networkMemberRow");
    setFixedHeight(44);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 4, 16, 4);
    layout->setSpacing(20);
    m_name = new QLabel;
    m_name->setObjectName("networkMemberName");
    m_name->setTextFormat(Qt::PlainText);
    m_name->setMinimumWidth(0);
    m_editor = new QLineEdit;
    m_editor->setObjectName("networkMemberRemarkEditor");
    m_editor->setMaxLength(128);
    m_editor->installEventFilter(this);
    m_nameStack = new QStackedWidget;
    m_nameStack->addWidget(m_name);
    m_nameStack->addWidget(m_editor);
    layout->addWidget(m_nameStack, 2);
    m_ip = new QLabel;
    m_ip->setObjectName("networkMemberIp");
    m_ip->setTextFormat(Qt::PlainText);
    m_ip->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_ip->setMinimumWidth(0);
    layout->addWidget(m_ip, 1);
    connect(m_editor, &QLineEdit::editingFinished, this, [this] { finishRemark(true); });
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        AnimatedMenu menu(this);
        menu.setObjectName("networkMemberMenu");
        menu.addAction(m_id, this, [this] { QApplication::clipboard()->setText(m_id); });
        connect(menu.addAction(QStringLiteral("修改备注")), &QAction::triggered,
                this, &NetworkMemberRow::editRemark, Qt::QueuedConnection);
        menu.exec(mapToGlobal(position));
    });
}

QString NetworkMemberRow::memberName() const
{
    // Share the node-ID mapping with the existing ZeroTier peers list.
    const QString name = QSettings().value("names/peers/" + m_id).toString().trimmed();
    return name.isEmpty() ? m_id : name;
}

void NetworkMemberRow::updateMember(const QJsonObject &member)
{
    QStringList ips;
    for (const auto &value : member.value("ipAssignments").toArray()) {
        const QString ip = value.toString().section('/', 0, 0);
        if (!ip.isEmpty() && !ips.contains(ip)) ips.append(ip);
    }
    m_name->setText(memberName());
    m_name->setToolTip(m_name->text());
    m_ip->setText(ips.isEmpty() ? QStringLiteral("—") : ips.join(", "));
    m_ip->setToolTip(m_ip->text());
    setAccessibleName(m_name->text() + ' ' + m_ip->text());
}

void NetworkMemberRow::editRemark()
{
    if (m_editing) { m_editor->setFocus(); return; }
    m_editing = true;
    m_editor->setText(memberName());
    m_nameStack->setCurrentWidget(m_editor);
    qApp->installEventFilter(this);
    m_editor->selectAll();
    m_editor->setFocus();
}

void NetworkMemberRow::finishRemark(bool save)
{
    if (!m_editing) return;
    m_editing = false;
    qApp->removeEventFilter(this);
    if (save) {
        const QString name = m_editor->text().trimmed();
        QSettings settings;
        if (name.isEmpty() || name == m_id) settings.remove("names/peers/" + m_id);
        else settings.setValue("names/peers/" + m_id, name);
        settings.sync();
    }
    m_name->setText(memberName());
    m_name->setToolTip(m_name->text());
    m_nameStack->setCurrentWidget(m_name);
    if (save) emit remarkChanged(m_id);
}

bool NetworkMemberRow::eventFilter(QObject *object, QEvent *event)
{
    if (m_editing && object == m_editor && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        finishRemark(false);
        return true;
    }
    if (m_editing && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && widget != m_editor && !m_editor->isAncestorOf(widget)) finishRemark(true);
    }
    return QFrame::eventFilter(object, event);
}
}
