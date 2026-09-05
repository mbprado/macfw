#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <objc/runtime.h>

#include <dispatch/dispatch.h>

static NSString *const kMacfwDeviceStatusTool = @"/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus";
static CFStringRef const kMacfwDeviceUID = CFSTR("com.mbprado.macfw.fw410.device");

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)refresh:(id)sender;
@end

static const void *kMacfwDeviceStateKey = &kMacfwDeviceStateKey;
static const void *kMacfwDeviceActiveRateKey = &kMacfwDeviceActiveRateKey;
static const void *kMacfwDeviceRequestedRateKey = &kMacfwDeviceRequestedRateKey;
static const void *kMacfwDevicePidKey = &kMacfwDevicePidKey;
static const void *kMacfwDeviceRateControlKey = &kMacfwDeviceRateControlKey;
static const void *kMacfwDeviceBufferKey = &kMacfwDeviceBufferKey;
static const void *kMacfwDeviceBufferControlKey = &kMacfwDeviceBufferControlKey;
static const void *kMacfwDeviceOutputLatencyKey = &kMacfwDeviceOutputLatencyKey;
static const void *kMacfwDeviceInputLatencyKey = &kMacfwDeviceInputLatencyKey;
static const void *kMacfwDeviceSafetyKey = &kMacfwDeviceSafetyKey;

static NSString *MacfwDeviceRunStatus(int *statusOut) {
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:kMacfwDeviceStatusTool]) {
        if (statusOut) *statusOut = 127;
        return @"";
    }
    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:kMacfwDeviceStatusTool];
    task.standardOutput = pipe;
    task.standardError = pipe;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        if (statusOut) *statusOut = 126;
        return @"";
    }
    [task waitUntilExit];
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    if (statusOut) *statusOut = task.terminationStatus;
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return text != nil ? text : @"";
}

static NSString *MacfwDeviceValue(NSString *prefix, NSString *text) {
    for (NSString *line in [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *trim = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (![trim hasPrefix:prefix]) continue;
        return [[trim substringFromIndex:prefix.length]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    }
    return nil;
}

static NSTextField *MacfwDeviceValueLabel(NSView *view, NSString *title, CGFloat y) {
    NSTextField *name = [NSTextField labelWithString:title];
    name.textColor = NSColor.secondaryLabelColor;
    name.frame = NSMakeRect(28, y, 160, 20);
    [view addSubview:name];

    NSTextField *value = [NSTextField labelWithString:@"—"];
    value.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
    value.frame = NSMakeRect(200, y, 355, 20);
    [view addSubview:value];
    return value;
}

static AudioObjectID MacfwFindCoreAudioDevice(void) {
    AudioObjectPropertyAddress address{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioObjectID)) return kAudioObjectUnknown;

    const UInt32 count = size / sizeof(AudioObjectID);
    NSMutableData *storage = [NSMutableData dataWithLength:size];
    auto *devices = static_cast<AudioObjectID *>(storage.mutableBytes);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices) != noErr)
        return kAudioObjectUnknown;

    AudioObjectPropertyAddress uidAddress{
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    for (UInt32 i = 0; i < count; ++i) {
        CFStringRef uid = nullptr;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddress, 0, nullptr, &uidSize, &uid) != noErr || !uid)
            continue;
        const bool match = CFEqual(uid, kMacfwDeviceUID);
        CFRelease(uid);
        if (match) return devices[i];
    }
    return kAudioObjectUnknown;
}

static BOOL MacfwReadUInt32(AudioObjectID device, AudioObjectPropertySelector selector,
                            AudioObjectPropertyScope scope, UInt32 *valueOut) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(device, &address)) return NO;
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || size != sizeof(value))
        return NO;
    if (valueOut) *valueOut = value;
    return YES;
}

static BOOL MacfwReadBufferRange(AudioObjectID device, AudioValueRange *rangeOut) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (!AudioObjectHasProperty(device, &address)) return NO;
    AudioValueRange range{};
    UInt32 size = sizeof(range);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &range) != noErr || size != sizeof(range))
        return NO;
    if (rangeOut) *rangeOut = range;
    return YES;
}

static BOOL MacfwPropertySettable(AudioObjectID device, AudioObjectPropertySelector selector,
                                  AudioObjectPropertyScope scope, BOOL *settableOut) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(device, &address)) return NO;
    Boolean settable = false;
    if (AudioObjectIsPropertySettable(device, &address, &settable) != noErr) return NO;
    if (settableOut) *settableOut = settable ? YES : NO;
    return YES;
}

static OSStatus MacfwSetNominalSampleRate(AudioObjectID device, Float64 rate) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (!AudioObjectHasProperty(device, &address)) return kAudioHardwareUnknownPropertyError;

    Boolean settable = false;
    OSStatus status = AudioObjectIsPropertySettable(device, &address, &settable);
    if (status != noErr) return status;
    if (!settable) return kAudioHardwareUnsupportedOperationError;

    return AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(rate), &rate);
}

static NSString *MacfwFramesAndMs(UInt32 frames, double sampleRate) {
    if (!(sampleRate > 0.0)) return [NSString stringWithFormat:@"%u frames", frames];
    return [NSString stringWithFormat:@"%u frames (%.2f ms)", frames,
            (1000.0 * static_cast<double>(frames)) / sampleRate];
}

@implementation AppDelegate (MacfwDevice)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;

    Method original = class_getInstanceMethod(cls, @selector(applicationDidFinishLaunching:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfwDevice_applicationDidFinishLaunching:));
    method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(refresh:));
    replacement = class_getInstanceMethod(cls, @selector(macfwDevice_refresh:));
    method_exchangeImplementations(original, replacement);
}

- (void)macfwDevice_applicationDidFinishLaunching:(NSNotification *)notification {
    [self macfwDevice_applicationDidFinishLaunching:notification];
    [self macfwBuildDeviceTab];
    [self macfwRefreshDevice];
}

- (void)macfwDevice_refresh:(id)sender {
    [self macfwDevice_refresh:sender];
    [self macfwRefreshDevice];
}

- (NSTabView *)macfwDeviceTabView {
    @try { return [self valueForKey:@"tabView"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (void)macfwBuildDeviceTab {
    NSTabView *tabs = [self macfwDeviceTabView];
    if (!tabs) return;

    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"device"];
    item.label = @"Device";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;

    NSUInteger infoIndex = tabs.numberOfTabViewItems;
    for (NSUInteger i = 0; i < tabs.numberOfTabViewItems; ++i) {
        if ([[tabs tabViewItemAtIndex:i].identifier isEqual:@"info"]) {
            infoIndex = i;
            break;
        }
    }
    [tabs insertTabViewItem:item atIndex:infoIndex];

    NSTextField *title = [NSTextField labelWithString:@"Transport / CoreAudio State"];
    title.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(28, 390, 300, 22);
    [view addSubview:title];

    NSTextField *description = [NSTextField labelWithString:@"Sample-rate changes use the normal CoreAudio device lifecycle; timing values below are read-only diagnostics."];
    description.textColor = NSColor.secondaryLabelColor;
    description.frame = NSMakeRect(28, 364, 540, 20);
    [view addSubview:description];

    NSTextField *state = MacfwDeviceValueLabel(view, @"Connection state", 326);
    NSTextField *active = MacfwDeviceValueLabel(view, @"Active sample rate", 296);
    active.frame = NSMakeRect(200, 296, 155, 20);
    NSTextField *requested = MacfwDeviceValueLabel(view, @"Requested rate", 266);
    NSTextField *pid = MacfwDeviceValueLabel(view, @"Engine PID", 236);
    NSTextField *buffer = MacfwDeviceValueLabel(view, @"CoreAudio buffer", 194);
    NSTextField *bufferControl = MacfwDeviceValueLabel(view, @"Buffer capability", 164);
    NSTextField *outputLatency = MacfwDeviceValueLabel(view, @"Output latency", 128);
    NSTextField *inputLatency = MacfwDeviceValueLabel(view, @"Input latency", 98);
    NSTextField *safety = MacfwDeviceValueLabel(view, @"Safety offsets", 68);

    NSSegmentedControl *rateControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(370, 292, 185, 26)];
    rateControl.segmentCount = 2;
    [rateControl setLabel:@"44.1 kHz" forSegment:0];
    [rateControl setLabel:@"48 kHz" forSegment:1];
    rateControl.target = self;
    rateControl.action = @selector(macfwDeviceRateChanged:);
    rateControl.enabled = NO;
    [view addSubview:rateControl];

    objc_setAssociatedObject(self, kMacfwDeviceStateKey, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceActiveRateKey, active, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceRequestedRateKey, requested, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDevicePidKey, pid, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceRateControlKey, rateControl, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceBufferKey, buffer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceBufferControlKey, bufferControl, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceOutputLatencyKey, outputLatency, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceInputLatencyKey, inputLatency, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceSafetyKey, safety, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    NSTextField *note = [NSTextField labelWithString:@"Latency values are currently HAL-reported diagnostics, not calibrated end-to-end measurements."];
    note.textColor = NSColor.tertiaryLabelColor;
    note.font = [NSFont systemFontOfSize:10];
    note.frame = NSMakeRect(28, 30, 535, 18);
    [view addSubview:note];
}

- (void)macfwDeviceRateChanged:(NSSegmentedControl *)sender {
    if (sender.selectedSegment < 0 || sender.selectedSegment > 1) return;

    AudioObjectID device = MacfwFindCoreAudioDevice();
    if (device == kAudioObjectUnknown) {
        NSBeep();
        [self macfwRefreshDevice];
        return;
    }

    const Float64 rate = sender.selectedSegment == 0 ? 44100.0 : 48000.0;
    const OSStatus status = MacfwSetNominalSampleRate(device, rate);
    if (status != noErr) {
        NSBeep();
        [self macfwRefreshDevice];
        return;
    }

    sender.enabled = NO;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        sender.enabled = YES;
        [self refresh:nil];
    });
}

- (void)macfwRefreshDevice {
    NSTextField *stateField = objc_getAssociatedObject(self, kMacfwDeviceStateKey);
    NSTextField *activeField = objc_getAssociatedObject(self, kMacfwDeviceActiveRateKey);
    NSTextField *requestedField = objc_getAssociatedObject(self, kMacfwDeviceRequestedRateKey);
    NSTextField *pidField = objc_getAssociatedObject(self, kMacfwDevicePidKey);
    NSSegmentedControl *rateControl = objc_getAssociatedObject(self, kMacfwDeviceRateControlKey);
    NSTextField *bufferField = objc_getAssociatedObject(self, kMacfwDeviceBufferKey);
    NSTextField *bufferControlField = objc_getAssociatedObject(self, kMacfwDeviceBufferControlKey);
    NSTextField *outputLatencyField = objc_getAssociatedObject(self, kMacfwDeviceOutputLatencyKey);
    NSTextField *inputLatencyField = objc_getAssociatedObject(self, kMacfwDeviceInputLatencyKey);
    NSTextField *safetyField = objc_getAssociatedObject(self, kMacfwDeviceSafetyKey);
    if (!stateField || !activeField || !requestedField || !pidField || !rateControl || !bufferField ||
        !bufferControlField || !outputLatencyField || !inputLatencyField || !safetyField) return;

    int status = 0;
    NSString *text = MacfwDeviceRunStatus(&status);
    double sampleRate = 0.0;
    if (status == 0) {
        NSString *state = MacfwDeviceValue(@"transport state:", text);
        NSString *active = MacfwDeviceValue(@"active rate:", text);
        NSString *requested = MacfwDeviceValue(@"requested rate:", text);
        NSString *pid = MacfwDeviceValue(@"engine pid:", text);

        stateField.stringValue = state.length ? state : @"Unknown";
        activeField.stringValue = active.length ? active : @"Unknown";
        requestedField.stringValue = requested.length ? requested : @"Unknown";
        pidField.stringValue = pid.length ? pid : @"Unknown";

        NSScanner *scanner = [NSScanner scannerWithString:active ?: @""];
        [scanner scanDouble:&sampleRate];
    } else {
        stateField.stringValue = @"Unavailable";
        activeField.stringValue = @"Unavailable";
        requestedField.stringValue = @"Unavailable";
        pidField.stringValue = @"Unavailable";
    }

    AudioObjectID device = MacfwFindCoreAudioDevice();
    if (device == kAudioObjectUnknown) {
        rateControl.enabled = NO;
        rateControl.selectedSegment = -1;
        bufferField.stringValue = @"CoreAudio device unavailable";
        bufferControlField.stringValue = @"—";
        outputLatencyField.stringValue = @"—";
        inputLatencyField.stringValue = @"—";
        safetyField.stringValue = @"—";
        return;
    }

    BOOL rateSettable = NO;
    const BOOL hasRateSettableInfo = MacfwPropertySettable(device,
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rateSettable);
    rateControl.enabled = hasRateSettableInfo && rateSettable;
    if (sampleRate > 44099.0 && sampleRate < 44101.0) {
        rateControl.selectedSegment = 0;
    } else if (sampleRate > 47999.0 && sampleRate < 48001.0) {
        rateControl.selectedSegment = 1;
    } else {
        rateControl.selectedSegment = -1;
    }

    UInt32 frames = 0;
    const BOOL hasBuffer = MacfwReadUInt32(device, kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal, &frames);
    bufferField.stringValue = hasBuffer ? MacfwFramesAndMs(frames, sampleRate) : @"Not exposed";

    BOOL settable = NO;
    const BOOL hasSettableInfo = MacfwPropertySettable(device, kAudioDevicePropertyBufferFrameSize,
                                                       kAudioObjectPropertyScopeGlobal, &settable);
    AudioValueRange range{};
    const BOOL hasRange = MacfwReadBufferRange(device, &range);
    NSString *mode = hasSettableInfo ? (settable ? @"settable" : @"read-only") : @"settable unknown";
    if (hasRange) {
        bufferControlField.stringValue = [NSString stringWithFormat:@"%@ • %.0f–%.0f frames",
                                          mode, range.mMinimum, range.mMaximum];
    } else {
        bufferControlField.stringValue = [NSString stringWithFormat:@"%@ • range not exposed", mode];
    }

    UInt32 outputLatency = 0;
    UInt32 inputLatency = 0;
    UInt32 outputSafety = 0;
    UInt32 inputSafety = 0;
    const BOOL hasOutputLatency = MacfwReadUInt32(device, kAudioDevicePropertyLatency,
                                                  kAudioObjectPropertyScopeOutput, &outputLatency);
    const BOOL hasInputLatency = MacfwReadUInt32(device, kAudioDevicePropertyLatency,
                                                 kAudioObjectPropertyScopeInput, &inputLatency);
    const BOOL hasOutputSafety = MacfwReadUInt32(device, kAudioDevicePropertySafetyOffset,
                                                 kAudioObjectPropertyScopeOutput, &outputSafety);
    const BOOL hasInputSafety = MacfwReadUInt32(device, kAudioDevicePropertySafetyOffset,
                                                kAudioObjectPropertyScopeInput, &inputSafety);

    outputLatencyField.stringValue = hasOutputLatency ? MacfwFramesAndMs(outputLatency, sampleRate) : @"Not exposed";
    inputLatencyField.stringValue = hasInputLatency ? MacfwFramesAndMs(inputLatency, sampleRate) : @"Not exposed";
    safetyField.stringValue = (hasOutputSafety || hasInputSafety)
        ? [NSString stringWithFormat:@"out %u / in %u frames",
           hasOutputSafety ? outputSafety : 0,
           hasInputSafety ? inputSafety : 0]
        : @"Not exposed";
}

@end
