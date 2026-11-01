#pragma once
#include <ztmedia/MediaTypes.h>
#include <QUuid>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace vusiif::zto::media::detail {
// Three independent producer/consumer slots. Windows interlocked operations
// publish data before READY and release it after the synchronous callback.
class SharedFrames {
  public:
    static constexpr int Slots = 3, SlotBytes = 1920 * 1080 * 4;
    static constexpr qsizetype HeaderBytes = 64, Bytes = HeaderBytes + qsizetype(Slots) * SlotBytes;
    SharedFrames() = default;
    SharedFrames(const SharedFrames &) = delete;
    ~SharedFrames() { close(); }
    bool create(quint64 generation) {
#ifdef Q_OS_WIN
        close();
        name = QString::fromLatin1(MappingPrefix) + QUuid::createUuid().toString(QUuid::Id128);
        epoch = generation;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(Bytes),
                                     reinterpret_cast<LPCWSTR>(name.utf16()));
        if (!mapping)
            return false;
        view = static_cast<uchar *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Bytes));
        if (!view) {
            close();
            return false;
        }
        std::memset(view, 0, HeaderBytes);
        std::memcpy(view + 48, &epoch, sizeof epoch);
        return true;
#else
        Q_UNUSED(generation);
        return false;
#endif
    }
    bool open(const QString &key, quint64 generation) {
#ifdef Q_OS_WIN
        close();
        name = key;
        epoch = generation;
        if (!name.startsWith(QLatin1StringView(MappingPrefix)) || name.size() != MappingKeyLength)
            return false;
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
        if (!mapping)
            return false;
        view = static_cast<uchar *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Bytes));
        quint64 actual = 0;
        if (view)
            std::memcpy(&actual, view + 48, sizeof actual);
        if (!view || actual != epoch) {
            close();
            return false;
        }
        return true;
#else
        Q_UNUSED(key);
        Q_UNUSED(generation);
        return false;
#endif
    }
    QString key() const { return name; }
    quint64 overwritten() const { return replacements; }
    int publish(const uchar *bytes, qsizetype size, quint64 serial = 0) {
#ifdef Q_OS_WIN
        if (!view || size < 0 || size > SlotBytes)
            return -1;
        for (LONG expected : {LONG(0), LONG(2)})
            for (int i = 0; i < Slots; ++i)
                if (InterlockedCompareExchange(state(i), 1, expected) == expected) {
                    if (expected == 2)
                        ++replacements;
                    std::memcpy(view + HeaderBytes + qsizetype(i) * SlotBytes, bytes, size);
                    std::memcpy(view + 16 + i * 8, &serial, sizeof serial);
                    InterlockedExchange(state(i), 2);
                    return i;
                }
#else
        Q_UNUSED(bytes);
        Q_UNUSED(size);
#endif
        return -1;
    }
    const uchar *acquire(int slot, qsizetype size, quint64 serial = 0) {
#ifdef Q_OS_WIN
        if (view && slot >= 0 && slot < Slots && size >= 0 && size <= SlotBytes &&
            InterlockedCompareExchange(state(slot), 3, 2) == 2) {
            quint64 actual = 0;
            std::memcpy(&actual, view + 16 + slot * 8, sizeof actual);
            if (actual != serial) {
                InterlockedExchange(state(slot), 2);
                return nullptr;
            }
            return view + HeaderBytes + qsizetype(slot) * SlotBytes;
        }
#else
        Q_UNUSED(slot);
        Q_UNUSED(size);
#endif
        return nullptr;
    }
    void release(int slot) {
#ifdef Q_OS_WIN
        if (view && slot >= 0 && slot < Slots)
            InterlockedCompareExchange(state(slot), 0, 3);
#else
        Q_UNUSED(slot);
#endif
    }
    void discard(int slot, quint64 serial) {
#ifdef Q_OS_WIN
        if (view && slot >= 0 && slot < Slots && InterlockedCompareExchange(state(slot), 1, 2) == 2) {
            quint64 actual = 0;
            std::memcpy(&actual, view + 16 + slot * 8, sizeof actual);
            InterlockedExchange(state(slot), actual == serial ? 0 : 2);
        }
#else
        Q_UNUSED(slot);
        Q_UNUSED(serial);
#endif
    }
    void close() {
#ifdef Q_OS_WIN
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
        view = nullptr;
        mapping = nullptr;
#endif
        name.clear();
    }

  private:
    static constexpr char MappingPrefix[] = "Local\\zto-frame-";
    static constexpr qsizetype MappingKeyLength = sizeof(MappingPrefix) - 1 + 32;
    QString name;
    quint64 replacements = 0;
    quint64 epoch = 0;
#ifdef Q_OS_WIN
    HANDLE mapping = nullptr;
    uchar *view = nullptr;
    volatile LONG *state(int i) { return reinterpret_cast<volatile LONG *>(view) + i; }
#endif
};
} // namespace vusiif::zto::media::detail
