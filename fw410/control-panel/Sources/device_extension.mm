#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static NSString *const kMacfwDeviceStatusTool = @"/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus";

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)refresh:(id)sender;
@end

static const void *kMacfwDeviceStateKey = &kMacfwDeviceStateKey;
static const void *kMacfwDeviceActiveRateKey = &kMacfwDeviceActiveRateKey;
static const void *kMacfwDeviceRequestedRateKey = &kMacfwDeviceRequestedRateKey;
static const void *kMacfwDevicePidKey = &kMacfwDevicePidKey;
static const void *kMacfwDeviceTransitionsKey = &kMacfwDeviceTransitionsKey;
static const void *kMacfwDeviceHeartbeatKey = &kMacfwDeviceHeartbeatKey;

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
    name.frame = NSMakeRect(28, y, 150, 20);
    [view addSubview:name];

    NSTextField *value = [NSTextField labelWithString:@"—"];
    value.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
    value.frame = NSMakeRect(190, y, 360, 20);
    [view addSubview:value];
    return value;
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

    NSTextField *title = [NSTextField labelWithString:@"Transport / Device State"];
    title.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(28, 386, 300, 22);
    [view addSubview:title];

    NSTextField *description = [NSTextField labelWithString:@"Read-only state from the active macfw transport. Sample-rate changes remain under CoreAudio control."];
    description.textColor = NSColor.secondaryLabelColor;
    description.frame = NSMakeRect(28, 358, 540, 20);
    [view addSubview:description];

    NSTextField *state = MacfwDeviceValueLabel(view, @"Connection state", 306);
    NSTextField *active = MacfwDeviceValueLabel(view, @"Active sample rate", 270);
    NSTextField *requested = MacfwDeviceValueLabel(view, @"Requested rate", 234);
    NSTextField *pid = MacfwDeviceValueLabel(view, @"Engine PID", 198);
    NSTextField *transitions = MacfwDeviceValueLabel(view, @"State transitions", 162);
    NSTextField *heartbeat = MacfwDeviceValueLabel(view, @"Heartbeat", 126);

    objc_setAssociatedObject(self, kMacfwDeviceStateKey, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceActiveRateKey, active, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceRequestedRateKey, requested, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDevicePidKey, pid, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceTransitionsKey, transitions, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwDeviceHeartbeatKey, heartbeat, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    NSTextField *note = [NSTextField wrappingLabelWithString:@"Clock-source selection is intentionally not exposed yet. Its FW410 semantics will be established from the Linux/FFADO references and hardware-tested before any write control is added."];
    note.textColor = NSColor.secondaryLabelColor;
    note.frame = NSMakeRect(28, 55, 535, 48);
    [view addSubview:note];
}

- (void)macfwRefreshDevice {
    NSArray<NSTextField *> *fields = @[
        objc_getAssociatedObject(self, kMacfwDeviceStateKey) ?: [NSNull null],
        objc_getAssociatedObject(self, kMacfwDeviceActiveRateKey) ?: [NSNull null],
        objc_getAssociatedObject(self, kMacfwDeviceRequestedRateKey) ?: [NSNull null],
        objc_getAssociatedObject(self, kMacfwDevicePidKey) ?: [NSNull null],
        objc_getAssociatedObject(self, kMacfwDeviceTransitionsKey) ?: [NSNull null],
        objc_getAssociatedObject(self, kMacfwDeviceHeartbeatKey) ?: [NSNull null]
    ];
    if ([fields[0] isKindOfClass:[NSNull class]]) return;

    int status = 0;
    NSString *text = MacfwDeviceRunStatus(&status);
    if (status != 0) {
        for (id field in fields) if ([field isKindOfClass:[NSTextField class]]) ((NSTextField *)field).stringValue = @"Unavailable";
        return;
    }

    NSString *state = MacfwDeviceValue(@"transport state:", text);
    NSString *active = MacfwDeviceValue(@"active rate:", text);
    NSString *requested = MacfwDeviceValue(@"requested rate:", text);
    NSString *pid = MacfwDeviceValue(@"engine pid:", text);
    NSString *transitions = MacfwDeviceValue(@"transitions:", text);
    NSString *heartbeat = MacfwDeviceValue(@"heartbeat:", text);

    ((NSTextField *)fields[0]).stringValue = state.length ? state : @"Unknown";
    ((NSTextField *)fields[1]).stringValue = active.length ? active : @"Unknown";
    ((NSTextField *)fields[2]).stringValue = requested.length ? requested : @"Unknown";
    ((NSTextField *)fields[3]).stringValue = pid.length ? pid : @"Unknown";
    ((NSTextField *)fields[4]).stringValue = transitions.length ? transitions : @"Unknown";
    ((NSTextField *)fields[5]).stringValue = heartbeat.length ? heartbeat : @"Unknown";
}

@end
