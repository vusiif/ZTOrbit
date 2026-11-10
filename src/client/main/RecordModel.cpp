#include "RecordModel.h"
#include <QCollator>
#include <QSet>
#include <algorithm>

namespace vusiif::zto::ui {
namespace {
// Placeholders ("—", "-", "不可达", empty) mean "no value" and always sort last.
bool isPlaceholder(const QString &value)
{
    const auto text = value.trimmed();
    return text.isEmpty() || text == QStringLiteral("—") || text == QStringLiteral("-") || text == QStringLiteral("不可达");
}
// Latency cells look like "12", "12.5 ms" or "<1 ms"; "<1" sorts just below 1.
bool sortNumber(QString text, double &value)
{
    text = text.trimmed();
    const bool below = text.startsWith(QLatin1Char('<'));
    if (below) text.remove(0, 1);
    if (text.endsWith(QStringLiteral("ms"), Qt::CaseInsensitive)) text.chop(2);
    bool ok = false;
    value = text.trimmed().toDouble(&ok);
    if (ok && below) value -= 0.001;
    return ok;
}
}

int compareSortValues(const QString &left, const QString &right, bool ascending)
{
    const bool leftEmpty = isPlaceholder(left), rightEmpty = isPlaceholder(right);
    if (leftEmpty || rightEmpty) return leftEmpty == rightEmpty ? 0 : leftEmpty ? 1 : -1;
    double leftValue = 0, rightValue = 0;
    int result = 0;
    if (sortNumber(left, leftValue) && sortNumber(right, rightValue)) {
        result = leftValue < rightValue ? -1 : leftValue > rightValue ? 1 : 0;
    } else {
        // Natural, case-insensitive order: "node2" before "node10", IPs by octet.
        static const QCollator collator = [] {
            QCollator value; value.setNumericMode(true); value.setCaseSensitivity(Qt::CaseInsensitive); return value;
        }();
        result = collator.compare(left, right);
    }
    return ascending ? result : -result;
}
QHash<int, QByteArray> RecordModel::roleNames() const
{
    static const QHash<int, QByteArray> roles{{RowId, "rowId"}, {Name, "name"}, {Cells, "cells"}, {Details, "details"},
            {Expanded, "expanded"}, {Members, "members"}, {MemberStatus, "memberStatus"}};
    return roles;
}
QVariant RecordModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    if (role == Members) return QVariant::fromValue(m_memberModels.value(m_rows[index.row()].value("rowId").toString()));
    return m_rows[index.row()].value(QString::fromUtf8(roleNames().value(role)));
}
QVariantMap RecordModel::record(const QString &id) const
{
    for (const auto &row : m_rows) if (row.value("rowId").toString() == id) return row;
    return {};
}
void RecordModel::update(const QString &id, const QVariantMap &fields)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].value("rowId").toString() != id) continue;
        QList<int> changed;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (m_rows[i].value(it.key()) == it.value()) continue;
            m_rows[i].insert(it.key(), it.value());
            if (it.key() == "members") {
                QList<QVariantMap> members;
                for (const auto &value : it.value().toList()) {
                    const auto member = value.toMap();
                    members.append({{"rowId", member.value("id")}, {"name", member.value("name")},
                        {"cells", QStringList{member.value("ip").toString(),member.value("latency").toString()}}, {"details",QVariantMap{{"latencySource",member.value("latencySource")}}}});
                }
                auto *model = m_memberModels.value(id);
                if (!model && !members.isEmpty()) {
                    model = new RecordModel(this);
                    m_memberModels.insert(id, model);
                    changed.append(Members);
                }
                if (model) model->updateRows(members);
                continue;
            }
            const auto roles = roleNames();
            for (auto role = roles.begin(); role != roles.end(); ++role)
                if (QString::fromUtf8(role.value()) == it.key()) changed.append(role.key());
        }
        if (!changed.isEmpty()) emit dataChanged(index(i), index(i), changed);
        return;
    }
}
void RecordModel::updateRows(const QList<QVariantMap> &rows)
{
    QSet<QString> ids;
    QList<QVariantMap> unique;
    for (const auto &row : rows) {
        const auto id = row.value("rowId").toString();
        if (!id.isEmpty() && !ids.contains(id)) { ids.insert(id); unique.append(row); }
    }
    if (m_sortColumn >= 0) {
        const auto text = [this](const QVariantMap &row) { return m_sortColumn == 0 ? row.value("name").toString() : row.value("cells").toStringList().value(m_sortColumn - 1); };
        std::stable_sort(unique.begin(), unique.end(), [&](const auto &a, const auto &b) {
            return compareSortValues(text(a), text(b), m_sortAscending) < 0;
        });
    }
    const int previousCount = count();
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (ids.contains(m_rows[i].value("rowId").toString())) continue;
        const auto id = m_rows[i].value("rowId").toString();
        beginRemoveRows({}, i, i); m_rows.removeAt(i); endRemoveRows();
        delete m_memberModels.take(id);
    }
    for (int to = 0; to < unique.size(); ++to) {
        const auto id = unique[to].value("rowId").toString();
        int from = to;
        while (from < m_rows.size() && m_rows[from].value("rowId").toString() != id) ++from;
        if (from == m_rows.size()) {
            auto row = unique[to];
            row.insert("expanded", false); row.insert("members", QVariantList{}); row.insert("memberStatus", QString{});
            beginInsertRows({}, to, to); m_rows.insert(to, row); endInsertRows();
        } else {
            if (from != to) { beginMoveRows({}, from, from, {}, to); m_rows.move(from, to); endMoveRows(); }
            update(id, unique[to]);
        }
    }
    if (count() != previousCount) emit countChanged();
}
void RecordModel::sortBy(int column, bool ascending)
{
    if (m_sortColumn != column || m_sortAscending != ascending) {
        m_sortColumn = column; m_sortAscending = ascending;
        emit sortChanged();
    }
    updateRows(m_rows);
}
}
