#include "DataTable.h"
#include "ZeroTierClient.h"
#include "UiTransitions.h"

#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPointer>
#include <QShowEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QWheelEvent>
#include <QStyledItemDelegate>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

namespace vusiif::ztgui::ui {
namespace {
constexpr int BadgeRole = Qt::UserRole + 2;
constexpr int ActionRowRole = Qt::UserRole + 3;
class TableItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const bool action = data(0, ActionRowRole).toBool();
        const bool otherAction = other.data(0, ActionRowRole).toBool();
        if (action != otherAction) {
            const bool ascending = treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder;
            return action ? !ascending : ascending;
        }
        return QTreeWidgetItem::operator<(other);
    }
};
class RemarkCommitFilter : public QObject {
public:
    explicit RemarkCommitFilter(QLineEdit *editor) : QObject(editor), m_editor(editor)
    {
        qApp->installEventFilter(this);
        connect(editor, &QLineEdit::editingFinished, this, [this] { m_active = false; });
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (m_active && m_editor && event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && widget != m_editor && !m_editor->isAncestorOf(widget)) m_editor->editingFinished();
        }
        return false;
    }
private:
    QPointer<QLineEdit> m_editor;
    bool m_active = true;
};
class CellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const auto *widget = opt.widget;
        auto *style = widget ? widget->style() : QApplication::style();
        // Draw the background independently so selection never changes text geometry.
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);
        painter->save();
        painter->setClipRect(opt.rect);
        painter->setFont(opt.font);
        const QRect content = opt.rect.adjusted(8, 0, -8, 0);
        const QString value = index.data().toString();
        if (index.data(BadgeRole).toBool() && value != QStringLiteral("—")) {
            const bool dark = qApp->property("darkTheme").toBool();
            QString color = dark ? "#99c9f5" : "#245f92";
            if (value == "直链" || value == "已连接" || value == "在线" || value == "是") color = dark ? "#88dbb5" : "#19734d";
            else if (value == "中转" || value == "等待授权" || value == "获取配置中" || value == "否" || value == "离线") color = dark ? "#f3c47f" : "#925b0d";
            else if (value == "中转站") color = dark ? "#cfb5f6" : "#7050a1";
            else if (value == "不可达" || value.contains("错误") || value.contains("不存在") || value.contains("过旧") || value.contains("身份验证")) color = dark ? "#ff9898" : "#b92c38";
            QColor foreground(color), background(color);
            background.setAlpha(dark ? 40 : 24);
            QRect pill(content.left(), content.center().y() - 12,
                       qMin(content.width(), opt.fontMetrics.horizontalAdvance(value) + 18), 24);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(pill, 12, 12);
            painter->setPen(foreground);
            painter->drawText(pill.adjusted(9, 0, -9, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              opt.fontMetrics.elidedText(value, Qt::ElideRight, qMax(0, pill.width() - 18)));
        } else {
            painter->setPen(opt.palette.color(opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(content, Qt::AlignVCenter | Qt::AlignLeft,
                              opt.fontMetrics.elidedText(value, Qt::ElideRight, content.width()));
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        // Match the horizontal margins and pill padding used by paint().
        size.setWidth(opt.fontMetrics.horizontalAdvance(opt.text) + 16
                      + (index.data(BadgeRole).toBool() && opt.text != QStringLiteral("—") ? 18 : 0));
        size.setHeight(qMax(size.height(), qMax(36, option.fontMetrics.height() + 14)));
        return size;
    }
};

class DragScrollFilter : public QObject {
public:
    explicit DragScrollFilter(QAbstractItemView *view) : QObject(view), m_view(view) {}
protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_pressed = true;
                m_dragging = false;
                m_origin = mouse->pos();
                m_horizontal = m_view->horizontalScrollBar()->value();
                m_vertical = m_view->verticalScrollBar()->value();
            }
        } else if (event->type() == QEvent::MouseMove && m_pressed) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint delta = mouse->pos() - m_origin;
            if (!m_dragging && delta.manhattanLength() >= QApplication::startDragDistance()) {
                m_dragging = true;
                m_view->viewport()->setCursor(Qt::ClosedHandCursor);
            }
            if (m_dragging) {
                m_view->horizontalScrollBar()->setValue(m_horizontal - delta.x());
                m_view->verticalScrollBar()->setValue(m_vertical - delta.y());
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const bool dragged = m_dragging;
            m_pressed = m_dragging = false;
            m_view->viewport()->unsetCursor();
            if (dragged) return true;
        } else if (event->type() == QEvent::UngrabMouse) {
            m_pressed = m_dragging = false;
            m_view->viewport()->unsetCursor();
        }
        return false;
    }
private:
    QAbstractItemView *m_view;
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_origin;
    int m_horizontal = 0;
    int m_vertical = 0;
};

void appendJson(QTreeWidgetItem *parent, const QString &key, const QJsonValue &value)
{
    auto *item = new QTreeWidgetItem(parent);
    item->setText(0, vusiif::ztgui::zerotier::translate(key));
    item->setToolTip(0, key);
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) appendJson(item, it.key(), it.value());
    } else if (value.isArray()) {
        const auto array = value.toArray();
        item->setText(1, QStringLiteral("%1 项").arg(array.size()));
        for (int i = 0; i < array.size(); ++i) appendJson(item, QString::number(i + 1), array[i]);
    } else {
        item->setText(1, vusiif::ztgui::zerotier::jsonText(value));
        item->setToolTip(1, value.toVariant().toString());
    }
}
}

void enableDragScrolling(QAbstractItemView *view)
{
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->viewport()->installEventFilter(new DragScrollFilter(view));
}

DataTable::DataTable(const QString &kind, const QStringList &headers, QWidget *parent, bool names, bool actions)
    : QWidget(parent), m_kind(kind), m_names(names)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(8);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(kind + "Table");
    m_tree->setHeaderLabels(headers);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(0);
    m_tree->setItemDelegate(new CellDelegate(m_tree));
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setSectionsMovable(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(40);
    for (int i = 0; i < headers.size(); ++i) m_tree->setColumnWidth(i, i < 2 ? 180 : 135);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(names && headers.size() > 1 ? 1 : 0, Qt::AscendingOrder);
    enableDragScrolling(m_tree);
    m_tree->viewport()->installEventFilter(this);
    m_tree->header()->viewport()->installEventFilter(this);
    m_layout->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this] { updateContentHeight(); });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this] { updateContentHeight(); });
    connect(m_tree->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateContentHeight(); });

    m_footer = new QWidget;
    auto *footer = new QHBoxLayout(m_footer);
    footer->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel;
    m_status->setObjectName("tableStatus");
    footer->addWidget(m_status, 1);
    if (names && actions) {
        m_nameButton = new QPushButton(QStringLiteral("编辑名称"));
        m_nameButton->setObjectName("renameButton");
        m_nameButton->setEnabled(false);
        footer->addWidget(m_nameButton);
        connect(m_nameButton, &QPushButton::clicked, this, &DataTable::renameSelected);
    }
    if (actions) connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this, actions](QTreeWidgetItem *, int column) {
        if (m_names && actions && column == 0) renameSelected();
        else showDetails();
    });
    if (actions) {
        m_detailsButton = new QPushButton(QStringLiteral("详细信息"));
        m_detailsButton->setObjectName("detailsButton");
        m_detailsButton->setEnabled(false);
        footer->addWidget(m_detailsButton);
        connect(m_detailsButton, &QPushButton::clicked, this, &DataTable::showDetails);
    }
    m_layout->addWidget(m_footer);
    if (!actions) m_footer->hide();
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool single = m_tree->selectedItems().size() == 1;
        if (m_detailsButton) m_detailsButton->setEnabled(single);
        if (m_nameButton) m_nameButton->setEnabled(single);
    });
    const auto state = QSettings().value("tables/" + kind + "/header-v3").toByteArray();
    if (!state.isEmpty()) m_tree->header()->restoreState(state);
    connect(m_tree->header(), &QHeaderView::sectionResized, this, [this] {
        QSettings().setValue("tables/" + m_kind + "/header-v3", m_tree->header()->saveState());
    });
    connect(m_tree->header(), &QHeaderView::sectionMoved, this, [this] {
        QSettings().setValue("tables/" + m_kind + "/header-v3", m_tree->header()->saveState());
    });
    updateStatus();
}

void DataTable::setRows(const QList<TableRow> &rows)
{
    // Reconcile by stable ID; keep the same item for every surviving row.
    const int horizontal = m_tree->horizontalScrollBar()->value();
    const int vertical = m_tree->verticalScrollBar()->value();
    const QSignalBlocker blocker(m_tree);
    m_tree->setUpdatesEnabled(false);
    const bool sorting = m_tree->isSortingEnabled();
    m_tree->setSortingEnabled(false);
    QHash<QString, QTreeWidgetItem *> existing;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto *item = m_tree->topLevelItem(i);
        if (item == m_actionRow) continue;
        existing.insert(item->data(0, Qt::UserRole).toString(), item);
    }
    QSettings settings;
    QList<QTreeWidgetItem *> ordered;
    for (const auto &row : rows) {
        if (row.id.isEmpty()) continue;
        auto *item = existing.take(row.id);
        if (!item) item = new TableItem(m_tree);
        ordered.append(item);
        item->setData(0, Qt::UserRole, row.id);
        item->setData(0, Qt::UserRole + 1, row.details);
        QStringList cells = row.cells;
        if (m_names) cells.prepend(settings.value("names/" + m_kind + "/" + row.id, defaultName(item)).toString());
        for (int i = 0; i < cells.size(); ++i) {
            if (i == 0 && row.id == m_editingId) continue;
            if (item->text(i) != cells[i]) item->setText(i, cells[i]);
            item->setToolTip(i, cells[i]);
            item->setData(i, BadgeRole, m_badgeColumns.contains(i)
                || (m_kind == "info" && i == 1 && row.details[row.id].isBool()));
        }
    }
    for (auto *item : existing) {
        if (item->data(0, Qt::UserRole).toString() == m_editingId) {
            m_editingId.clear();
            m_tree->removeItemWidget(item, 0);
        }
        delete item;
    }
    if (!sorting && m_kind == "peers" && m_editingId.isEmpty()) {
        // Sort by connection quality, with PLANET rows always last. Preserve item identity.
        for (int i = 0; i < ordered.size(); ++i) {
            auto *item = ordered[i];
            const int from = m_tree->indexOfTopLevelItem(item);
            if (from == i) continue;
            const bool selected = item->isSelected();
            const bool current = m_tree->currentItem() == item;
            m_tree->takeTopLevelItem(from);
            m_tree->insertTopLevelItem(i, item);
            item->setSelected(selected);
            if (current) m_tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        }
    }
    m_tree->setSortingEnabled(sorting);
    m_tree->horizontalScrollBar()->setValue(horizontal);
    m_tree->verticalScrollBar()->setValue(vertical);
    m_tree->setUpdatesEnabled(true);
    const bool single = m_tree->selectedItems().size() == 1;
    if (m_detailsButton) m_detailsButton->setEnabled(single);
    if (m_nameButton) m_nameButton->setEnabled(single);
    updateStatus();
    updateContentHeight();
}

void DataTable::setInlineAction(QWidget *widget)
{
    Q_ASSERT(!m_actionRow);
    m_actionRow = new TableItem(m_tree);
    m_actionRow->setData(0, ActionRowRole, true);
    m_actionRow->setFlags(Qt::ItemIsEnabled);
    m_actionRow->setFirstColumnSpanned(true);
    m_actionRow->setSizeHint(0, QSize(0, 48));
    m_tree->setUniformRowHeights(false);
    m_tree->setItemWidget(m_actionRow, 0, widget);
    updateContentHeight();
}

void DataTable::setContentSized(bool enabled)
{
    m_contentSized = enabled;
    m_tree->setVerticalScrollBarPolicy(enabled ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Expanding, enabled ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    if (!enabled) { m_tree->setMinimumHeight(0); m_tree->setMaximumHeight(QWIDGETSIZE_MAX); }
    updateContentHeight();
}

void DataTable::updateContentHeight()
{
    if (!m_contentSized) return;
    int height = m_tree->header()->height() + 2 * m_tree->frameWidth();
    QStyleOptionViewItem option;
    option.initFrom(m_tree);
    std::function<void(QTreeWidgetItem *)> add = [&](QTreeWidgetItem *item) {
        if (item->isHidden()) return;
        height += m_tree->itemDelegate()->sizeHint(option, m_tree->indexFromItem(item)).height();
        if (item->isExpanded()) for (int i = 0; i < item->childCount(); ++i) add(item->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) add(m_tree->topLevelItem(i));
    if (m_tree->horizontalScrollBar()->maximum() > 0) height += m_tree->horizontalScrollBar()->sizeHint().height();
    if (m_tree->height() != height) m_tree->setFixedHeight(height);
}

void DataTable::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateContentHeight();
}

void DataTable::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateContentHeight();
}

bool DataTable::eventFilter(QObject *object, QEvent *event)
{
    if (m_contentSized && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const bool vertical = wheel->angleDelta().y() != 0 || wheel->pixelDelta().y() != 0;
        if (vertical && !(wheel->modifiers() & Qt::ShiftModifier)) {
            for (auto *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
                if (auto *scroll = qobject_cast<QScrollArea *>(ancestor)) {
                    QWheelEvent forwarded(scroll->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),
                        wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(),
                        wheel->modifiers(), wheel->phase(), wheel->inverted());
                    QApplication::sendEvent(scroll->viewport(), &forwarded);
                    wheel->accept();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(object, event);
}

void DataTable::setBadgeColumns(const QList<int> &columns)
{
    m_badgeColumns = columns;
    auto *header = m_tree->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
}

void DataTable::setEmpty(bool empty)
{
    m_tree->setVisible(!empty);
    m_footer->setVisible(!empty && (m_nameButton || m_detailsButton));
}

void DataTable::setAvailability(bool available, const QString &message)
{
    m_available = available;
    m_message = message;
    updateStatus();
}

void DataTable::updateStatus()
{
    vusiif::ztgui::ui::setText(m_status, m_available ? QString() : m_message);
}

QString DataTable::selectedId() const
{
    const auto selected = m_tree->selectedItems();
    return selected.size() == 1 ? selected.first()->data(0, Qt::UserRole).toString() : QString();
}

QString DataTable::defaultName(QTreeWidgetItem *item) const
{
    const auto object = item->data(0, Qt::UserRole + 1).toJsonObject();
    const QString name = m_kind == "networks" ? object["name"].toString() : QString();
    return name.isEmpty() ? item->data(0, Qt::UserRole).toString() : name;
}

void DataTable::editRemark()
{
    if (!m_names || selectedId().isEmpty() || !m_editingId.isEmpty()) return;
    auto *item = m_tree->selectedItems().first();
    const QString id = selectedId();
    m_editingId = id;
    auto *editor = new QLineEdit(item->text(0), m_tree);
    editor->setObjectName("remarkEditor");
    new RemarkCommitFilter(editor);
    m_tree->setItemWidget(item, 0, editor);
    connect(editor, &QLineEdit::editingFinished, this, [this, editor, id] {
        if (m_editingId != id) return;
        m_editingId.clear();
        const QString name = editor->text().trimmed();
        const QString key = "names/" + m_kind + "/" + id;
        QTreeWidgetItem *target = nullptr;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            if (m_tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == id) target = m_tree->topLevelItem(i);
        if (!target) return;
        const QString fallback = defaultName(target);
        if (name.isEmpty() || name == fallback) QSettings().remove(key);
        else QSettings().setValue(key, name);
        target->setText(0, name.isEmpty() ? fallback : name);
        target->setToolTip(0, target->text(0));
        m_tree->removeItemWidget(target, 0);
    });
    editor->selectAll();
    editor->setFocus();
}

void DataTable::renameSelected()
{
    if (!m_names || m_tree->selectedItems().size() != 1) return;
    const QString id = selectedId();
    const QString key = "names/" + m_kind + "/" + id;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("编辑名称"),
        QStringLiteral("为 %1 设置本机名称（留空恢复为 ID）：").arg(id), QLineEdit::Normal,
        QSettings().value(key, id).toString(), &ok).trimmed();
    if (!ok) return;
    QSettings settings;
    if (name.isEmpty() || name == id) settings.remove(key);
    else settings.setValue(key, name);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto *item = m_tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == id) {
            item->setText(0, name.isEmpty() ? id : name);
            item->setToolTip(0, item->text(0));
            break;
        }
    }
}

void DataTable::showDetails()
{
    const auto selected = m_tree->selectedItems();
    if (selected.size() != 1) return;
    if (m_details) { m_details->close(); delete m_details; }
    const auto *item = selected.first();
    auto object = item->data(0, Qt::UserRole + 1).toJsonObject();
    if (m_names) object.insert("localName", item->text(0));
    m_details = new AnimatedDialog(this, Qt::Tool);
    m_details->setObjectName("detailsPopup");
    m_details->setWindowTitle(QStringLiteral("详细信息 — %1").arg(item->text(0)));
    m_details->resize(680, 480);
    auto *layout = new QVBoxLayout(m_details);
    auto *tabs = new QTabWidget;
    auto *fields = new QTreeWidget;
    fields->setHeaderLabels({QStringLiteral("字段"), QStringLiteral("值")});
    fields->header()->resizeSection(0, 210);
    for (auto it = object.begin(); it != object.end(); ++it) appendJson(fields->invisibleRootItem(), it.key(), it.value());
    fields->expandToDepth(1);
    enableDragScrolling(fields);
    tabs->addTab(fields, QStringLiteral("完整信息"));
    auto *json = new QPlainTextEdit;
    json->setObjectName("detailsJson");
    json->setReadOnly(true);
    json->setPlainText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)));
    tabs->addTab(json, QStringLiteral("原始 JSON"));
    layout->addWidget(tabs);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    connect(close, &QPushButton::clicked, m_details, &QDialog::close);
    layout->addWidget(close, 0, Qt::AlignRight);
    m_details->show();
}

} // namespace vusiif::ztgui::ui
