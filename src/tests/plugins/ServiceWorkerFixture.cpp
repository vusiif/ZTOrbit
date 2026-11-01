#include <ztprotocol/ServiceWorker.h>
#include <QFile>
#include <QThread>
#include <QProcess>
#include <QDebug>
using namespace vusiif::zto;
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(qEnvironmentVariableIntValue("ZTO_FIXTURE_LOG_STARTUP")) qInfo()<<"fixture-startup";
    if(app.arguments().contains("--ownership-child")) return app.exec();
    QJsonArray provides{QJsonObject{{"service","fixture.audio"},{"api","1"}},QJsonObject{{"service","fixture.video"},{"api","1"}}};
    if(qEnvironmentVariableIntValue("ZTO_WORKER_BAD_CAPABILITY")) provides.removeLast();
    protocol::service::Worker worker(provides);
    const auto preparationDelay=qEnvironmentVariableIntValue("ZTO_WORKER_PREPARATION_DELAY");
    if(preparationDelay>0) {
        worker.deferReady();
        worker.onConnected=[&] { QTimer::singleShot(preparationDelay,&worker,[&] { worker.markReady(); }); };
    }
    QSet<QString> sessions;
    worker.onCall=[&](const QString &lease,const QString &,const QString &method,const QJsonObject &data,const QByteArray &bytes,protocol::service::Worker::Reply reply) {
        sessions.insert(lease);
        if(method=="crash") {
#ifdef Q_OS_WIN
            TerminateProcess(GetCurrentProcess(),7);
#else
            std::_Exit(7);
#endif
            return;
        }
        if(method=="hang") return;
        if(method=="spawn-child") {
            auto *child=new QProcess(&worker);
            QObject::connect(child,&QProcess::started,&worker,[child,reply] { reply({{"pid",QString::number(child->processId())}}, {}, {}); });
            child->start(QCoreApplication::applicationFilePath(),{"--ownership-child"});
            return;
        }
        reply({{"sessions",sessions.size()},{"consumer",data["consumer"]}},bytes,{});
    };
    worker.onRelease=[&](const QString &lease) { sessions.remove(lease); };
    worker.onShutdown=[&] { sessions.clear(); const auto delay=qBound(0,qEnvironmentVariableIntValue("ZTO_WORKER_SHUTDOWN_DELAY"),10000); if(delay) QThread::msleep(delay); };
    if(qEnvironmentVariableIntValue("ZTO_WORKER_READY_DELAY")>0) {
        QTimer::singleShot(qEnvironmentVariableIntValue("ZTO_WORKER_READY_DELAY"),&worker,[&] { if(!worker.start(app.arguments())) QCoreApplication::exit(2); });
    } else if(!worker.start(app.arguments())) return 2;
    return app.exec();
}
