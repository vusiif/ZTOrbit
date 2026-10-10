#pragma once

#include <QWidget>
#include <QJsonObject>
#include <QTreeWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QDialog;

struct TableRow {
    QString id;
    QStringList cells; // Name is inserted separately as the first column.
    QJsonObject details;
};

// Adds mouse panning without taking away clicks, double clicks or wheel scrolling.
void enableDragScrolling(QAbstractItemView *view);

class DataTable : public QWidget {
    Q_OBJECT
public:
    DataTable(const QString &kind, const QStringList &headers, QWidget *parent = nullptr,
              bool names = true, bool actions = true);
    void setRows(const QList<TableRow> &rows);
    void setAvailability(bool available, const QString &message);
    QString selectedId() const;
    QTreeWidget *tree() const { return m_tree; }
    QVBoxLayout *bodyLayout() const { return m_layout; }
    void renameSelected();
    void editRemark();
    void showDetails();
    void setBadgeColumns(const QList<int> &columns);
    void setEmpty(bool empty);
    void setContentSized(bool enabled);
    void setInlineAction(QWidget *widget);
    QTreeWidgetItem *inlineActionItem() const { return m_actionRow; }
    int rowCount() const { return m_tree->topLevelItemCount() - (m_actionRow ? 1 : 0); }
protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QString m_kind;
    bool m_names;
    QTreeWidget *m_tree;
    QLabel *m_status;
    QPushButton *m_detailsButton = nullptr;
    QPushButton *m_nameButton = nullptr;
    QVBoxLayout *m_layout;
    QWidget *m_footer;
    QList<int> m_badgeColumns;
    QDialog *m_details = nullptr;
    bool m_available = false;
    QString m_message = QStringLiteral("正在读取…");
    void updateStatus();
    QString defaultName(QTreeWidgetItem *item) const;
    QString m_editingId;
    QTreeWidgetItem *m_actionRow = nullptr;
    bool m_contentSized = false;
    void updateContentHeight();
};
