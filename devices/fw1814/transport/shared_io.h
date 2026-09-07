#pragma once

#include "../hal/include/macfw_fw1814_capture_shm.h"
#include "../hal/include/macfw_fw1814_hal_shm.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace macfw::fw1814::transport {

class SharedPlaybackReader {
public:
    ~SharedPlaybackReader() { reset(); }

    bool open() {
        reset();
        fd_ = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDWR, 0);
        if (fd_ < 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_); fd_ = -1;
            return false;
        }
        ring_ = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
        if (!macfw::fw1814::hal::valid(*ring_)) {
            reset();
            return false;
        }
        return true;
    }

    void discardBacklog() {
        if (!ring_) return;
        const auto w = ring_->writeFrame.load(std::memory_order_acquire);
        ring_->readFrame.store(w, std::memory_order_release);
    }

    macfw::fw1814::hal::SharedPlaybackRing* ring() { return ring_; }

private:
    void reset() {
        if (ring_) munmap(ring_, sizeof(*ring_));
        ring_ = nullptr;
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
    macfw::fw1814::hal::SharedPlaybackRing* ring_ = nullptr;
};

class SharedCaptureWriter {
public:
    ~SharedCaptureWriter() { reset(); }

    bool open(std::uint32_t sampleRate) {
        reset();

        const char* const name = macfw::fw1814::hal::capture::kShmName;
        const std::size_t bytes =
            sizeof(macfw::fw1814::hal::capture::SharedCaptureRing);

        // The transport owns this versioned capture object. Reuse it when its
        // ABI size is already correct; macOS can reject an unnecessary
        // ftruncate() on an existing POSIX SHM object with EINVAL. If the
        // object is stale/wrong-sized, replace it before any consumer is active.
        fd_ = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            std::fprintf(stderr, "FW1814 capture shm_open(%s) failed: %s\n",
                         name, std::strerror(errno));
            return false;
        }

        struct stat st{};
        if (fstat(fd_, &st) != 0) {
            const int savedErrno = errno;
            std::fprintf(stderr, "FW1814 capture fstat(%s) failed: %s\n",
                         name, std::strerror(savedErrno));
            reset();
            return false;
        }

        if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) != bytes) {
            const auto oldSize = st.st_size;
            close(fd_);
            fd_ = -1;

            if (shm_unlink(name) != 0 && errno != ENOENT) {
                const int savedErrno = errno;
                std::fprintf(stderr,
                             "FW1814 capture shm_unlink(%s) failed while repairing size %lld -> %zu: %s\n",
                             name, static_cast<long long>(oldSize), bytes,
                             std::strerror(savedErrno));
                return false;
            }

            fd_ = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd_ < 0) {
                const int savedErrno = errno;
                std::fprintf(stderr,
                             "FW1814 capture recreate shm_open(%s) failed: %s\n",
                             name, std::strerror(savedErrno));
                return false;
            }

            if (ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
                const int savedErrno = errno;
                std::fprintf(stderr,
                             "FW1814 capture initial ftruncate(%zu) failed: %s\n",
                             bytes, std::strerror(savedErrno));
                reset();
                shm_unlink(name);
                return false;
            }

            std::fprintf(stdout,
                         "FW1814 capture SHM recreated: %s (%lld -> %zu bytes)\n",
                         name, static_cast<long long>(oldSize), bytes);
        }

        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            const int savedErrno = errno;
            std::fprintf(stderr,
                         "FW1814 capture mmap(%zu) failed: %s\n",
                         bytes, std::strerror(savedErrno));
            reset();
            return false;
        }

        ring_ = static_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(p);
        macfw::fw1814::hal::capture::initialize(*ring_, sampleRate);
        ring_->active.store(0, std::memory_order_release);

        std::fprintf(stdout,
                     "FW1814 capture SHM ready: %s (%zu bytes, %u Hz, %u channels)\n",
                     name, bytes, sampleRate,
                     macfw::fw1814::hal::capture::kInputChannels);
        return true;
    }

    bool activateForConsumer(std::size_t prefillFrames) {
        if (!ring_) return false;
        if (ring_->active.load(std::memory_order_acquire) != 0) return true;
        if (ring_->halReadCalls.load(std::memory_order_acquire) == 0) return false;
        const auto w = ring_->writeFrame.load(std::memory_order_acquire);
        const auto r = ring_->readFrame.load(std::memory_order_acquire);
        const std::size_t available = static_cast<std::size_t>(w - r);
        if (available < prefillFrames) return false;
        ring_->readFrame.store(w - prefillFrames, std::memory_order_release);
        ring_->active.store(1, std::memory_order_release);
        return true;
    }

    macfw::fw1814::hal::capture::SharedCaptureRing* ring() { return ring_; }

private:
    void reset() {
        if (ring_) {
            ring_->active.store(0, std::memory_order_release);
            munmap(ring_, sizeof(*ring_));
        }
        ring_ = nullptr;
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
    macfw::fw1814::hal::capture::SharedCaptureRing* ring_ = nullptr;
};

} // namespace macfw::fw1814::transport
