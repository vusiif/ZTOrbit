#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <QtTest>
#include "WindowsWindowCapture.h"

using vusiif::zto::media::engine::WindowsWindowCapture;

namespace {
struct Source {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"ZTOrbit capture policy test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                40, 40, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Source() { if (hwnd) DestroyWindow(hwnd); }
    WindowsWindowCapture::Window window() const {
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(hwnd, &process);
        return {reinterpret_cast<quintptr>(hwnd), process, thread, QStringLiteral("policy test")};
    }
};
}

class WindowsWindowCaptureTests : public QObject {
    Q_OBJECT
private slots:
    void protectedBeforeStart_data() {
        QTest::addColumn<quint32>("affinity");
        QTest::newRow("monitor") << quint32(WDA_MONITOR);
        QTest::newRow("exclude") << quint32(WDA_EXCLUDEFROMCAPTURE);
    }
    void protectedBeforeStart() {
        QFETCH(quint32, affinity);
        Source source;
        QVERIFY(source.hwnd);
        QVERIFY(SetWindowDisplayAffinity(source.hwnd, affinity));
        WindowsWindowCapture capture;
        const auto error = capture.start(source.window(), 30);
        QVERIFY2(error.contains("protected from capture"), qPrintable(error));
        QVERIFY(!capture.isActive());
    }
    void protectedDuringCapture_data() { protectedBeforeStart_data(); }
    void protectedDuringCapture() {
        QFETCH(quint32, affinity);
        Source source;
        QVERIFY(source.hwnd);
        WindowsWindowCapture capture;
        int frames = 0, failures = 0;
        QString failure;
        capture.frame = [&](QVideoFrame) { ++frames; };
        capture.failed = [&](QString error) { ++failures; failure = error; };
        const auto error = capture.start(source.window(), 30);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(frames > 0, 5000);
        QVERIFY(SetWindowDisplayAffinity(source.hwnd, affinity));
        QTRY_COMPARE_WITH_TIMEOUT(failures, 1, 2000);
        QVERIFY2(failure.contains("protected from capture"), qPrintable(failure));
        QVERIFY(!capture.isActive());
        const auto after = frames;
        QTest::qWait(200);
        QCOMPARE(failures, 1);
        QCOMPARE(frames, after);
        // Explicit retry after the source removes its policy still works.
        QVERIFY(SetWindowDisplayAffinity(source.hwnd, WDA_NONE));
        QVERIFY(capture.start(source.window(), 30).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(frames > after, 5000);
        capture.stop();
    }
    void staticAndMinimizedRemainActive() {
        Source source;
        QVERIFY(source.hwnd);
        WindowsWindowCapture capture;
        int frames = 0, failures = 0;
        capture.frame = [&](QVideoFrame) { ++frames; };
        capture.failed = [&](QString) { ++failures; };
        QVERIFY(capture.start(source.window(), 30).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(frames > 0, 5000);
        QTest::qWait(1500); // No source drawing or motion.
        QVERIFY(capture.isActive());
        QCOMPARE(failures, 0);
        ShowWindow(source.hwnd, SW_MINIMIZE);
        QTest::qWait(1500); // No-frame intervals are not permission failures.
        QVERIFY(capture.isActive());
        QCOMPARE(failures, 0);
        capture.stop();
    }
};

QTEST_MAIN(WindowsWindowCaptureTests)
#include "WindowsWindowCaptureTests.moc"
