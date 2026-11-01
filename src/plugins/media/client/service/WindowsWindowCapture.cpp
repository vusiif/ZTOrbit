#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "WindowsWindowCapture.h"
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <QImage>
#include <QTimer>
#include <QDebug>
#include <QThread>
#include <QScopeGuard>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace vusiif::zto::media::engine {
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
namespace {
struct Apartment {
    bool release = false;
    int type = -1;
    Apartment() {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        check_hresult(hr);
        release = true;
        APTTYPE actual; APTTYPEQUALIFIER qualifier;
        check_hresult(CoGetApartmentType(&actual, &qualifier));
        type = int(actual);
        if (qEnvironmentVariableIntValue("ZTO_DESKTOP_SHARE_DIAGNOSTICS")) {
            qInfo() << "wgc apartment" << qint64(hr) << "type" << type;
        }
    }
    ~Apartment() { if (release) CoUninitialize(); }
};
QString message(const hresult_error &error) {
    return QStringLiteral("Window capture failed (0x%1): %2")
        .arg(quint32(error.code()), 8, 16, QLatin1Char('0'))
        .arg(QString::fromWCharArray(error.message().c_str()));
}
bool sameWindow(const WindowsWindowCapture::Window &window) {
    auto hwnd = reinterpret_cast<HWND>(window.handle);
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(hwnd, &process);
    return IsWindow(hwnd) && process == window.process && thread == window.thread;
}
bool captureRestricted(const WindowsWindowCapture::Window &window) {
    DWORD affinity = WDA_NONE;
    // A successful query is an explicit source policy. A failed query, or
    // absence of frames, says nothing about whether capture is permitted.
    return GetWindowDisplayAffinity(reinterpret_cast<HWND>(window.handle), &affinity)
        && (affinity == WDA_MONITOR || affinity == WDA_EXCLUDEFROMCAPTURE);
}
QString captureDenied() {
    return QStringLiteral("Window capture denied: source is protected from capture");
}
}
class WindowCaptureWorker final : public QObject {
public:
    using Window = WindowsWindowCapture::Window;
    explicit WindowCaptureWorker(QObject *parent = nullptr);
    ~WindowCaptureWorker() override;
    QString start(const Window &window, int frameRate);
    void stop();
    bool isActive() const;
    QJsonObject statistics() const;
    std::function<void(QVideoFrame)> frame;
    std::function<void(QString)> failed;
private:
    struct State;
    std::unique_ptr<State> state;
    void poll();
};
struct WindowCaptureWorker::State {
    Apartment apartment; // Outlives every COM/WinRT object below.
    Window window;
    QTimer timer;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice captureDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    event_token arrival{}, closed{};
    bool arrivalRegistered = false, closedRegistered = false;
    struct Events {
        std::atomic<quint64> arrivals{0};
        std::atomic<bool> closed{false};
        std::atomic<const char *> phase{"idle"};
        std::atomic<quint64> phaseSince{0};
        std::atomic<quint64> lastPoll{0};
        std::atomic<bool> active{false};
        std::mutex mutex;
        std::condition_variable wake;
        bool stopMonitor = false;
    };
    std::shared_ptr<Events> events = std::make_shared<Events>();
    SizeInt32 poolSize{}, itemSize{}, firstSize{};
    QSize windowRectSize;
    quint64 acquired = 0, converted = 0, recreates = 0;
    bool active = false;
    std::thread monitor;
    void traceClose(const char *phase) const {
        events->phaseSince = GetTickCount64();
        events->phase = phase;
    }
    void startMonitor() {
        if (!qEnvironmentVariableIntValue("ZTO_DESKTOP_SHARE_DIAGNOSTICS")) return;
        const auto shared = events;
        const auto handle = window.handle;
        monitor = std::thread([shared, handle] {
            std::unique_lock<std::mutex> lock(shared->mutex);
            quint64 reported = 0;
            while (!shared->wake.wait_for(lock, std::chrono::seconds(1), [&] { return shared->stopMonitor; })) {
                const auto since = shared->phaseSince.load();
                const auto phase = shared->phase.load();
                if (since && since != reported && GetTickCount64() - since > 2000 && std::strcmp(phase,"idle")) {
                    reported = since;
                    qWarning() << "wgc stalled" << quint64(handle) << phase
                               << "elapsedMs" << GetTickCount64()-since
                               << "closed" << shared->closed.load() << "notifications" << shared->arrivals.load();
                } else if (shared->active && shared->lastPoll && GetTickCount64()-shared->lastPoll > 2000) {
                    qWarning() << "wgc device loop stalled" << quint64(handle) << phase
                               << "pollGapMs" << GetTickCount64()-shared->lastPoll
                               << "closed" << shared->closed.load();
                }
            }
        });
    }
    void close() noexcept {
        active = false;
        events->active = false;
        timer.stop();
        if (!pool && !session) return;
        traceClose("begin");
        // A device loss or an already closed capture item must not turn stop
        // or destruction into an exception. Event handlers own only Events.
        if (pool && arrivalRegistered) {
            traceClose("revoke-arrival");
            try { pool.FrameArrived(arrival); } catch (const hresult_error &) {}
            arrivalRegistered = false;
        }
        if (item && closedRegistered) {
            traceClose("revoke-closed");
            try { item.Closed(closed); } catch (const hresult_error &) {}
            closedRegistered = false;
        }
        if (session) {
            traceClose("session-close");
            try { session.Close(); } catch (const hresult_error &) {}
            session = nullptr;
            traceClose("session-closed");
        }
        if (pool) {
            traceClose("pool-close");
            try { pool.Close(); } catch (const hresult_error &) {}
            pool = nullptr;
            traceClose("pool-closed");
        }
        item = nullptr;
        staging = nullptr;
        captureDevice = nullptr;
        context = nullptr;
        device = nullptr;
        traceClose("idle");
    }
    ~State() {
        close();
        { std::lock_guard<std::mutex> lock(events->mutex); events->stopMonitor = true; }
        events->wake.notify_all();
        if (monitor.joinable()) monitor.join();
    }
};
QList<WindowsWindowCapture::Window> WindowsWindowCapture::windows() {
    QList<Window> result;
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd || hwnd == GetShellWindow()) return TRUE;
        const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (style & WS_EX_TOOLWINDOW) return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
        const int length = GetWindowTextLengthW(hwnd);
        if (length <= 0) return TRUE;
        std::wstring title(size_t(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
        if (copied <= 0) return TRUE;
        Window window;
        window.handle = reinterpret_cast<quintptr>(hwnd);
        DWORD process = 0;
        window.thread = GetWindowThreadProcessId(hwnd, &process);
        window.process = process;
        window.title = QString::fromWCharArray(title.data(), copied);
        reinterpret_cast<QList<Window> *>(parameter)->append(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
WindowCaptureWorker::WindowCaptureWorker(QObject *parent) : QObject(parent) {}
WindowCaptureWorker::~WindowCaptureWorker() = default;
QString WindowCaptureWorker::start(const Window &window, int frameRate) {
    stop();
    if (!sameWindow(window)) return QStringLiteral("Window unavailable; enumerate sources again");
    if (captureRestricted(window)) return captureDenied();
    try {
        auto next = std::make_unique<State>();
        auto &s = *next;
        if (!GraphicsCaptureSession::IsSupported()) return QStringLiteral("Windows Graphics Capture is unavailable");
        s.window = window;
        const auto hwnd = reinterpret_cast<HWND>(window.handle);
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        s.windowRectSize = QSize(rect.right - rect.left, rect.bottom - rect.top);
        const auto factory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(factory->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(s.item)));
        // The capture item size excludes invisible resize borders. Using the
        // Win32 rectangle here and discarding a differently sized first frame
        // can lose the only frame of an entirely static window.
        s.itemSize = s.poolSize = s.item.Size();
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            s.device.put(), nullptr, s.context.put()));
        auto dxgi = s.device.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        s.captureDevice = inspectable.as<IDirect3DDevice>();
        s.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(s.captureDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, s.poolSize);
        const auto events = s.events;
        s.arrival = s.pool.FrameArrived([events](auto &&, auto &&) { ++events->arrivals; });
        s.arrivalRegistered = true;
        s.closed = s.item.Closed([events](auto &&, auto &&) { events->closed = true; });
        s.closedRegistered = true;
        s.session = s.pool.CreateCaptureSession(s.item);
        s.timer.setTimerType(Qt::PreciseTimer);
        s.timer.setInterval(std::max(5, 1000 / std::clamp(frameRate, 1, 60)));
        connect(&s.timer, &QTimer::timeout, this, [this] { poll(); });
        state = std::move(next);
        state->startMonitor();
        state->session.StartCapture();
        state->active = true;
        state->events->active = true;
        state->events->lastPoll = GetTickCount64();
        state->timer.start();
    } catch (const hresult_error &error) {
        stop();
        return message(error);
    }
    return {};
}
void WindowCaptureWorker::stop() { if (state) state->close(); }
bool WindowCaptureWorker::isActive() const { return state && state->active; }
void WindowCaptureWorker::poll() {
    if (!isActive()) return;
    auto &s = *state;
    s.events->lastPoll = GetTickCount64();
    QString error;
    if (s.events->closed || !sameWindow(s.window)) error = QStringLiteral("Window closed or unavailable");
    else if (captureRestricted(s.window)) error = captureDenied();
    else if (IsIconic(reinterpret_cast<HWND>(s.window.handle))) { s.traceClose("idle"); return; }
    else try {
        QImage image;
        SizeInt32 size{};
        {
        s.traceClose("get-frame");
        auto captured = s.pool.TryGetNextFrame();
        if (!captured) { s.traceClose("idle"); return; }
        // Surface and texture references below are destroyed before returning
        // the borrowed frame, including on errors and empty content sizes.
        const auto returnFrame = qScopeGuard([&] {
            s.traceClose("close-frame");
            try { captured.Close(); } catch (const hresult_error &) {}
        });
        ++s.acquired;
        size = captured.ContentSize();
        if (!s.firstSize.Width) s.firstSize = size;
        if (size.Width <= 0 || size.Height <= 0) { s.traceClose("idle"); return; }
        s.traceClose("get-surface");
        auto access = captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void()));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        // During a real resize the old pool can clip the new content. Copy
        // only valid texels, then resize the pool after releasing this frame.
        const auto width = std::min(UINT(size.Width), desc.Width);
        const auto height = std::min(UINT(size.Height), desc.Height);
        D3D11_TEXTURE2D_DESC cpuDesc{};
        if (s.staging) s.staging->GetDesc(&cpuDesc);
        if (!s.staging || cpuDesc.Width != desc.Width || cpuDesc.Height != desc.Height) {
            s.staging = nullptr;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check_hresult(s.device->CreateTexture2D(&desc, nullptr, s.staging.put()));
        }
        s.traceClose("copy-resource");
        s.context->CopyResource(s.staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        s.traceClose("map-staging");
        check_hresult(s.context->Map(s.staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto unmap = qScopeGuard([&] { s.context->Unmap(s.staging.get(), 0); });
        image = QImage(int(width), int(height), QImage::Format_RGB32);
        if (!image.isNull()) for (UINT y = 0; y < height; ++y)
            std::memcpy(image.scanLine(y), static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, width * 4);
        }
        // No borrowed frame, Surface or source texture is retained across a
        // pool recreation, a delivery callback, or session teardown.
        if (size.Width != s.poolSize.Width || size.Height != s.poolSize.Height) {
            s.traceClose("recreate-pool");
            s.pool.Recreate(s.captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            s.poolSize = size;
            ++s.recreates;
        }
        if (!image.isNull()) {
            ++s.converted;
            if (frame) frame(QVideoFrame(image));
        }
        s.traceClose("idle");
    } catch (const hresult_error &failure) { error = message(failure); }
    if (!error.isEmpty()) {
        stop();
        if (failed) failed(error);
    }
}
QJsonObject WindowCaptureWorker::statistics() const {
    if (!state) return {};
    const auto &s = *state;
    return {{"backend", "windows-graphics-capture"}, {"apartmentType", s.apartment.type},
        {"notifications", QString::number(s.events->arrivals.load())},
        {"nativeFrames", QString::number(s.acquired)}, {"cpuFrames", QString::number(s.converted)},
        {"poolRecreates", QString::number(s.recreates)},
        {"windowRectWidth", s.windowRectSize.width()}, {"windowRectHeight", s.windowRectSize.height()},
        {"itemWidth", s.itemSize.Width}, {"itemHeight", s.itemSize.Height},
        {"firstFrameWidth", s.firstSize.Width}, {"firstFrameHeight", s.firstSize.Height}};
}

struct WindowsWindowCapture::State {
    struct Output {
        std::mutex mutex;
        QVideoFrame latest;
        quint64 generation = 0, offered = 0, delivered = 0, replaced = 0;
        bool queued = false;
        std::atomic<bool> active{false};
    };
    QThread thread;
    QObject dispatch;
    WindowCaptureWorker *worker = nullptr;
    std::shared_ptr<Output> output = std::make_shared<Output>();
};
WindowsWindowCapture::WindowsWindowCapture(QObject *parent) : QObject(parent), state(std::make_unique<State>()) {
    // Qt Multimedia initializes the device thread as STA. Capture resources
    // must instead be created, used and destroyed on their own MTA thread.
    state->thread.setObjectName(QStringLiteral("wgc-capture"));
    state->dispatch.moveToThread(&state->thread);
    state->thread.start();
    QMetaObject::invokeMethod(&state->dispatch, [this] { state->worker = new WindowCaptureWorker; }, Qt::BlockingQueuedConnection);
}
WindowsWindowCapture::~WindowsWindowCapture() {
    stop();
    auto *caller = QThread::currentThread();
    QMetaObject::invokeMethod(&state->dispatch, [this, caller] {
        delete state->worker;
        state->worker = nullptr;
        state->dispatch.moveToThread(caller);
    }, Qt::BlockingQueuedConnection);
    state->thread.quit();
    state->thread.wait();
}
QString WindowsWindowCapture::start(const Window &window, int frameRate) {
    stop();
    const auto output = state->output;
    quint64 generation;
    {
        std::lock_guard<std::mutex> lock(output->mutex);
        generation = ++output->generation;
        output->offered = output->delivered = output->replaced = 0;
    }
    QString error;
    QMetaObject::invokeMethod(&state->dispatch, [&, this, output, generation] {
        state->worker->frame = [this, output, generation](QVideoFrame value) {
            {
                std::lock_guard<std::mutex> lock(output->mutex);
                if (output->generation != generation) return;
                ++output->offered;
                output->latest = std::move(value);
                if (output->queued) { ++output->replaced; return; }
                output->queued = true;
            }
            QMetaObject::invokeMethod(this, [this, output, generation] {
                QVideoFrame value;
                {
                    std::lock_guard<std::mutex> lock(output->mutex);
                    if (output->generation != generation) return;
                    value = std::move(output->latest);
                    output->queued = false;
                    ++output->delivered;
                }
                if (frame) frame(std::move(value));
            }, Qt::QueuedConnection);
        };
        state->worker->failed = [this, output, generation](QString message) {
            output->active = false;
            QMetaObject::invokeMethod(this, [this, output, generation, message] {
                {
                    std::lock_guard<std::mutex> lock(output->mutex);
                    if (output->generation != generation) return;
                }
                if (failed) failed(message);
            }, Qt::QueuedConnection);
        };
        error = state->worker->start(window, frameRate);
        output->active = error.isEmpty();
    }, Qt::BlockingQueuedConnection);
    return error;
}
void WindowsWindowCapture::stop() {
    const auto output = state->output;
    {
        std::lock_guard<std::mutex> lock(output->mutex);
        ++output->generation;
        output->active = false;
        output->latest = {};
        output->queued = false;
    }
    QMetaObject::invokeMethod(&state->dispatch, [this] {
        state->worker->stop();
        state->worker->frame = {};
        state->worker->failed = {};
    }, Qt::BlockingQueuedConnection);
}
bool WindowsWindowCapture::isActive() const { return state->output->active.load(); }
QJsonObject WindowsWindowCapture::statistics() const {
    QJsonObject result;
    QMetaObject::invokeMethod(&state->dispatch, [&] { result = state->worker->statistics(); }, Qt::BlockingQueuedConnection);
    const auto output = state->output;
    std::lock_guard<std::mutex> lock(output->mutex);
    result["outputOffered"] = QString::number(output->offered);
    result["outputDelivered"] = QString::number(output->delivered);
    result["outputReplaced"] = QString::number(output->replaced);
    result["outputPending"] = output->queued;
    return result;
}
}
