// Diagnostic wrapper around the known-good HAL bridge implementation.
//
// Keep FW410HALBridge.cpp as the enumeration/playback baseline and intercept
// only the COM/I/O lifecycle entry points here. This lets shmprobe observe
// what Monterey actually calls without logging or allocating on the audio
// thread.

#define FW410HALFactory FW410HALFactory_Base
#include "FW410HALBridge.cpp"
#undef FW410HALFactory

namespace {

extern AudioServerPlugInDriverInterface gInstrumentedInterface;
AudioServerPlugInDriverInterface* gInstrumentedInterfacePtr = &gInstrumentedInterface;

bool TransportOnlineForAudio() {
    // Real-time safe observation only: never shm_open/mmap from the I/O thread.
    // If the status ABI was unavailable when the HAL initialized/started, keep
    // the legacy fail-open behavior for this checkpoint rather than silently
    // breaking an otherwise working audio path. A later lifecycle checkpoint
    // will add non-RT remapping/retry so a missing publisher can be treated as
    // explicitly offline.
    if (!gTransportStatus || !macfw::hal::transport::valid(*gTransportStatus)) return true;
    const auto raw = gTransportStatus->state.load(std::memory_order_acquire);
    return raw == static_cast<std::uint32_t>(macfw::hal::transport::State::Online);
}

HRESULT STDMETHODCALLTYPE InstrumentedQueryInterface(void*, REFIID uuid, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;
    *outInterface = nullptr;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);
    const bool supported = requested &&
        (CFEqual(requested, IUnknownUUID) ||
         CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));
    if (requested) CFRelease(requested);
    if (!supported) return E_NOINTERFACE;
    gRefCount.fetch_add(1, std::memory_order_relaxed);
    *outInterface = &gInstrumentedInterfacePtr;
    return S_OK;
}

OSStatus STDMETHODCALLTYPE InstrumentedStartIO(AudioServerPlugInDriverRef driver,
                                               AudioObjectID device,
                                               UInt32 clientID) {
    if (gRing) gRing->startIOCalls.fetch_add(1, std::memory_order_relaxed);
    return StartIO(driver, device, clientID);
}

OSStatus STDMETHODCALLTYPE InstrumentedStopIO(AudioServerPlugInDriverRef driver,
                                              AudioObjectID device,
                                              UInt32 clientID) {
    if (gRing) gRing->stopIOCalls.fetch_add(1, std::memory_order_relaxed);
    return StopIO(driver, device, clientID);
}

OSStatus STDMETHODCALLTYPE InstrumentedWillDoIOOperation(AudioServerPlugInDriverRef driver,
                                                         AudioObjectID device,
                                                         UInt32 clientID,
                                                         UInt32 operationID,
                                                         Boolean* willDo,
                                                         Boolean* inPlace) {
    if (gRing) {
        gRing->willDoCalls.fetch_add(1, std::memory_order_relaxed);
        gRing->lastWillDoOperation.store(operationID, std::memory_order_relaxed);
    }
    return WillDoIOOperation(driver, device, clientID, operationID, willDo, inPlace);
}

OSStatus STDMETHODCALLTYPE InstrumentedBeginIOOperation(AudioServerPlugInDriverRef driver,
                                                        AudioObjectID device,
                                                        UInt32 clientID,
                                                        UInt32 operationID,
                                                        UInt32 frames,
                                                        const AudioServerPlugInIOCycleInfo* cycleInfo) {
    if (gRing) {
        gRing->beginIOCalls.fetch_add(1, std::memory_order_relaxed);
        gRing->lastBeginOperation.store(operationID, std::memory_order_relaxed);
    }
    return BeginIOOperation(driver, device, clientID, operationID, frames, cycleInfo);
}

OSStatus STDMETHODCALLTYPE InstrumentedDoIOOperation(AudioServerPlugInDriverRef driver,
                                                     AudioObjectID device,
                                                     AudioObjectID stream,
                                                     UInt32 clientID,
                                                     UInt32 operationID,
                                                     UInt32 frames,
                                                     const AudioServerPlugInIOCycleInfo* cycleInfo,
                                                     void* mainBuffer,
                                                     void* secondaryBuffer) {
    if (gRing) {
        gRing->doIOCalls.fetch_add(1, std::memory_order_relaxed);
        gRing->doIOFrames.fetch_add(frames, std::memory_order_relaxed);
        gRing->lastDoOperation.store(operationID, std::memory_order_relaxed);
        gRing->lastDoFrames.store(frames, std::memory_order_relaxed);
        if (mainBuffer)
            gRing->nonNullMainBufferCalls.fetch_add(1, std::memory_order_relaxed);
        if (operationID == kAudioServerPlugInIOOperationWriteMix)
            gRing->writeMixCalls.fetch_add(1, std::memory_order_relaxed);
    }

    const bool captureCall =
        stream == kInputStreamID && operationID == kAudioServerPlugInIOOperationReadInput;
    const bool playbackCall =
        stream == kOutputStreamID && operationID == kAudioServerPlugInIOOperationWriteMix;
    const bool transportOnline = TransportOnlineForAudio();

    // Keep the logical CoreAudio endpoint alive while physical transport is
    // recovering. Do not enqueue playback that cannot reach hardware, and
    // return deterministic silence for capture until the native engine signals
    // READY and the supervisor publishes ONLINE again.
    if (!transportOnline && playbackCall) return kAudioHardwareNoError;
    if (!transportOnline && captureCall) {
        if (!mainBuffer) return kAudioHardwareIllegalOperationError;
        std::memset(mainBuffer, 0,
                    static_cast<std::size_t>(frames) * kInputChannels * sizeof(Float32));
        if (gCaptureRing && macfw::hal::capture::valid(*gCaptureRing)) {
            gCaptureRing->halReadCalls.fetch_add(1, std::memory_order_relaxed);
            gCaptureRing->halRequestedFrames.fetch_add(frames, std::memory_order_relaxed);
            gCaptureRing->halZeroFilledFrames.fetch_add(frames, std::memory_order_relaxed);
        }
        return kAudioHardwareNoError;
    }

    std::uint64_t captureReadBefore = 0;
    if (captureCall && gCaptureRing && macfw::hal::capture::valid(*gCaptureRing)) {
        gCaptureRing->halReadCalls.fetch_add(1, std::memory_order_relaxed);
        gCaptureRing->halRequestedFrames.fetch_add(frames, std::memory_order_relaxed);
        captureReadBefore = gCaptureRing->readFrame.load(std::memory_order_acquire);
    }

    const OSStatus status = DoIOOperation(driver, device, stream, clientID, operationID, frames,
                                          cycleInfo, mainBuffer, secondaryBuffer);

    if (captureCall && gCaptureRing && macfw::hal::capture::valid(*gCaptureRing)) {
        const std::uint64_t captureReadAfter =
            gCaptureRing->readFrame.load(std::memory_order_acquire);
        const std::uint64_t consumed = captureReadAfter >= captureReadBefore
            ? captureReadAfter - captureReadBefore : 0;
        const std::uint64_t bounded = consumed > frames ? frames : consumed;
        gCaptureRing->halFramesFromRing.fetch_add(bounded, std::memory_order_relaxed);
        gCaptureRing->halZeroFilledFrames.fetch_add(frames - bounded, std::memory_order_relaxed);
    }

    return status;
}

OSStatus STDMETHODCALLTYPE InstrumentedEndIOOperation(AudioServerPlugInDriverRef driver,
                                                      AudioObjectID device,
                                                      UInt32 clientID,
                                                      UInt32 operationID,
                                                      UInt32 frames,
                                                      const AudioServerPlugInIOCycleInfo* cycleInfo) {
    if (gRing) {
        gRing->endIOCalls.fetch_add(1, std::memory_order_relaxed);
        gRing->lastEndOperation.store(operationID, std::memory_order_relaxed);
    }
    return EndIOOperation(driver, device, clientID, operationID, frames, cycleInfo);
}

AudioServerPlugInDriverInterface gInstrumentedInterface = {
    nullptr,
    InstrumentedQueryInterface,
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
    InstrumentedStartIO,
    InstrumentedStopIO,
    GetZeroTimeStamp,
    InstrumentedWillDoIOOperation,
    InstrumentedBeginIOOperation,
    InstrumentedDoIOOperation,
    InstrumentedEndIOOperation
};

} // namespace

extern "C" void* FW410HALFactory(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!typeUUID || !CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    return &gInstrumentedInterfacePtr;
}
