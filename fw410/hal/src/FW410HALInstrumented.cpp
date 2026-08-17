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
    return DoIOOperation(driver, device, stream, clientID, operationID, frames,
                         cycleInfo, mainBuffer, secondaryBuffer);
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
