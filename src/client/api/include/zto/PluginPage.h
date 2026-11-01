#pragma once
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariantMap>

namespace vusiif::zto::api {
// Each page must own its controller. Do not register DLL-owned global QML types:
// pages and controllers have to be destroyed before the library is unloaded.
inline QQuickItem *createQmlPage(QQmlEngine *engine, QQuickItem *parent,
                               const QUrl &source, const QVariantMap &properties = {})
{
    if (!engine || !parent) return nullptr;
    QQmlComponent component(engine, source);
    if (!component.isReady()) return nullptr;
    auto *object = component.beginCreate(engine->rootContext());
    auto *item = qobject_cast<QQuickItem *>(object);
    if (!item) { component.completeCreate(); delete object; return nullptr; }
    item->setParent(parent);
    item->setParentItem(parent);
    component.setInitialProperties(item, properties);
    component.completeCreate();
    if (component.isError()) { delete item; return nullptr; }
    QQmlEngine::setObjectOwnership(item, QQmlEngine::CppOwnership);
    return item;
}
}
