#pragma once

#include "macfw_hal_transport_status.h"

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace macfw::transport {

class TransportStatusPublisher {
public:
    ~TransportStatusPublisher() {
        if (status_) munmap(status_, sizeof(*status_));
        if (fd_ >= 0) close(fd_);
    }

    bool open() {
        // haltransport is the single publisher/owner of this status block.
        // Always recreate it so stale objects from an older ABI or a crashed
        // supervisor cannot leave Darwin with an incompatible SHM object.
        if (shm_unlink(macfw::hal::transport::kShmName) != 0 && errno != ENOENT) {
            std::perror("transport status shm_unlink");
            return false;
        }

        fd_ = shm_open(macfw::hal::transport::kShmName,
                       O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd_ < 0) {
            std::perror("transport status shm_open");
            return false;
        }
        if (ftruncate(fd_, sizeof(macfw::hal::transport::SharedStatus)) != 0) {
            std::perror("transport status ftruncate");
            close(fd_);
            fd_ = -1;
            shm_unlink(macfw::hal::transport::kShmName);
            return false;
        }

        void* p = mmap(nullptr, sizeof(macfw::hal::transport::SharedStatus),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            std::perror("transport status mmap");
            close(fd_);
            fd_ = -1;
            shm_unlink(macfw::hal::transport::kShmName);
            return false;
        }

        status_ = static_cast<macfw::hal::transport::SharedStatus*>(p);
        new (status_) macfw::hal::transport::SharedStatus;
        status_->magic = macfw::hal::transport::kMagic;
        status_->version = macfw::hal::transport::kVersion;
        status_->structSize = sizeof(*status_);
        status_->reserved0 = 0;
        status_->state.store(static_cast<std::uint32_t>(macfw::hal::transport::State::Offline),
                             std::memory_order_release);
        status_->requestedRate.store(0, std::memory_order_release);
        status_->activeRate.store(0, std::memory_order_release);
        status_->enginePid.store(0, std::memory_order_release);
        status_->transitionSequence.store(0, std::memory_order_release);
        status_->heartbeatSequence.store(0, std::memory_order_release);
        lastState_ = macfw::hal::transport::State::Offline;
        lastRequestedRate_ = 0;
        lastActiveRate_ = 0;
        lastPid_ = 0;
        return true;
    }

    void publish(macfw::hal::transport::State state,
                 std::uint32_t requestedRate,
                 std::uint32_t activeRate,
                 std::uint32_t enginePid) {
        if (!status_) return;

        if (state != lastState_ ||
            requestedRate != lastRequestedRate_ ||
            activeRate != lastActiveRate_ ||
            enginePid != lastPid_) {
            status_->transitionSequence.fetch_add(1, std::memory_order_acq_rel);
            lastState_ = state;
            lastRequestedRate_ = requestedRate;
            lastActiveRate_ = activeRate;
            lastPid_ = enginePid;
        }

        status_->requestedRate.store(requestedRate, std::memory_order_release);
        status_->activeRate.store(activeRate, std::memory_order_release);
        status_->enginePid.store(enginePid, std::memory_order_release);
        status_->state.store(static_cast<std::uint32_t>(state), std::memory_order_release);
        status_->heartbeatSequence.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    int fd_ = -1;
    macfw::hal::transport::SharedStatus* status_ = nullptr;
    macfw::hal::transport::State lastState_ = macfw::hal::transport::State::Offline;
    std::uint32_t lastRequestedRate_ = 0;
    std::uint32_t lastActiveRate_ = 0;
    std::uint32_t lastPid_ = 0;
};

} // namespace macfw::transport
