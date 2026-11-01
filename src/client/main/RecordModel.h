#pragma once
#include <QAbstractListModel>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace vusiif::zto::ui {
// Stable row IDs retain delegates, focus, selection and expanded members across
// polling updates. UI state belongs to this model rather than recycled delegates.
class RecordModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role { RowId = Qt::UserRole + 1, Name, Cells, Details, Expanded, Members, MemberStatus };
    explicit RecordModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    int count() const { return m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void updateRows(const QList<QVariantMap> &rows);
    void update(const QString &id, const QVariantMap &fields);
    Q_INVOKABLE void sortBy(int column, bool ascending);
    Q_INVOKABLE QVariantMap get(int row) const { return row >= 0 && row < m_rows.size() ? m_rows[row] : QVariantMap{}; }
    QVariantMap record(const QString &id) const;
signals:
    void countChanged();
private:
    QList<QVariantMap> m_rows;
    QHash<QString, RecordModel *> m_memberModels;
    int m_sortColumn = -1;
    bool m_sortAscending = true;
};
}
