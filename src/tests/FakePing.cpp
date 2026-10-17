#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments().mid(1);
#ifdef Q_OS_WIN
    const QStringList expected{"-n", "1", "-w", "4000"};
#elif defined(Q_OS_MACOS)
    const QStringList expected{"-n", "-c", "1", "-W", "4000"};
#else
    const QStringList expected{"-n", "-c", "1", "-W", "4"};
#endif
    if (arguments.mid(0, expected.size()) != expected || arguments.size() != expected.size() + 1) return 2;
    QFile log(qEnvironmentVariable("ZT_TEST_PING_LOG"));
    if (log.open(QIODevice::Append)) { log.write(arguments.join(' ').toUtf8() + '\n'); log.close(); }
    const auto mode = qEnvironmentVariable("ZT_TEST_PING_MODE");
    const auto output = mode == "unreachable" ? QByteArray("Reply from 10.0.0.1: Destination host unreachable.\n")
        : QByteArray("Reply: bytes=32 time=12ms TTL=64\n");
    const int delay = mode == "hang" ? 15000 : qEnvironmentVariableIntValue("ZT_TEST_PING_DELAY");
    QTimer::singleShot(delay, &app, [&app, output] { std::fwrite(output.constData(), 1, output.size(), stdout); std::fflush(stdout); app.quit(); });
    return app.exec();
}
