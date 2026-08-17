#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <atomic>

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

extern AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;

bool IsKnownObject(AudioObjectID id) {
    return id == kAudioObjectPlugInObject || id == kDeviceID || id == kOutputStreamID;
}

AudioStreamBasicDescription Format(Float64 rate) {
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = kBytesPerFrame;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = kBytesPerFrame;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel = 32;
    return asbd;
}

bool ScopeIsOutput(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput;
}

template <typename T>
OSStatus CopyScalar(UInt32 inDataSize, UInt32* outDataSize, void* outData, const T& value) {
    if (!outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    if (inDataSize < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    *reinterpret_cast<T*>(outData) = value;
    *outDataSize = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus CopyCFString(UInt32 inDataSize, UInt32* outDataSize, void* outData, CFStringRef value) {
    return CopyScalar(inDataSize, outDataSize, outData, value);
}

void Notify(AudioObjectID object, AudioObjectPropertySelector selector,
            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    if (!gHost) return;
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    gHost->PropertiesChanged(gHost, object, 1, &address);
}

HRESULT STDMETHODCALLTYPE QueryInterface(void*, REFIID uuid, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;
    *outInterface = nullptr;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);
    const bool supported = requested &&
        (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));
    if (requested) CFRelease(requested);
    if (!supported) return E_NOINTERFACE;
    gRefCount.fetch_add(1, std::memory_order_relaxed);
    *outInterface = &gInterfacePtr;
    return S_OK;
}

ULONG STDMETHODCALLTYPE AddRef(void*) {
    return gRefCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE Release(void*) {
    UInt32 old = gRefCount.load(std::memory_order_relaxed);
    while (old > 0 && !gRefCount.compare_exchange_weak(old, old - 1)) {}
    return old > 0 ? old - 1 : 0;
}

OSStatus STDMETHODCALLTYPE Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    gHost = host;
    mach_timebase_info(&gTimebase);
    gStartHostTime = mach_absolute_time();
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                                        const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID device,
                                           const AudioServerPlugInClientInfo*) {
    return device == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID device,
                                              const AudioServerPlugInClientInfo*) {
    return device == kDeviceID ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE PerformDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                            AudioObjectID device, UInt64 action, void*) {
    if (device != kDeviceID) return kAudioHardwareBadObjectError;
    if (action != 44100 && action != 48000) return kAudioHardwareIllegalOperationError;
    gSampleRate = static_cast<Float64>(action);
    Notify(kDeviceID, kAudioDevicePropertyNominalSampleRate);
    Notify(kOutputStreamID, kAudioStreamPropertyVirtualFormat);
    Notify(kOutputStreamID, kAudioStreamPropertyPhysicalFormat);
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE AbortDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                          AudioObjectID, UInt64, void*) {
    return kAudioHardwareNoError;
}

Boolean STDMETHODCALLTYPE HasProperty(AudioServerPlugInDriverRef, AudioObjectID object,
                                      pid_t, const AudioObjectPropertyAddress* address) {
    if (!address || !IsKnownObject(object)) return false;
    const auto s = address->mSelector;

    if (s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass ||
        s == kAudioObjectPropertyOwner || s == kAudioObjectPropertyOwnedObjects ||
        s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer) return true;

    if (object == kAudioObjectPlugInObject) {
        return s == kAudioPlugInPropertyDeviceList ||
               s == kAudioPlugInPropertyTranslateUIDToDevice ||
               s == kAudioPlugInPropertyResourceBundle;
    }

    if (object == kDeviceID) {
        switch (s) {
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

    if (object == kOutputStreamID) {
        switch (s) {
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

OSStatus STDMETHODCALLTYPE IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                              pid_t pid, const AudioObjectPropertyAddress* address,
                                              Boolean* outSettable) {
    if (!outSettable || !address) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address)) return kAudioHardwareUnknownPropertyError;
    *outSettable = (object == kDeviceID && address->mSelector == kAudioDevicePropertyNominalSampleRate) ||
                   (object == kOutputStreamID && address->mSelector == kAudioStreamPropertyIsActive);
    return kAudioHardwareNoError;
}

UInt32 PropertySize(AudioObjectID object, const AudioObjectPropertyAddress& a) {
    const auto s = a.mSelector;
    if (s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass) return sizeof(AudioClassID);
    if (s == kAudioObjectPropertyOwner) return sizeof(AudioObjectID);
    if (s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer) return sizeof(CFStringRef);

    if (object == kAudioObjectPlugInObject) {
        if (s == kAudioObjectPropertyOwnedObjects || s == kAudioPlugInPropertyDeviceList) return sizeof(AudioObjectID);
        if (s == kAudioPlugInPropertyTranslateUIDToDevice) return sizeof(AudioObjectID);
        if (s == kAudioPlugInPropertyResourceBundle) return sizeof(CFStringRef);
    }

    if (object == kDeviceID) {
        switch (s) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
                return ScopeIsOutput(a.mScope) ? sizeof(AudioObjectID) : 0;
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

    if (object == kOutputStreamID) {
        switch (s) {
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

OSStatus STDMETHODCALLTYPE GetPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                               pid_t pid, const AudioObjectPropertyAddress* address,
                                               UInt32, const void*, UInt32* outSize) {
    if (!address || !outSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address)) return kAudioHardwareUnknownPropertyError;
    *outSize = PropertySize(object, *address);
    return kAudioHardwareNoError;
}

OSStatus GetCommonObjectProperty(AudioObjectID object, const AudioObjectPropertyAddress& a,
                                 UInt32 inSize, UInt32* outSize, void* outData) {
    if (a.mSelector == kAudioObjectPropertyBaseClass) {
        return CopyScalar(inSize, outSize, outData, static_cast<AudioClassID>(kAudioObjectClassID));
    }
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
        return CopyCFString(inSize, outSize, outData, v);
    }
    if (a.mSelector == kAudioObjectPropertyManufacturer)
        return CopyCFString(inSize, outSize, outData, CFSTR("macfw"));
    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE GetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                           pid_t pid, const AudioObjectPropertyAddress* address,
                                           UInt32 qualifierSize, const void* qualifier,
                                           UInt32 inSize, UInt32* outSize, void* outData) {
    if (!address || !outSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address)) return kAudioHardwareUnknownPropertyError;

    const UInt32 needed = PropertySize(object, *address);
    if (needed != 0 && !outData) return kAudioHardwareIllegalOperationError;

    const auto common = GetCommonObjectProperty(object, *address, inSize, outSize, outData);
    if (common != kAudioHardwareUnknownPropertyError) return common;
    const auto s = address->mSelector;

    if (object == kAudioObjectPlugInObject) {
        if (s == kAudioObjectPropertyOwnedObjects || s == kAudioPlugInPropertyDeviceList)
            return CopyScalar(inSize, outSize, outData, kDeviceID);
        if (s == kAudioPlugInPropertyTranslateUIDToDevice) {
            if (qualifierSize != sizeof(CFStringRef) || !qualifier) return kAudioHardwareBadPropertySizeError;
            const CFStringRef uid = *reinterpret_cast<CFStringRef const*>(qualifier);
            const AudioObjectID id = (uid && CFEqual(uid, CFSTR("com.mbprado.macfw.fw410.device")))
                ? kDeviceID : kAudioObjectUnknown;
            return CopyScalar(inSize, outSize, outData, id);
        }
        if (s == kAudioPlugInPropertyResourceBundle)
            return CopyCFString(inSize, outSize, outData, CFSTR(""));
    }

    if (object == kDeviceID) {
        if (s == kAudioObjectPropertyControlList) {
            *outSize = 0;
            return kAudioHardwareNoError;
        }
        switch (s) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
                if (!ScopeIsOutput(address->mScope)) { *outSize = 0; return kAudioHardwareNoError; }
                return CopyScalar(inSize, outSize, outData, kOutputStreamID);
            case kAudioDevicePropertyDeviceUID:
                return CopyCFString(inSize, outSize, outData, CFSTR("com.mbprado.macfw.fw410.device"));
            case kAudioDevicePropertyModelUID:
                return CopyCFString(inSize, outSize, outData, CFSTR("com.mbprado.macfw.fw410.model"));
            case kAudioDevicePropertyTransportType:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(kAudioDeviceTransportTypeFireWire));
            case kAudioDevicePropertyRelatedDevices:
                return CopyScalar(inSize, outSize, outData, kDeviceID);
            case kAudioDevicePropertyClockDomain:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioDevicePropertyDeviceIsAlive:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioDevicePropertyDeviceIsRunning:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(gRunningClients.load() != 0));
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                return CopyScalar(inSize, outSize, outData,
                                  static_cast<UInt32>(address->mScope == kAudioObjectPropertyScopeOutput));
            case kAudioDevicePropertyIsHidden:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioDevicePropertyZeroTimeStampPeriod:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(512));
            case kAudioDevicePropertyNominalSampleRate:
                return CopyScalar(inSize, outSize, outData, gSampleRate);
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                if (inSize < 2 * sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                auto* ranges = reinterpret_cast<AudioValueRange*>(outData);
                ranges[0] = {kRate44100, kRate44100};
                ranges[1] = {kRate48000, kRate48000};
                *outSize = 2 * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            default:
                break;
        }
    }

    if (object == kOutputStreamID) {
        switch (s) {
            case kAudioObjectPropertyOwnedObjects:
                *outSize = 0;
                return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyDirection:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioStreamPropertyTerminalType:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(kAudioStreamTerminalTypeLine));
            case kAudioStreamPropertyStartingChannel:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(1));
            case kAudioStreamPropertyLatency:
                return CopyScalar(inSize, outSize, outData, static_cast<UInt32>(0));
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                const auto f = Format(gSampleRate);
                return CopyScalar(inSize, outSize, outData, f);
            }
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                if (inSize < 2 * sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                auto* formats = reinterpret_cast<AudioStreamRangedDescription*>(outData);
                formats[0] = {Format(kRate44100), {kRate44100, kRate44100}};
                formats[1] = {Format(kRate48000), {kRate48000, kRate48000}};
                *outSize = 2 * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default:
                break;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                           pid_t pid, const AudioObjectPropertyAddress* address,
                                           UInt32, const void*, UInt32 inSize, const void* inData) {
    if (!address || !inData) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(driver, object, pid, address)) return kAudioHardwareUnknownPropertyError;
    if (object == kDeviceID && address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        const Float64 rate = *reinterpret_cast<const Float64*>(inData);
        if (rate != kRate44100 && rate != kRate48000) return kAudioHardwareIllegalOperationError;
        if (rate != gSampleRate && gHost)
            gHost->RequestDeviceConfigurationChange(gHost, kDeviceID, static_cast<UInt64>(rate), nullptr);
        return kAudioHardwareNoError;
    }
    if (object == kOutputStreamID && address->mSelector == kAudioStreamPropertyIsActive)
        return inSize == sizeof(UInt32) ? kAudioHardwareNoError : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE StartIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32) {
    if (device != kDeviceID) return kAudioHardwareBadObjectError;
    if (gRunningClients.fetch_add(1) == 0) gStartHostTime = mach_absolute_time();
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE StopIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32) {
    if (device != kDeviceID) return kAudioHardwareBadObjectError;
    UInt32 old = gRunningClients.load();
    while (old > 0 && !gRunningClients.compare_exchange_weak(old, old - 1)) {}
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID device, UInt32,
                                            Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (device != kDeviceID || !outSampleTime || !outHostTime || !outSeed)
        return kAudioHardwareIllegalOperationError;
    const UInt64 now = mach_absolute_time();
    const long double nanos = static_cast<long double>(now - gStartHostTime) * gTimebase.numer / gTimebase.denom;
    const long double frames = nanos * gSampleRate / 1000000000.0L;
    const UInt64 period = 512;
    const UInt64 frame = static_cast<UInt64>(frames) / period * period;
    const long double periodNanos = static_cast<long double>(frame) * 1000000000.0L / gSampleRate;
    const UInt64 hostDelta = static_cast<UInt64>(periodNanos * gTimebase.denom / gTimebase.numer);
    *outSampleTime = static_cast<Float64>(frame);
    *outHostTime = gStartHostTime + hostDelta;
    *outSeed = 1;
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID device, UInt32,
                                             UInt32 operation, Boolean* outWillDo, Boolean* outInPlace) {
    if (device != kDeviceID || !outWillDo || !outInPlace) return kAudioHardwareIllegalOperationError;
    *outWillDo = operation == kAudioServerPlugInIOOperationWriteMix;
    *outInPlace = true;
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                            UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID device,
                                         AudioObjectID stream, UInt32, UInt32 operation, UInt32,
                                         const AudioServerPlugInIOCycleInfo*, void*, void*) {
    if (device != kDeviceID || stream != kOutputStreamID) return kAudioHardwareBadObjectError;
    return operation == kAudioServerPlugInIOOperationWriteMix ? kAudioHardwareNoError
                                                              : kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                          UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
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

extern "C" void* FW410HALFactory(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!typeUUID || !CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    return &gInterfacePtr;
}
