#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include "../include/macfw_hal_shm.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr AudioObjectID kDeviceID = 2;
constexpr AudioObjectID kOutputStreamID = 3;
constexpr Float64 kRate44100 = 44100.0;
constexpr Float64 kRate48000 = 48000.0;
constexpr UInt32 kChannels = 2;
constexpr UInt32 kBytesPerFrame = sizeof(Float32) * kChannels;

AudioServerPlugInHostRef gHost = nullptr;
std::atomic<UInt32> gRefCount{1};
std::atomic<UInt32> gRunningClients{0};
Float64 gSampleRate = kRate44100;
UInt64 gStartHostTime = 0;
mach_timebase_info_data_t gTimebase{};
int gShmFd = -1;
macfw::hal::SharedPcmRing* gRing = nullptr;

extern AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;

bool IsKnownObject(AudioObjectID id) {
    return id == kAudioObjectPlugInObject || id == kDeviceID || id == kOutputStreamID;
}

bool MapSharedRing() {
    if (gRing) return true;
    gShmFd = shm_open(macfw::hal::kShmName, O_CREAT | O_RDWR, 0666);
    if (gShmFd < 0) return false;
    if (ftruncate(gShmFd, sizeof(macfw::hal::SharedPcmRing)) != 0) {
        close(gShmFd); gShmFd = -1; return false;
    }
    void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing), PROT_READ | PROT_WRITE,
                   MAP_SHARED, gShmFd, 0);
    if (p == MAP_FAILED) {
        close(gShmFd); gShmFd = -1; return false;
    }
    gRing = static_cast<macfw::hal::SharedPcmRing*>(p);
    macfw::hal::initialize(*gRing, static_cast<std::uint32_t>(gSampleRate));
    return true;
}

AudioStreamBasicDescription Format(Float64 rate) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kBytesPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kBytesPerFrame;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

bool ScopeIsOutput(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput;
}

template <typename T>
OSStatus CopyScalar(UInt32 inSize, UInt32* outSize, void* outData, const T& value) {
    if (!outSize || !outData) return kAudioHardwareIllegalOperationError;
    if (inSize < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    *reinterpret_cast<T*>(outData) = value;
    *outSize = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus CopyString(UInt32 inSize, UInt32* outSize, void* outData, CFStringRef value) {
    return CopyScalar(inSize, outSize, outData, value);
}

void Notify(AudioObjectID object, AudioObjectPropertySelector selector,
            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    if (!gHost) return;
    AudioObjectPropertyAddress a{selector, scope, kAudioObjectPropertyElementMain};
    gHost->PropertiesChanged(gHost, object, 1, &a);
}

HRESULT STDMETHODCALLTYPE QueryInterface(void*, REFIID uuid, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;
    *outInterface = nullptr;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);
    const bool ok = requested && (CFEqual(requested, IUnknownUUID) ||
                                  CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));
    if (requested) CFRelease(requested);
    if (!ok) return E_NOINTERFACE;
    gRefCount.fetch_add(1, std::memory_order_relaxed);
    *outInterface = &gInterfacePtr;
    return S_OK;
}

ULONG STDMETHODCALLTYPE AddRef(void*) { return gRefCount.fetch_add(1) + 1; }
ULONG STDMETHODCALLTYPE Release(void*) {
    UInt32 old = gRefCount.load();
    while (old > 0 && !gRefCount.compare_exchange_weak(old, old - 1)) {}
    return old ? old - 1 : 0;
}

OSStatus STDMETHODCALLTYPE Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    gHost = host;
    mach_timebase_info(&gTimebase);
    gStartHostTime = mach_absolute_time();
    MapSharedRing();
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                                        const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus STDMETHODCALLTYPE DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus STDMETHODCALLTYPE AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID d,
                                           const AudioServerPlugInClientInfo*) {
    return d == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
OSStatus STDMETHODCALLTYPE RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID d,
                                              const AudioServerPlugInClientInfo*) {
    return d == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE PerformDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                            AudioObjectID d, UInt64 action, void*) {
    if (d != kDeviceID) return kAudioHardwareBadObjectError;
    if (action != 44100 && action != 48000) return kAudioHardwareIllegalOperationError;
    gSampleRate = static_cast<Float64>(action);
    if (gRing) gRing->sampleRate.store(static_cast<std::uint32_t>(action), std::memory_order_release);
    Notify(kDeviceID, kAudioDevicePropertyNominalSampleRate);
    Notify(kOutputStreamID, kAudioStreamPropertyVirtualFormat);
    Notify(kOutputStreamID, kAudioStreamPropertyPhysicalFormat);
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE AbortDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                          AudioObjectID, UInt64, void*) {
    return kAudioHardwareNoError;
}

Boolean STDMETHODCALLTYPE HasProperty(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
                                      const AudioObjectPropertyAddress* a) {
    if (!a || !IsKnownObject(object)) return false;
    const auto s = a->mSelector;
    if (s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass ||
        s == kAudioObjectPropertyOwner || s == kAudioObjectPropertyOwnedObjects ||
        s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer) return true;
    if (object == kAudioObjectPlugInObject)
        return s == kAudioPlugInPropertyDeviceList || s == kAudioPlugInPropertyTranslateUIDToDevice ||
               s == kAudioPlugInPropertyResourceBundle;
    if (object == kDeviceID) {
        switch (s) {
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList:
            case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyIsHidden: return true;
            default: return false;
        }
    }
    if (object == kOutputStreamID) {
        switch (s) {
            case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency: case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: return true;
            default: return false;
        }
    }
    return false;
}

OSStatus STDMETHODCALLTYPE IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                              pid_t pid, const AudioObjectPropertyAddress* a,
                                              Boolean* outSettable) {
    if (!a || !outSettable) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, a)) return kAudioHardwareUnknownPropertyError;
    *outSettable = (object == kDeviceID && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
                   (object == kOutputStreamID && a->mSelector == kAudioStreamPropertyIsActive);
    return kAudioHardwareNoError;
}

UInt32 PropertySize(AudioObjectID object, const AudioObjectPropertyAddress& a) {
    const auto s = a.mSelector;
    if (s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass) return sizeof(AudioClassID);
    if (s == kAudioObjectPropertyOwner) return sizeof(AudioObjectID);
    if (s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer) return sizeof(CFStringRef);
    if (object == kAudioObjectPlugInObject) {
        if (s == kAudioObjectPropertyOwnedObjects || s == kAudioPlugInPropertyDeviceList ||
            s == kAudioPlugInPropertyTranslateUIDToDevice) return sizeof(AudioObjectID);
        if (s == kAudioPlugInPropertyResourceBundle) return sizeof(CFStringRef);
    }
    if (object == kDeviceID) {
        switch (s) {
            case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams:
                return ScopeIsOutput(a.mScope) ? sizeof(AudioObjectID) : 0;
            case kAudioObjectPropertyControlList: return 0;
            case kAudioDevicePropertyRelatedDevices: return sizeof(AudioObjectID);
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: return sizeof(CFStringRef);
            case kAudioDevicePropertyNominalSampleRate: return sizeof(Float64);
            case kAudioDevicePropertyAvailableNominalSampleRates: return 2 * sizeof(AudioValueRange);
            default: return sizeof(UInt32);
        }
    }
    if (object == kOutputStreamID) {
        switch (s) {
            case kAudioObjectPropertyOwnedObjects: return 0;
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
                return sizeof(AudioStreamBasicDescription);
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
                return 2 * sizeof(AudioStreamRangedDescription);
            default: return sizeof(UInt32);
        }
    }
    return 0;
}

OSStatus STDMETHODCALLTYPE GetPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                               pid_t pid, const AudioObjectPropertyAddress* a,
                                               UInt32, const void*, UInt32* outSize) {
    if (!a || !outSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, a)) return kAudioHardwareUnknownPropertyError;
    *outSize = PropertySize(object, *a);
    return kAudioHardwareNoError;
}

OSStatus GetCommon(AudioObjectID object, const AudioObjectPropertyAddress& a,
                   UInt32 inSize, UInt32* outSize, void* outData) {
    if (a.mSelector == kAudioObjectPropertyBaseClass)
        return CopyScalar(inSize, outSize, outData, static_cast<AudioClassID>(kAudioObjectClassID));
    if (a.mSelector == kAudioObjectPropertyClass) {
        const AudioClassID v = object == kAudioObjectPlugInObject ? kAudioPlugInClassID :
                               object == kDeviceID ? kAudioDeviceClassID : kAudioStreamClassID;
        return CopyScalar(inSize, outSize, outData, v);
    }
    if (a.mSelector == kAudioObjectPropertyOwner) {
        const AudioObjectID v = object == kAudioObjectPlugInObject ? kAudioObjectUnknown :
                                object == kDeviceID ? kAudioObjectPlugInObject : kDeviceID;
        return CopyScalar(inSize, outSize, outData, v);
    }
    if (a.mSelector == kAudioObjectPropertyName) {
        const CFStringRef v = object == kAudioObjectPlugInObject ? CFSTR("macfw FW410 HAL") :
                              object == kDeviceID ? CFSTR("M-Audio FireWire 410") : CFSTR("Output 1/2");
        return CopyString(inSize, outSize, outData, v);
    }
    if (a.mSelector == kAudioObjectPropertyManufacturer)
        return CopyString(inSize, outSize, outData, CFSTR("macfw"));
    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE GetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                           pid_t pid, const AudioObjectPropertyAddress* a,
                                           UInt32 qualifierSize, const void* qualifier,
                                           UInt32 inSize, UInt32* outSize, void* outData) {
    if (!a || !outSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, a)) return kAudioHardwareUnknownPropertyError;
    const UInt32 needed = PropertySize(object, *a);
    if (needed && !outData) return kAudioHardwareIllegalOperationError;
    const OSStatus common = GetCommon(object, *a, inSize, outSize, outData);
    if (common != kAudioHardwareUnknownPropertyError) return common;
    const auto s = a->mSelector;
    if (object == kAudioObjectPlugInObject) {
        if (s == kAudioObjectPropertyOwnedObjects || s == kAudioPlugInPropertyDeviceList)
            return CopyScalar(inSize, outSize, outData, kDeviceID);
        if (s == kAudioPlugInPropertyTranslateUIDToDevice) {
            if (qualifierSize != sizeof(CFStringRef) || !qualifier) return kAudioHardwareBadPropertySizeError;
            const CFStringRef uid = *reinterpret_cast<CFStringRef const*>(qualifier);
            const AudioObjectID id = uid && CFEqual(uid, CFSTR("com.mbprado.macfw.fw410.device")) ?
                                     kDeviceID : kAudioObjectUnknown;
            return CopyScalar(inSize, outSize, outData, id);
        }
        if (s == kAudioPlugInPropertyResourceBundle) return CopyString(inSize, outSize, outData, CFSTR(""));
    }
    if (object == kDeviceID) {
        if (s == kAudioObjectPropertyControlList) { *outSize = 0; return kAudioHardwareNoError; }
        switch (s) {
            case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams:
                if (!ScopeIsOutput(a->mScope)) { *outSize = 0; return kAudioHardwareNoError; }
                return CopyScalar(inSize, outSize, outData, kOutputStreamID);
            case kAudioDevicePropertyDeviceUID: return CopyString(inSize, outSize, outData, CFSTR("com.mbprado.macfw.fw410.device"));
            case kAudioDevicePropertyModelUID: return CopyString(inSize, outSize, outData, CFSTR("com.mbprado.macfw.fw410.model"));
            case kAudioDevicePropertyTransportType: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(kAudioDeviceTransportTypeFireWire));
            case kAudioDevicePropertyRelatedDevices: return CopyScalar(inSize, outSize, outData, kDeviceID);
            case kAudioDevicePropertyClockDomain: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioDevicePropertyDeviceIsAlive: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioDevicePropertyDeviceIsRunning: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(gRunningClients.load() != 0));
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(a->mScope == kAudioObjectPropertyScopeOutput));
            case kAudioDevicePropertyIsHidden: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyZeroTimeStampPeriod: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(512));
            case kAudioDevicePropertyNominalSampleRate: return CopyScalar(inSize, outSize, outData, gSampleRate);
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                if (inSize < 2 * sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                auto* v = static_cast<AudioValueRange*>(outData);
                v[0] = {kRate44100, kRate44100}; v[1] = {kRate48000, kRate48000};
                *outSize = 2 * sizeof(AudioValueRange); return kAudioHardwareNoError;
            }
            default: break;
        }
    }
    if (object == kOutputStreamID) {
        switch (s) {
            case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyDirection: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioStreamPropertyTerminalType: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(kAudioStreamTerminalTypeLine));
            case kAudioStreamPropertyStartingChannel: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyLatency: return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: {
                const auto f = Format(gSampleRate); return CopyScalar(inSize, outSize, outData, f);
            }
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
                if (inSize < 2 * sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                auto* v = static_cast<AudioStreamRangedDescription*>(outData);
                v[0] = {Format(kRate44100), {kRate44100, kRate44100}};
                v[1] = {Format(kRate48000), {kRate48000, kRate48000}};
                *outSize = 2 * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            }
            default: break;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                           pid_t pid, const AudioObjectPropertyAddress* a,
                                           UInt32, const void*, UInt32 inSize, const void* inData) {
    if (!a || !inData) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, a)) return kAudioHardwareUnknownPropertyError;
    if (object == kDeviceID && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        const Float64 rate = *static_cast<const Float64*>(inData);
        if (rate != kRate44100 && rate != kRate48000) return kAudioHardwareIllegalOperationError;
        if (rate != gSampleRate && gHost)
            gHost->RequestDeviceConfigurationChange(gHost, kDeviceID, static_cast<UInt64>(rate), nullptr);
        return kAudioHardwareNoError;
    }
    if (object == kOutputStreamID && a->mSelector == kAudioStreamPropertyIsActive)
        return inSize == sizeof(UInt32) ? kAudioHardwareNoError : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE StartIO(AudioServerPlugInDriverRef, AudioObjectID d, UInt32) {
    if (d != kDeviceID) return kAudioHardwareBadObjectError;
    if (gRunningClients.fetch_add(1) == 0) {
        gStartHostTime = mach_absolute_time();
        if (gRing) gRing->active.store(1, std::memory_order_release);
    }
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE StopIO(AudioServerPlugInDriverRef, AudioObjectID d, UInt32) {
    if (d != kDeviceID) return kAudioHardwareBadObjectError;
    UInt32 old = gRunningClients.load();
    while (old > 0 && !gRunningClients.compare_exchange_weak(old, old - 1)) {}
    if (old == 1 && gRing) gRing->active.store(0, std::memory_order_release);
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID d, UInt32,
                                            Float64* sample, UInt64* host, UInt64* seed) {
    if (d != kDeviceID || !sample || !host || !seed) return kAudioHardwareIllegalOperationError;
    const UInt64 now = mach_absolute_time();
    const long double ns = static_cast<long double>(now - gStartHostTime) * gTimebase.numer / gTimebase.denom;
    const long double frames = ns * gSampleRate / 1000000000.0L;
    const UInt64 period = 512;
    const UInt64 frame = static_cast<UInt64>(frames) / period * period;
    const long double frameNs = static_cast<long double>(frame) * 1000000000.0L / gSampleRate;
    *sample = static_cast<Float64>(frame);
    *host = gStartHostTime + static_cast<UInt64>(frameNs * gTimebase.denom / gTimebase.numer);
    *seed = 1;
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID d, UInt32,
                                             UInt32 op, Boolean* willDo, Boolean* inPlace) {
    if (d != kDeviceID || !willDo || !inPlace) return kAudioHardwareIllegalOperationError;
    *willDo = op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                            UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID d, AudioObjectID stream,
                                         UInt32, UInt32 op, UInt32 frames,
                                         const AudioServerPlugInIOCycleInfo*, void* mainBuffer, void*) {
    if (d != kDeviceID || stream != kOutputStreamID) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix) return kAudioHardwareUnsupportedOperationError;
    if (gRing && mainBuffer && macfw::hal::valid(*gRing))
        macfw::hal::write(*gRing, static_cast<const float*>(mainBuffer), frames);
    return kAudioHardwareNoError;
}
OSStatus STDMETHODCALLTYPE EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                          UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

AudioServerPlugInDriverInterface gInterface = {
    nullptr, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice,
    AddDeviceClient, RemoveDeviceClient, PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation,
    BeginIOOperation, DoIOOperation, EndIOOperation
};

} // namespace

extern "C" void* FW410HALFactory(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!typeUUID || !CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    return &gInterfacePtr;
}
