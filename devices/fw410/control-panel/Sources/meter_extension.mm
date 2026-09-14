#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static const char *kMacfwMeterSocketPath = "/tmp/macfw-fw410-meter.sock";
static constexpr double kMacfwMeterVisualFloorDb = -70.0;

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
@end

static const void *kMacfwMeterBarsKey = &kMacfwMeterBarsKey;
static const void *kMacfwMeterValuesKey = &kMacfwMeterValuesKey;
static const void *kMacfwMeterTimerKey = &kMacfwMeterTimerKey;
static const void *kMacfwMeterPollBusyKey = &kMacfwMeterPollBusyKey;

static NSString *MacfwReadMeters(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nil;

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(kMacfwMeterSocketPath) >= sizeof(address.sun_path)) {
        close(fd);
        return nil;
    }
    std::strncpy(address.sun_path, kMacfwMeterSocketPath, sizeof(address.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return nil;
    }

    static const char request[] = "METERS GET\n";
    ssize_t sent = send(fd, request, sizeof(request) - 1, 0);
    if (sent != static_cast<ssize_t>(sizeof(request) - 1)) {
        close(fd);
        return nil;
    }

    char buffer[256] = {};
    ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0);
    close(fd);
    if (received <= 0) return nil;

    buffer[received] = '\0';
    return [[NSString alloc] initWithBytes:buffer
                                   length:static_cast<NSUInteger>(received)
                                 encoding:NSUTF8StringEncoding];
}

static BOOL MacfwParseMeters(NSString *text, double values[4]) {
    if (![text hasPrefix:@"OK "]) return NO;
    NSScanner *scanner = [NSScanner scannerWithString:text];
    if (![scanner scanString:@"OK" intoString:nil]) return NO;
    for (NSUInteger i = 0; i < 4; ++i) {
        if (![scanner scanDouble:&values[i]]) return NO;
    }
    return YES;
}

@implementation AppDelegate (MacfwMeters)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;
    Method original = class_getInstanceMethod(cls, @selector(applicationDidFinishLaunching:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfwMeters_applicationDidFinishLaunching:));
    method_exchangeImplementations(original, replacement);
}

- (void)macfwMeters_applicationDidFinishLaunching:(NSNotification *)notification {
    [self macfwMeters_applicationDidFinishLaunching:notification];
    [self macfwBuildMetersTab];
    [self macfwStartMeterPolling];
}

- (NSTabView *)macfwMetersTabView {
    @try { return [self valueForKey:@"tabView"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (void)macfwBuildMetersTab {
    NSTabView *tabs = [self macfwMetersTabView];
    if (!tabs) return;

    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"inputs"];
    item.label = @"Inputs";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;

    NSUInteger insertIndex = tabs.numberOfTabViewItems > 0 ? 1 : 0;
    [tabs insertTabViewItem:item atIndex:insertIndex];

    NSTextField *description = [NSTextField labelWithString:@"Live capture levels from the FW410 input stream."];
    description.textColor = NSColor.secondaryLabelColor;
    description.frame = NSMakeRect(24, 392, 540, 20);
    [view addSubview:description];

    NSTextField *scale = [NSTextField labelWithString:@"Meter display: −70 to 0 dBFS • numeric values retain the full transport reading"];
    scale.textColor = NSColor.tertiaryLabelColor;
    scale.font = [NSFont systemFontOfSize:11];
    scale.frame = NSMakeRect(24, 368, 540, 18);
    [view addSubview:scale];

    NSTextField *analogTitle = [NSTextField labelWithString:@"Analog Inputs"];
    analogTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    analogTitle.frame = NSMakeRect(24, 334, 180, 20);
    [view addSubview:analogTitle];

    NSTextField *digitalTitle = [NSTextField labelWithString:@"Digital Input"];
    digitalTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    digitalTitle.frame = NSMakeRect(24, 190, 180, 20);
    [view addSubview:digitalTitle];

    NSArray<NSString *> *names = @[@"Analog Input 1", @"Analog Input 2", @"S/PDIF Input L", @"S/PDIF Input R"];
    NSArray<NSNumber *> *positions = @[@278.0, @218.0, @134.0, @74.0];
    NSMutableArray<NSLevelIndicator *> *bars = [NSMutableArray arrayWithCapacity:4];
    NSMutableArray<NSTextField *> *values = [NSMutableArray arrayWithCapacity:4];

    for (NSUInteger i = 0; i < names.count; ++i) {
        CGFloat y = positions[i].doubleValue;

        NSTextField *label = [NSTextField labelWithString:names[i]];
        label.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
        label.frame = NSMakeRect(24, y + 25, 150, 20);
        [view addSubview:label];

        NSLevelIndicator *bar = [[NSLevelIndicator alloc] initWithFrame:NSMakeRect(174, y + 22, 310, 20)];
        bar.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
        bar.minValue = kMacfwMeterVisualFloorDb;
        bar.maxValue = 0.0;
        bar.doubleValue = kMacfwMeterVisualFloorDb;
        [view addSubview:bar];
        [bars addObject:bar];

        NSTextField *value = [NSTextField labelWithString:@"—"];
        value.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        value.alignment = NSTextAlignmentRight;
        value.frame = NSMakeRect(494, y + 24, 76, 20);
        [view addSubview:value];
        [values addObject:value];

        NSTextField *floor = [NSTextField labelWithString:@"−70"];
        floor.font = [NSFont systemFontOfSize:9];
        floor.textColor = NSColor.tertiaryLabelColor;
        floor.frame = NSMakeRect(174, y + 7, 35, 12);
        [view addSubview:floor];

        NSTextField *zero = [NSTextField labelWithString:@"0 dBFS"];
        zero.font = [NSFont systemFontOfSize:9];
        zero.textColor = NSColor.tertiaryLabelColor;
        zero.alignment = NSTextAlignmentRight;
        zero.frame = NSMakeRect(434, y + 7, 50, 12);
        [view addSubview:zero];
    }

    NSTextField *monitoring = [NSTextField labelWithString:@"Direct monitoring is routed in Mixer. These controls do not change the CoreAudio input channel assignment."];
    monitoring.textColor = NSColor.tertiaryLabelColor;
    monitoring.font = [NSFont systemFontOfSize:10];
    monitoring.frame = NSMakeRect(24, 24, 545, 18);
    [view addSubview:monitoring];

    objc_setAssociatedObject(self, kMacfwMeterBarsKey, bars, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self, kMacfwMeterValuesKey, values, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (void)macfwStartMeterPolling {
    if (objc_getAssociatedObject(self, kMacfwMeterTimerKey)) return;

    NSTimer *timer = [NSTimer timerWithTimeInterval:0.10
                                             target:self
                                           selector:@selector(macfwPollMeters:)
                                           userInfo:nil
                                            repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    objc_setAssociatedObject(self, kMacfwMeterTimerKey, timer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self macfwPollMeters:nil];
}

- (void)macfwPollMeters:(NSTimer *)timer {
    (void)timer;
    NSNumber *busy = objc_getAssociatedObject(self, kMacfwMeterPollBusyKey);
    if (busy.boolValue) return;
    objc_setAssociatedObject(self, kMacfwMeterPollBusyKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    __weak AppDelegate *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *reply = MacfwReadMeters();
        double parsed[4] = {};
        BOOL valid = reply != nil && MacfwParseMeters(reply, parsed);
        NSArray<NSNumber *> *snapshot = valid
            ? @[@(parsed[0]), @(parsed[1]), @(parsed[2]), @(parsed[3])]
            : nil;

        dispatch_async(dispatch_get_main_queue(), ^{
            AppDelegate *strongSelf = weakSelf;
            if (!strongSelf) return;
            objc_setAssociatedObject(strongSelf, kMacfwMeterPollBusyKey, @NO, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [strongSelf macfwApplyMeterSnapshot:snapshot];
        });
    });
}

- (void)macfwApplyMeterSnapshot:(NSArray<NSNumber *> *)snapshot {
    NSArray<NSLevelIndicator *> *bars = objc_getAssociatedObject(self, kMacfwMeterBarsKey);
    NSArray<NSTextField *> *labels = objc_getAssociatedObject(self, kMacfwMeterValuesKey);
    if (bars.count != 4 || labels.count != 4) return;

    if (snapshot.count != 4) {
        for (NSUInteger i = 0; i < 4; ++i) {
            bars[i].doubleValue = kMacfwMeterVisualFloorDb;
            labels[i].stringValue = @"—";
        }
        return;
    }

    for (NSUInteger i = 0; i < 4; ++i) {
        double db = snapshot[i].doubleValue;
        bars[i].doubleValue = db < kMacfwMeterVisualFloorDb ? kMacfwMeterVisualFloorDb : (db > 0.0 ? 0.0 : db);
        labels[i].stringValue = [NSString stringWithFormat:@"%.1f dB", db];
    }
}

@end
