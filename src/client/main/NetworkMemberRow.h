#pragma once
#include <QFrame>
#include <QJsonObject>
class QLabel;
class QLineEdit;
class QStackedWidget;

namespace vusiif::ztgui::ui {
class NetworkMemberRow final : public QFrame {
    Q_OBJECT
public:
    explicit NetworkMemberRow(const QString &id, QWidget *parent = nullptr);
    void updateMember(const QJsonObject &member);
    QString memberId() const { return m_id; }
    QString memberName() const;
    void editRemark();
signals:
    void remarkChanged(const QString &id);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QString m_id;
    QLabel *m_name;
    QLabel *m_ip;
    QLineEdit *m_editor;
    QStackedWidget *m_nameStack;
    bool m_editing = false;
    void finishRemark(bool save);
};
}
