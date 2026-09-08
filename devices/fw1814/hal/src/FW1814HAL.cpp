#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include "../include/macfw_fw1814_capture_shm.h"
#include "../include/macfw_fw1814_hal_shm.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr AudioObjectID kDeviceID = 2;
constexpr AudioObjectID kOutputStreamID = 3;
constexpr AudioObjectID kInputStreamID = 4;
constexpr Float64 kRate44100 = 44100.0;
constexpr Float64 kRate48000 = 48000.0;
constexpr UInt32 kOutputChannels = macfw::fw1814::hal::kOutputChannels;
constexpr UInt32 kInputChannels = macfw::fw1814::hal::capture::kInputChannels;

AudioServerPlugInHostRef gHost = nullptr;
std::atomic<UInt32> gRefCount{1};
std::atomic<UInt32> gRunningClients{0};
std::atomic<std::uint32_t> gSampleRate{48000};
UInt64 gStartHostTime = 0;
mach_timebase_info_data_t gTimebase{};

int gPlaybackFd = -1;
macfw::fw1814::hal::SharedPlaybackRing* gPlaybackRing = nullptr;
int gCaptureFd = -1;
macfw::fw1814::hal::capture::SharedCaptureRing* gCaptureRing = nullptr;

extern AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;

bool IsKnownObject(AudioObjectID id) {
    return id == kAudioObjectPlugInObject || id == kDeviceID ||
           id == kOutputStreamID || id == kInputStreamID;
}

bool IsSupportedRate(std::uint32_t rate) {
    return rate == 44100 || rate == 48000;
}

int OpenStableShm(const char* name, std::size_t bytes) {
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) {
        if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            close(fd);
            shm_unlink(name);
            return -1;
        }
        return fd;
    }

    if (errno != EEXIST)
        return -1;

    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0)
        return -1;

    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size >= 0 &&
        static_cast<std::size_t>(st.st_size) == bytes) {
        return fd;
    }

    close(fd);
    fd = -1;

    // This HAL owns the persistent versioned SHM objects. A size mismatch
    // means an old ABI object is still present; replace it cleanly rather than
    // resizing an existing macOS POSIX SHM object in place.
    if (shm_unlink(name) != 0 && errno != ENOENT)
        return -1;

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        close(fd);
        shm_unlink(name);
        return -1;
    }
    return fd;
}

bool MapPlaybackRing() {
    if (gPlaybackRing && macfw::fw1814::hal::valid(*gPlaybackRing))
        return true;

    if (gPlaybackRing) {
        munmap(gPlaybackRing, sizeof(*gPlaybackRing));
        gPlaybackRing = nullptr;
    }
    if (gPlaybackFd >= 0) {
        close(gPlaybackFd);
        gPlaybackFd = -1;
    }

    const std::size_t bytes = sizeof(macfw::fw1814::hal::SharedPlaybackRing);
    gPlaybackFd = OpenStableShm(macfw::fw1814::hal::kPlaybackShmName, bytes);
    if (gPlaybackFd < 0)
        return false;

    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, gPlaybackFd, 0);
    if (p == MAP_FAILED) {
        close(gPlaybackFd);
        gPlaybackFd = -1;
        return false;
    }

    gPlaybackRing = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
    if (!macfw::fw1814::hal::valid(*gPlaybackRing)) {
        macfw::fw1814::hal::initialize(
            *gPlaybackRing, gSampleRate.load(std::memory_order_relaxed));
    } else {
        const std::uint32_t persistedRate =
            gPlaybackRing->sampleRate.load(std::memory_order_acquire);
        if (IsSupportedRate(persistedRate)) {
            gSampleRate.store(persistedRate, std::memory_order_relaxed);
        } else {
            const auto w =
                gPlaybackRing->writeFrame.load(std::memory_order_acquire);
            gPlaybackRing->readFrame.store(w, std::memory_order_release);
            gPlaybackRing->active.store(0, std::memory_order_release);
            gPlaybackRing->sampleRate.store(48000, std::memory_order_release);
            gSampleRate.store(48000, std::memory_order_relaxed);
        }
    }
    return true;
}

bool MapCaptureRing() {
    if (gCaptureRing && macfw::fw1814::hal::capture::valid(*gCaptureRing))
        return true;

    if (gCaptureRing) {
        munmap(gCaptureRing, sizeof(*gCaptureRing));
        gCaptureRing = nullptr;
    }
    if (gCaptureFd >= 0) {
        close(gCaptureFd);
        gCaptureFd = -1;
    }

    const std::size_t bytes = sizeof(macfw::fw1814::hal::capture::SharedCaptureRing);
    gCaptureFd = OpenStableShm(macfw::fw1814::hal::capture::kShmName, bytes);
    if (gCaptureFd < 0)
        return false;

    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, gCaptureFd, 0);
    if (p == MAP_FAILED) {
        close(gCaptureFd);
        gCaptureFd = -1;
        return false;
    }

    gCaptureRing = static_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(p);
    if (!macfw::fw1814::hal::capture::valid(*gCaptureRing)) {
        macfw::fw1814::hal::capture::initialize(
            *gCaptureRing, gSampleRate.load(std::memory_order_relaxed));
        gCaptureRing->active.store(0, std::memory_order_release);
    }
    return true;
}

AudioStreamBasicDescription Format(Float64 rate, UInt32 channels) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = sizeof(Float32) * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(Float32) * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

AudioStreamBasicDescription OutputFormat(Float64 rate) {
    return Format(rate, kOutputChannels);
}

AudioStreamBasicDescription InputFormat(Float64 rate) {
    return Format(rate, kInputChannels);
}

bool ScopeIsOutput(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeOutput;
}

bool ScopeIsInput(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeInput;
}

template <typename T>
OSStatus CopyScalar(UInt32 inSize, UInt32* outSize, void* outData, const T& value) {
    if (!outSize || !outData)
        return kAudioHardwareIllegalOperationError;
    if (inSize < sizeof(T))
        return kAudioHardwareBadPropertySizeError;
    *reinterpret_cast<T*>(outData) = value;
    *outSize = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus CopyString(UInt32 inSize, UInt32* outSize, void* outData, CFStringRef value) {
    return CopyScalar(inSize, outSize, outData, value);
}

void Notify(AudioObjectID object,
            AudioObjectPropertySelector selector,
            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    if (!gHost) return;
    AudioObjectPropertyAddress address{
        selector, scope, kAudioObjectPropertyElementMain};
    gHost->PropertiesChanged(gHost, object, 1, &address);
}

HRESULT STDMETHODCALLTYPE QueryInterface(void*, REFIID uuid, LPVOID* outInterface) {
    if (!outInterface)
        return E_POINTER;
    *outInterface = nullptr;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);
    const bool ok = requested &&
        (CFEqual(requested, IUnknownUUID) ||
         CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));
    if (requested)
        CFRelease(requested);
    if (!ok)
        return E_NOINTERFACE;

    gRefCount.fetch_add(1, std::memory_order_relaxed);
    *outInterface = &gInterfacePtr;
    return S_OK;
}

ULONG STDMETHODCALLTYPE AddRef(void*) {
    return gRefCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE Release(void*) {
    UInt32 old = gRefCount.load(std::memory_order_relaxed);
    while (old > 0 &&
           !gRefCount.compare_exchange_weak(old, old - 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}
    return old ? old - 1 : 0;
}

OSStatus STDMETHODCALLTYPE Initialize(AudioServerPlugInDriverRef,
                                      AudioServerPlugInHostRef host) {
    gHost = host;
    mach_timebase_info(&gTimebase);
    gStartHostTime = mach_absolute_time();
    MapPlaybackRing();
    MapCaptureRing();
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE CreateDevice(AudioServerPlugInDriverRef,
                                        CFDictionaryRef,
                                        const AudioServerPlugInClientInfo*,
                                        AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DestroyDevice(AudioServerPlugInDriverRef,
                                         AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE AddDeviceClient(AudioServerPlugInDriverRef,
                                           AudioObjectID device,
                                           const AudioServerPlugInClientInfo*) {
    return device == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE RemoveDeviceClient(AudioServerPlugInDriverRef,
                                              AudioObjectID device,
                                              const AudioServerPlugInClientInfo*) {
    return device == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE PerformDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                            AudioObjectID device,
                                                            UInt64 action,
                                                            void*) {
    if (device != kDeviceID)
        return kAudioHardwareBadObjectError;
    if (action != 44100 && action != 48000)
        return kAudioHardwareIllegalOperationError;

    const auto rate = static_cast<std::uint32_t>(action);
    gSampleRate.store(rate, std::memory_order_release);

    if (gPlaybackRing) {
        const auto w =
            gPlaybackRing->writeFrame.load(std::memory_order_acquire);
        gPlaybackRing->readFrame.store(w, std::memory_order_release);
        gPlaybackRing->active.store(0, std::memory_order_release);
        // Publish the rate last: this is the supervisor's handoff trigger.
        gPlaybackRing->sampleRate.store(rate, std::memory_order_release);
    }

    if (gCaptureRing) {
        const auto w =
            gCaptureRing->writeFrame.load(std::memory_order_acquire);
        gCaptureRing->readFrame.store(w, std::memory_order_release);
        gCaptureRing->active.store(0, std::memory_order_release);
        // Keep the old capture rate until the new transport reinitializes the
        // ring. ReadInput zero-fills while it does not match gSampleRate.
    }

    Notify(kDeviceID, kAudioDevicePropertyNominalSampleRate);
    Notify(kOutputStreamID, kAudioStreamPropertyVirtualFormat);
    Notify(kOutputStreamID, kAudioStreamPropertyPhysicalFormat);
    Notify(kInputStreamID, kAudioStreamPropertyVirtualFormat);
    Notify(kInputStreamID, kAudioStreamPropertyPhysicalFormat);
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE AbortDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                          AudioObjectID,
                                                          UInt64,
                                                          void*) {
    return kAudioHardwareNoError;
}

Boolean STDMETHODCALLTYPE HasProperty(AudioServerPlugInDriverRef,
                                      AudioObjectID object,
                                      pid_t,
                                      const AudioObjectPropertyAddress* address) {
    if (!address || !IsKnownObject(object))
        return false;

    const auto selector = address->mSelector;
    if (selector == kAudioObjectPropertyBaseClass ||
        selector == kAudioObjectPropertyClass ||
        selector == kAudioObjectPropertyOwner ||
        selector == kAudioObjectPropertyOwnedObjects ||
        selector == kAudioObjectPropertyName ||
        selector == kAudioObjectPropertyManufacturer)
        return true;

    if (object == kAudioObjectPlugInObject) {
        return selector == kAudioPlugInPropertyDeviceList ||
               selector == kAudioPlugInPropertyTranslateUIDToDevice ||
               selector == kAudioPlugInPropertyResourceBundle;
    }

    if (object == kDeviceID) {
        switch (selector) {
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyIsHidden:
                return true;
            default:
                return false;
        }
    }

    if (object == kOutputStreamID || object == kInputStreamID) {
        switch (selector) {
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return true;
            default:
                return false;
        }
    }

    return false;
}

OSStatus STDMETHODCALLTYPE IsPropertySettable(AudioServerPlugInDriverRef driver,
                                              AudioObjectID object,
                                              pid_t pid,
                                              const AudioObjectPropertyAddress* address,
                                              Boolean* outSettable) {
    if (!address || !outSettable)
        return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address))
        return kAudioHardwareUnknownPropertyError;

    *outSettable =
        (object == kDeviceID &&
         address->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        ((object == kOutputStreamID || object == kInputStreamID) &&
         address->mSelector == kAudioStreamPropertyIsActive);
    return kAudioHardwareNoError;
}

UInt32 PropertySize(AudioObjectID object,
                    const AudioObjectPropertyAddress& address) {
    const auto selector = address.mSelector;

    if (selector == kAudioObjectPropertyBaseClass ||
        selector == kAudioObjectPropertyClass)
        return sizeof(AudioClassID);
    if (selector == kAudioObjectPropertyOwner)
        return sizeof(AudioObjectID);
    if (selector == kAudioObjectPropertyName ||
        selector == kAudioObjectPropertyManufacturer)
        return sizeof(CFStringRef);

    if (object == kAudioObjectPlugInObject) {
        if (selector == kAudioObjectPropertyOwnedObjects ||
            selector == kAudioPlugInPropertyDeviceList ||
            selector == kAudioPlugInPropertyTranslateUIDToDevice)
            return sizeof(AudioObjectID);
        if (selector == kAudioPlugInPropertyResourceBundle)
            return sizeof(CFStringRef);
    }

    if (object == kDeviceID) {
        switch (selector) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
                if (address.mScope == kAudioObjectPropertyScopeGlobal)
                    return 2 * sizeof(AudioObjectID);
                if (ScopeIsOutput(address.mScope) || ScopeIsInput(address.mScope))
                    return sizeof(AudioObjectID);
                return 0;
            case kAudioObjectPropertyControlList:
                return 0;
            case kAudioDevicePropertyRelatedDevices:
                return sizeof(AudioObjectID);
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                return sizeof(CFStringRef);
            case kAudioDevicePropertyNominalSampleRate:
                return sizeof(Float64);
            case kAudioDevicePropertyAvailableNominalSampleRates:
                return 2 * sizeof(AudioValueRange);
            default:
                return sizeof(UInt32);
        }
    }

    if (object == kOutputStreamID || object == kInputStreamID) {
        switch (selector) {
            case kAudioObjectPropertyOwnedObjects:
                return 0;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                return sizeof(AudioStreamBasicDescription);
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return 2 * sizeof(AudioStreamRangedDescription);
            default:
                return sizeof(UInt32);
        }
    }

    return 0;
}

OSStatus STDMETHODCALLTYPE GetPropertyDataSize(AudioServerPlugInDriverRef driver,
                                               AudioObjectID object,
                                               pid_t pid,
                                               const AudioObjectPropertyAddress* address,
                                               UInt32,
                                               const void*,
                                               UInt32* outSize) {
    if (!address || !outSize)
        return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address))
        return kAudioHardwareUnknownPropertyError;
    *outSize = PropertySize(object, *address);
    return kAudioHardwareNoError;
}

OSStatus GetCommon(AudioObjectID object,
                   const AudioObjectPropertyAddress& address,
                   UInt32 inSize,
                   UInt32* outSize,
                   void* outData) {
    if (address.mSelector == kAudioObjectPropertyBaseClass)
        return CopyScalar(inSize, outSize, outData,
                          static_cast<AudioClassID>(kAudioObjectClassID));

    if (address.mSelector == kAudioObjectPropertyClass) {
        const AudioClassID value =
            object == kAudioObjectPlugInObject ? kAudioPlugInClassID :
            object == kDeviceID ? kAudioDeviceClassID : kAudioStreamClassID;
        return CopyScalar(inSize, outSize, outData, value);
    }

    if (address.mSelector == kAudioObjectPropertyOwner) {
        const AudioObjectID value =
            object == kAudioObjectPlugInObject ? kAudioObjectUnknown :
            object == kDeviceID ? kAudioObjectPlugInObject : kDeviceID;
        return CopyScalar(inSize, outSize, outData, value);
    }

    if (address.mSelector == kAudioObjectPropertyName) {
        const CFStringRef value =
            object == kAudioObjectPlugInObject ? CFSTR("macfw FW1814 HAL") :
            object == kDeviceID ? CFSTR("M-Audio FireWire 1814") :
            object == kOutputStreamID ? CFSTR("Analog Outputs 1-4") :
                                        CFSTR("Analog Inputs 1-8");
        return CopyString(inSize, outSize, outData, value);
    }

    if (address.mSelector == kAudioObjectPropertyManufacturer)
        return CopyString(inSize, outSize, outData, CFSTR("macfw"));

    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE GetPropertyData(AudioServerPlugInDriverRef driver,
                                           AudioObjectID object,
                                           pid_t pid,
                                           const AudioObjectPropertyAddress* address,
                                           UInt32 qualifierSize,
                                           const void* qualifier,
                                           UInt32 inSize,
                                           UInt32* outSize,
                                           void* outData) {
    if (!address || !outSize)
        return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address))
        return kAudioHardwareUnknownPropertyError;

    const UInt32 needed = PropertySize(object, *address);
    if (needed && !outData)
        return kAudioHardwareIllegalOperationError;

    const OSStatus common = GetCommon(object, *address, inSize, outSize, outData);
    if (common != kAudioHardwareUnknownPropertyError)
        return common;

    const auto selector = address->mSelector;

    if (object == kAudioObjectPlugInObject) {
        if (selector == kAudioObjectPropertyOwnedObjects ||
            selector == kAudioPlugInPropertyDeviceList)
            return CopyScalar(inSize, outSize, outData, kDeviceID);

        if (selector == kAudioPlugInPropertyTranslateUIDToDevice) {
            if (qualifierSize != sizeof(CFStringRef) || !qualifier)
                return kAudioHardwareBadPropertySizeError;
            const CFStringRef uid =
                *reinterpret_cast<CFStringRef const*>(qualifier);
            const AudioObjectID id =
                uid && CFEqual(uid, CFSTR("com.mbprado.macfw.fw1814.device"))
                    ? kDeviceID : kAudioObjectUnknown;
            return CopyScalar(inSize, outSize, outData, id);
        }

        if (selector == kAudioPlugInPropertyResourceBundle)
            return CopyString(inSize, outSize, outData, CFSTR(""));
    }

    if (object == kDeviceID) {
        if (selector == kAudioObjectPropertyControlList) {
            *outSize = 0;
            return kAudioHardwareNoError;
        }

        switch (selector) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                if (address->mScope == kAudioObjectPropertyScopeGlobal) {
                    if (inSize < 2 * sizeof(AudioObjectID))
                        return kAudioHardwareBadPropertySizeError;
                    auto* ids = static_cast<AudioObjectID*>(outData);
                    ids[0] = kOutputStreamID;
                    ids[1] = kInputStreamID;
                    *outSize = 2 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                }
                if (ScopeIsOutput(address->mScope))
                    return CopyScalar(inSize, outSize, outData, kOutputStreamID);
                if (ScopeIsInput(address->mScope))
                    return CopyScalar(inSize, outSize, outData, kInputStreamID);
                *outSize = 0;
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyDeviceUID:
                return CopyString(inSize, outSize, outData,
                                  CFSTR("com.mbprado.macfw.fw1814.device"));
            case kAudioDevicePropertyModelUID:
                return CopyString(inSize, outSize, outData,
                                  CFSTR("com.mbprado.macfw.fw1814.model"));
            case kAudioDevicePropertyTransportType:
                return CopyScalar(inSize, outSize, outData,
                                  static_cast<UInt32>(kAudioDeviceTransportTypeFireWire));
            case kAudioDevicePropertyRelatedDevices:
                return CopyScalar(inSize, outSize, outData, kDeviceID);
            case kAudioDevicePropertyClockDomain:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1814));
            case kAudioDevicePropertyDeviceIsAlive:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioDevicePropertyDeviceIsRunning:
                return CopyScalar(inSize, outSize, outData,
                                  static_cast<UInt32>(gRunningClients.load() != 0));
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                return CopyScalar(inSize, outSize, outData,
                    static_cast<UInt32>(
                        address->mScope == kAudioObjectPropertyScopeInput ||
                        address->mScope == kAudioObjectPropertyScopeOutput));
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                return CopyScalar(inSize, outSize, outData,
                    static_cast<UInt32>(
                        address->mScope == kAudioObjectPropertyScopeOutput));
            case kAudioDevicePropertyIsHidden:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyZeroTimeStampPeriod:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(512));
            case kAudioDevicePropertyNominalSampleRate:
                return CopyScalar(
                    inSize, outSize, outData,
                    static_cast<Float64>(
                        gSampleRate.load(std::memory_order_acquire)));
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                if (inSize < 2 * sizeof(AudioValueRange))
                    return kAudioHardwareBadPropertySizeError;
                auto* rates = static_cast<AudioValueRange*>(outData);
                rates[0] = {kRate44100, kRate44100};
                rates[1] = {kRate48000, kRate48000};
                *outSize = 2 * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            default:
                break;
        }
    }

    if (object == kOutputStreamID || object == kInputStreamID) {
        const bool isInput = object == kInputStreamID;
        const Float64 rate = static_cast<Float64>(
            gSampleRate.load(std::memory_order_acquire));
        const auto format =
            isInput ? InputFormat(rate) : OutputFormat(rate);

        switch (selector) {
            case kAudioObjectPropertyOwnedObjects:
                *outSize = 0;
                return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyDirection:
                return CopyScalar(inSize, outSize, outData,
                                  static_cast<UInt32>(isInput ? 1 : 0));
            case kAudioStreamPropertyTerminalType:
                return CopyScalar(inSize, outSize, outData,
                                  static_cast<UInt32>(kAudioStreamTerminalTypeLine));
            case kAudioStreamPropertyStartingChannel:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyLatency:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                return CopyScalar(inSize, outSize, outData, format);
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                if (inSize < 2 * sizeof(AudioStreamRangedDescription))
                    return kAudioHardwareBadPropertySizeError;
                auto* formats =
                    static_cast<AudioStreamRangedDescription*>(outData);
                if (isInput) {
                    formats[0] = {
                        InputFormat(kRate44100), {kRate44100, kRate44100}};
                    formats[1] = {
                        InputFormat(kRate48000), {kRate48000, kRate48000}};
                } else {
                    formats[0] = {
                        OutputFormat(kRate44100), {kRate44100, kRate44100}};
                    formats[1] = {
                        OutputFormat(kRate48000), {kRate48000, kRate48000}};
                }
                *outSize = 2 * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default:
                break;
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE SetPropertyData(AudioServerPlugInDriverRef driver,
                                           AudioObjectID object,
                                           pid_t pid,
                                           const AudioObjectPropertyAddress* address,
                                           UInt32,
                                           const void*,
                                           UInt32 inSize,
                                           const void* inData) {
    if (!address || !inData)
        return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address))
        return kAudioHardwareUnknownPropertyError;

    if (object == kDeviceID &&
        address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64))
            return kAudioHardwareBadPropertySizeError;
        const Float64 rate = *static_cast<const Float64*>(inData);
        if (rate != kRate44100 && rate != kRate48000)
            return kAudioHardwareIllegalOperationError;
        if (static_cast<std::uint32_t>(rate) ==
            gSampleRate.load(std::memory_order_acquire))
            return kAudioHardwareNoError;
        if (!gHost)
            return kAudioHardwareIllegalOperationError;
        gHost->RequestDeviceConfigurationChange(
            gHost, kDeviceID, static_cast<UInt64>(rate), nullptr);
        return kAudioHardwareNoError;
    }

    if ((object == kOutputStreamID || object == kInputStreamID) &&
        address->mSelector == kAudioStreamPropertyIsActive) {
        if (inSize != sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE StartIO(AudioServerPlugInDriverRef,
                                   AudioObjectID device,
                                   UInt32) {
    if (device != kDeviceID)
        return kAudioHardwareBadObjectError;

    const UInt32 old = gRunningClients.fetch_add(1, std::memory_order_relaxed);
    if (old == 0) {
        gStartHostTime = mach_absolute_time();
        if (!MapPlaybackRing() || !MapCaptureRing()) {
            gRunningClients.fetch_sub(1, std::memory_order_relaxed);
            return kAudioHardwareUnspecifiedError;
        }

        gPlaybackRing->sampleRate.store(
            gSampleRate.load(std::memory_order_acquire),
            std::memory_order_release);
        gPlaybackRing->startIOCalls.fetch_add(1, std::memory_order_relaxed);
    }

    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE StopIO(AudioServerPlugInDriverRef,
                                  AudioObjectID device,
                                  UInt32) {
    if (device != kDeviceID)
        return kAudioHardwareBadObjectError;

    UInt32 old = gRunningClients.load(std::memory_order_relaxed);
    while (old > 0 &&
           !gRunningClients.compare_exchange_weak(old, old - 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {}

    if (old == 1 && gPlaybackRing) {
        gPlaybackRing->stopIOCalls.fetch_add(1, std::memory_order_relaxed);
    }

    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetZeroTimeStamp(AudioServerPlugInDriverRef,
                                            AudioObjectID device,
                                            UInt32,
                                            Float64* sampleTime,
                                            UInt64* hostTime,
                                            UInt64* seed) {
    if (device != kDeviceID || !sampleTime || !hostTime || !seed)
        return kAudioHardwareIllegalOperationError;

    const UInt64 now = mach_absolute_time();
    const long double rate = static_cast<long double>(
        gSampleRate.load(std::memory_order_acquire));
    const long double ns =
        static_cast<long double>(now - gStartHostTime) *
        gTimebase.numer / gTimebase.denom;
    const long double frames = ns * rate / 1000000000.0L;
    constexpr UInt64 period = 512;
    const UInt64 frame = static_cast<UInt64>(frames) / period * period;
    const long double frameNs =
        static_cast<long double>(frame) * 1000000000.0L / rate;

    *sampleTime = static_cast<Float64>(frame);
    *hostTime = gStartHostTime +
        static_cast<UInt64>(frameNs * gTimebase.denom / gTimebase.numer);
    *seed = 1;
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE WillDoIOOperation(AudioServerPlugInDriverRef,
                                             AudioObjectID device,
                                             UInt32,
                                             UInt32 operation,
                                             Boolean* willDo,
                                             Boolean* inPlace) {
    if (device != kDeviceID || !willDo || !inPlace)
        return kAudioHardwareIllegalOperationError;

    *willDo = operation == kAudioServerPlugInIOOperationWriteMix ||
              operation == kAudioServerPlugInIOOperationReadInput;
    *inPlace = true;
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE BeginIOOperation(AudioServerPlugInDriverRef,
                                            AudioObjectID,
                                            UInt32,
                                            UInt32,
                                            UInt32,
                                            const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE DoIOOperation(AudioServerPlugInDriverRef,
                                         AudioObjectID device,
                                         AudioObjectID stream,
                                         UInt32,
                                         UInt32 operation,
                                         UInt32 frames,
                                         const AudioServerPlugInIOCycleInfo*,
                                         void* mainBuffer,
                                         void*) {
    if (device != kDeviceID || !mainBuffer)
        return kAudioHardwareIllegalOperationError;

    if (stream == kOutputStreamID &&
        operation == kAudioServerPlugInIOOperationWriteMix) {
        if (!gPlaybackRing || !macfw::fw1814::hal::valid(*gPlaybackRing))
            return kAudioHardwareUnspecifiedError;

        gPlaybackRing->doIOCalls.fetch_add(1, std::memory_order_relaxed);
        gPlaybackRing->doIOFrames.fetch_add(frames, std::memory_order_relaxed);

        const std::uint32_t rate =
            gSampleRate.load(std::memory_order_acquire);
        if (gPlaybackRing->active.load(std::memory_order_acquire) == 0 ||
            gPlaybackRing->sampleRate.load(std::memory_order_acquire) != rate)
            return kAudioHardwareNoError;

        macfw::fw1814::hal::write(
            *gPlaybackRing, static_cast<const Float32*>(mainBuffer), frames);
        return kAudioHardwareNoError;
    }

    if (stream == kInputStreamID &&
        operation == kAudioServerPlugInIOOperationReadInput) {
        auto* out = static_cast<Float32*>(mainBuffer);

        if (!gCaptureRing ||
            !macfw::fw1814::hal::capture::valid(*gCaptureRing)) {
            std::memset(out, 0,
                        static_cast<std::size_t>(frames) *
                        kInputChannels * sizeof(Float32));
            return kAudioHardwareNoError;
        }

        // Announce input demand even before the producer has activated the
        // capture ring. The FW1814 transport uses this to prefill safely before
        // switching live CoreAudio reads on.
        gCaptureRing->halReadCalls.fetch_add(1, std::memory_order_relaxed);
        gCaptureRing->halRequestedFrames.fetch_add(frames,
                                                   std::memory_order_relaxed);

        std::size_t got = 0;
        if (gCaptureRing->active.load(std::memory_order_acquire) != 0 &&
            gCaptureRing->sampleRate.load(std::memory_order_acquire) ==
                gSampleRate.load(std::memory_order_acquire)) {
            got = macfw::fw1814::hal::capture::read(
                *gCaptureRing, out, frames);
            gCaptureRing->halFramesFromRing.fetch_add(got,
                                                      std::memory_order_relaxed);
        }

        if (got < frames) {
            std::memset(out + got * kInputChannels, 0,
                        (static_cast<std::size_t>(frames) - got) *
                        kInputChannels * sizeof(Float32));
            gCaptureRing->halZeroFilledFrames.fetch_add(
                frames - got, std::memory_order_relaxed);
        }

        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE EndIOOperation(AudioServerPlugInDriverRef,
                                          AudioObjectID,
                                          UInt32,
                                          UInt32,
                                          UInt32,
                                          const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

AudioServerPlugInDriverInterface gInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation
};

} // namespace

extern "C" void* FW1814HALFactory(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!typeUUID || !CFEqual(typeUUID, kAudioServerPlugInTypeUUID))
        return nullptr;
    return &gInterfacePtr;
}
