#pragma once

#include <atomic>
#include <memory>

namespace vusiif::zto::util {
class CancellationSource;

// Copyable, thread-safe observation of a cooperative stop request.
class CancellationToken {
public:
    CancellationToken() = default;
    bool isCancellationRequested() const noexcept {
        return m_requested && m_requested->load(std::memory_order_acquire);
    }
private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> requested)
        : m_requested(std::move(requested)) {}
    std::shared_ptr<const std::atomic<bool>> m_requested;
};

class CancellationSource {
public:
    CancellationSource() : m_requested(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const noexcept { return CancellationToken(m_requested); }
    void requestCancellation() const noexcept { m_requested->store(true, std::memory_order_release); }
private:
    std::shared_ptr<std::atomic<bool>> m_requested;
};
}
