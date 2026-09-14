#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "version.h"
#include <cmath>

static NSString *const kControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";
static NSString *const kStatusTool = @"/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus";
static NSString *const kDeviceProbe = @"/Library/Application Support/macfw/fw410/tools/device/deviceprobe/deviceprobe";
static NSString *const kHALInfoPlist = @"/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver/Contents/Info.plist";

static NSString *RunTool(NSString *path, NSArray<NSString *> *args, int *statusOut) {
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:path]) {
        if (statusOut) *statusOut = 127;
        return [NSString stringWithFormat:@"missing executable: %@", path];
    }

    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = args;
    task.standardOutput = pipe;
    task.standardError = pipe;

    @try {
        NSError *launchError = nil;
        if (![task launchAndReturnError:&launchError]) {
            if (statusOut) *statusOut = 126;
            NSString *description = launchError.localizedDescription;
            return description != nil ? description : @"task failed";
        }
        [task waitUntilExit];
    } @catch (NSException *exception) {
        if (statusOut) *statusOut = 126;
        NSString *reason = exception.reason;
        return reason != nil ? reason : @"task failed";
    }

    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString *decoded = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    NSString *text = decoded != nil ? decoded : @"";
    if (statusOut) *statusOut = task.terminationStatus;
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *ValueAfterPrefix(NSString *prefix, NSString *text) {
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSString *trim = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([trim hasPrefix:prefix]) {
            return [[trim substringFromIndex:prefix.length]
                    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }
    }
    return nil;
}

static NSString *FirstIORegistryObjectName(NSString *text) {
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange marker = [line rangeOfString:@"+-o "];
        if (marker.location == NSNotFound) continue;
        NSString *tail = [line substringFromIndex:marker.location + marker.length];
        NSRange classMarker = [tail rangeOfString:@"  <class"];
        if (classMarker.location != NSNotFound) tail = [tail substringToIndex:classMarker.location];
        tail = [tail stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (tail.length > 0) return tail;
    }
    return nil;
}

static NSString *FindFireWirePCIController(NSString *text) {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSString *currentHeading = nil;
    for (NSString *line in lines) {
        NSString *trim = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (trim.length == 0) continue;
        if ([trim hasSuffix:@":"] && ![trim containsString:@"Vendor ID"] &&
            ![trim containsString:@"Device ID"] && ![trim containsString:@"Revision ID"] &&
            ![trim containsString:@"Subsystem"]) {
            currentHeading = [trim substringToIndex:trim.length - 1];
        }
        if ([trim localizedCaseInsensitiveContainsString:@"firewire"] ||
            [trim localizedCaseInsensitiveContainsString:@"ieee 1394"]) {
            if (currentHeading.length > 0 &&
                ![currentHeading localizedCaseInsensitiveContainsString:@"pci cards"]) {
                return currentHeading;
            }
        }
    }
    return nil;
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSTextField *statusLabel;
@property NSTabView *tabView;
@property NSSegmentedControl *sourceControl;
@property NSSlider *leftSlider;
@property NSSlider *rightSlider;
@property NSTextField *leftValue;
@property NSTextField *rightValue;
@property NSArray<NSButton *> *mixerButtons;
@property NSSlider *auxStreamLeftSlider;
@property NSSlider *auxStreamRightSlider;
@property NSTextField *auxStreamLeftValue;
@property NSTextField *auxStreamRightValue;
@property NSSlider *auxOutputLeftSlider;
@property NSSlider *auxOutputRightSlider;
@property NSTextField *auxOutputLeftValue;
@property NSTextField *auxOutputRightValue;
@property NSTextView *infoTextView;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    NSRect frame = NSMakeRect(0, 0, 650, 580);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"macfw FW410 Control";
    [self.window center];

    NSView *content = self.window.contentView;

    NSTextField *title = [NSTextField labelWithString:@"M-Audio FireWire 410"];
    title.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(24, 535, 340, 28);
    [content addSubview:title];

    self.statusLabel = [NSTextField labelWithString:@"Checking device…"];
    self.statusLabel.textColor = NSColor.secondaryLabelColor;
    self.statusLabel.frame = NSMakeRect(24, 507, 475, 20);
    [content addSubview:self.statusLabel];

    NSButton *refresh = [NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refresh:)];
    refresh.frame = NSMakeRect(545, 530, 80, 28);
    [content addSubview:refresh];

    self.tabView = [[NSTabView alloc] initWithFrame:NSMakeRect(18, 18, 614, 475)];
    [content addSubview:self.tabView];

    [self buildHeadphonesTab];
    [self buildAuxTab];
    [self buildInfoTab];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self refresh:nil];
}

- (void)buildHeadphonesTab {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"headphones"];
    item.label = @"Headphones";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;
    [self.tabView addTabViewItem:item];

    NSTextField *sourceTitle = [NSTextField labelWithString:@"Headphone Source"];
    sourceTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    sourceTitle.frame = NSMakeRect(24, 385, 180, 20);
    [view addSubview:sourceTitle];

    self.sourceControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(24, 342, 330, 28)];
    self.sourceControl.segmentCount = 2;
    [self.sourceControl setLabel:@"Mixer" forSegment:0];
    [self.sourceControl setLabel:@"Auxiliary" forSegment:1];
    self.sourceControl.target = self;
    self.sourceControl.action = @selector(sourceChanged:);
    [view addSubview:self.sourceControl];

    NSTextField *levelTitle = [NSTextField labelWithString:@"Headphone Level"];
    levelTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    levelTitle.frame = NSMakeRect(24, 292, 180, 20);
    [view addSubview:levelTitle];

    [self addSliderTo:view label:@"L" y:250 slider:&_leftSlider valueField:&_leftValue action:@selector(volumeSliderMoved:)];
    [self addSliderTo:view label:@"R" y:212 slider:&_rightSlider valueField:&_rightValue action:@selector(volumeSliderMoved:)];

    NSTextField *mixerTitle = [NSTextField labelWithString:@"Mixer Sources"];
    mixerTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    mixerTitle.frame = NSMakeRect(24, 158, 180, 20);
    [view addSubview:mixerTitle];

    NSTextField *hint = [NSTextField labelWithString:@"Choose which output pairs feed the headphones when Mixer is selected."];
    hint.textColor = NSColor.secondaryLabelColor;
    hint.frame = NSMakeRect(24, 132, 500, 20);
    [view addSubview:hint];

    NSArray<NSString *> *labels = @[@"1/2", @"3/4", @"5/6", @"7/8", @"9/10"];
    NSMutableArray<NSButton *> *buttons = [NSMutableArray array];
    CGFloat x = 24;
    for (NSUInteger i = 0; i < labels.count; ++i) {
        NSButton *button = [NSButton checkboxWithTitle:labels[i] target:self action:@selector(mixerChanged:)];
        button.tag = (NSInteger)i;
        button.frame = NSMakeRect(x, 90, 82, 24);
        [view addSubview:button];
        [buttons addObject:button];
        x += 103;
    }
    self.mixerButtons = buttons;
}

- (void)buildAuxTab {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"aux"];
    item.label = @"AUX";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;
    [self.tabView addTabViewItem:item];

    NSTextField *description = [NSTextField labelWithString:@"AUX is a separate stereo mix path and can be selected directly as the headphone source."];
    description.textColor = NSColor.secondaryLabelColor;
    description.frame = NSMakeRect(24, 385, 545, 20);
    [view addSubview:description];

    NSTextField *streamTitle = [NSTextField labelWithString:@"Software Return 1/2 → AUX"];
    streamTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    streamTitle.frame = NSMakeRect(24, 335, 250, 20);
    [view addSubview:streamTitle];

    [self addSliderTo:view label:@"L" y:293 slider:&_auxStreamLeftSlider valueField:&_auxStreamLeftValue action:@selector(auxStreamSliderMoved:)];
    [self addSliderTo:view label:@"R" y:255 slider:&_auxStreamRightSlider valueField:&_auxStreamRightValue action:@selector(auxStreamSliderMoved:)];

    NSTextField *outputTitle = [NSTextField labelWithString:@"AUX Output Level"];
    outputTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    outputTitle.frame = NSMakeRect(24, 195, 250, 20);
    [view addSubview:outputTitle];

    [self addSliderTo:view label:@"L" y:153 slider:&_auxOutputLeftSlider valueField:&_auxOutputLeftValue action:@selector(auxOutputSliderMoved:)];
    [self addSliderTo:view label:@"R" y:115 slider:&_auxOutputRightSlider valueField:&_auxOutputRightValue action:@selector(auxOutputSliderMoved:)];
}

- (void)buildInfoTab {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"info"];
    item.label = @"Info";

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(14, 14, 570, 405)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;

    self.infoTextView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 550, 405)];
    self.infoTextView.editable = NO;
    self.infoTextView.selectable = YES;
    self.infoTextView.drawsBackground = NO;
    self.infoTextView.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.infoTextView.textContainerInset = NSMakeSize(12, 12);
    scroll.documentView = self.infoTextView;

    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    [view addSubview:scroll];
    item.view = view;
    [self.tabView addTabViewItem:item];
}

- (void)addSliderTo:(NSView *)view
               label:(NSString *)label
                   y:(CGFloat)y
              slider:(NSSlider * __strong *)sliderOut
          valueField:(NSTextField * __strong *)valueOut
              action:(SEL)action {
    NSTextField *lab = [NSTextField labelWithString:label];
    lab.frame = NSMakeRect(24, y + 2, 20, 20);
    lab.textColor = NSColor.secondaryLabelColor;
    [view addSubview:lab];

    NSSlider *slider = [[NSSlider alloc] initWithFrame:NSMakeRect(50, y, 430, 24)];
    slider.minValue = -128;
    slider.maxValue = 0;
    slider.numberOfTickMarks = 0;
    slider.continuous = YES;
    slider.target = self;
    slider.action = action;
    [view addSubview:slider];

    NSTextField *value = [NSTextField labelWithString:@"0 dB"];
    value.alignment = NSTextAlignmentRight;
    value.frame = NSMakeRect(490, y + 2, 80, 20);
    [view addSubview:value];

    *sliderOut = slider;
    *valueOut = value;
}

- (void)refresh:(id)sender {
    (void)sender;
    int status = 0;

    NSString *source = RunTool(kControlTool, @[@"headphone-source", @"get"], &status);
    if (status != 0) {
        self.statusLabel.stringValue = [NSString stringWithFormat:@"Unavailable — %@", source];
        [self refreshInfo];
        return;
    }
    self.sourceControl.selectedSegment = [source hasPrefix:@"aux"] ? 1 : 0;

    NSString *volume = RunTool(kControlTool, @[@"headphone-volume", @"get"], &status);
    if (status == 0) [self parseStereoVolume:volume left:self.leftSlider right:self.rightSlider leftValue:self.leftValue rightValue:self.rightValue];

    NSString *mixer = RunTool(kControlTool, @[@"headphone-mixer", @"get"], &status);
    if (status == 0) [self parseMixer:mixer];

    NSString *auxStream = RunTool(kControlTool, @[@"aux-stream12-volume", @"get"], &status);
    if (status == 0) [self parseStereoVolume:auxStream left:self.auxStreamLeftSlider right:self.auxStreamRightSlider leftValue:self.auxStreamLeftValue rightValue:self.auxStreamRightValue];

    NSString *auxOutput = RunTool(kControlTool, @[@"aux-output-volume", @"get"], &status);
    if (status == 0) [self parseStereoVolume:auxOutput left:self.auxOutputLeftSlider right:self.auxOutputRightSlider leftValue:self.auxOutputLeftValue rightValue:self.auxOutputRightValue];

    NSString *transport = RunTool(kStatusTool, @[], &status);
    NSString *state = @"Control online";
    NSString *rate = @"";
    if (status == 0) {
        NSString *foundState = ValueAfterPrefix(@"transport state:", transport);
        NSString *foundRate = ValueAfterPrefix(@"active rate:", transport);
        if (foundState.length > 0) state = foundState;
        if (foundRate.length > 0) rate = foundRate;
    }
    self.statusLabel.stringValue = rate.length > 0
        ? [NSString stringWithFormat:@"Connected • %@ • %@", state, rate]
        : [NSString stringWithFormat:@"Connected • %@", state];

    [self refreshInfo];
}

- (double)dbFromLine:(NSString *)line {
    if ([line containsString:@"-inf"]) return -128;
    NSRange colon = [line rangeOfString:@":"];
    if (colon.location == NSNotFound || colon.location + 1 >= line.length) return 0;
    NSString *tail = [[line substringFromIndex:colon.location + 1]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSScanner *scanner = [NSScanner scannerWithString:tail];
    double value = 0;
    return [scanner scanDouble:&value] ? value : 0;
}

- (NSString *)displayDb:(double)value {
    return value <= -128 ? @"−∞" : [NSString stringWithFormat:@"%.0f dB", value];
}

- (void)parseStereoVolume:(NSString *)text
                     left:(NSSlider *)left
                    right:(NSSlider *)right
                leftValue:(NSTextField *)leftValue
               rightValue:(NSTextField *)rightValue {
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"left:"]) {
            double value = [self dbFromLine:line];
            left.doubleValue = value;
            leftValue.stringValue = [self displayDb:value];
        } else if ([line hasPrefix:@"right:"]) {
            double value = [self dbFromLine:line];
            right.doubleValue = value;
            rightValue.stringValue = [self displayDb:value];
        }
    }
}

- (void)parseMixer:(NSString *)text {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSInteger count = MIN((NSInteger)lines.count, (NSInteger)self.mixerButtons.count);
    for (NSInteger i = 0; i < count; ++i) {
        self.mixerButtons[(NSUInteger)i].state = [lines[(NSUInteger)i] hasSuffix:@"on"]
            ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (BOOL)isMouseDragging {
    NSEvent *event = NSApp.currentEvent;
    return event != nil && event.type == NSEventTypeLeftMouseDragged;
}

- (void)sourceChanged:(NSSegmentedControl *)sender {
    NSString *value = sender.selectedSegment == 1 ? @"aux" : @"mixer";
    int status = 0;
    RunTool(kControlTool, @[@"headphone-source", @"set", value], &status);
    [self refresh:nil];
}

- (void)volumeSliderMoved:(NSSlider *)sender {
    (void)sender;
    self.leftValue.stringValue = [self displayDb:round(self.leftSlider.doubleValue)];
    self.rightValue.stringValue = [self displayDb:round(self.rightSlider.doubleValue)];
    if (![self isMouseDragging]) [self commitStereoControl:@"headphone-volume" left:self.leftSlider right:self.rightSlider];
}

- (void)auxStreamSliderMoved:(NSSlider *)sender {
    (void)sender;
    self.auxStreamLeftValue.stringValue = [self displayDb:round(self.auxStreamLeftSlider.doubleValue)];
    self.auxStreamRightValue.stringValue = [self displayDb:round(self.auxStreamRightSlider.doubleValue)];
    if (![self isMouseDragging]) [self commitStereoControl:@"aux-stream12-volume" left:self.auxStreamLeftSlider right:self.auxStreamRightSlider];
}

- (void)auxOutputSliderMoved:(NSSlider *)sender {
    (void)sender;
    self.auxOutputLeftValue.stringValue = [self displayDb:round(self.auxOutputLeftSlider.doubleValue)];
    self.auxOutputRightValue.stringValue = [self displayDb:round(self.auxOutputRightSlider.doubleValue)];
    if (![self isMouseDragging]) [self commitStereoControl:@"aux-output-volume" left:self.auxOutputLeftSlider right:self.auxOutputRightSlider];
}

- (void)commitStereoControl:(NSString *)control left:(NSSlider *)left right:(NSSlider *)right {
    int leftDb = (int)llround(left.doubleValue);
    int rightDb = (int)llround(right.doubleValue);
    int status = 0;
    RunTool(kControlTool,
            @[control, @"set",
              [NSString stringWithFormat:@"%d", leftDb],
              [NSString stringWithFormat:@"%d", rightDb]],
            &status);
    [self refresh:nil];
}

- (void)mixerChanged:(NSButton *)sender {
    NSArray<NSString *> *labels = @[@"1/2", @"3/4", @"5/6", @"7/8", @"9/10"];
    if (sender.tag < 0 || sender.tag >= (NSInteger)labels.count) return;
    NSString *state = sender.state == NSControlStateValueOn ? @"on" : @"off";
    int status = 0;
    RunTool(kControlTool, @[@"headphone-mixer", @"set", labels[(NSUInteger)sender.tag], state], &status);
    [self refresh:nil];
}

- (void)refreshInfo {
    NSMutableString *info = [NSMutableString string];

    [info appendString:@"macfw components\n"];
    [info appendFormat:@"  Control Panel:  %s build %s\n", macfw::build::kVersion, macfw::build::kGitSha];

    NSDictionary *halInfo = [NSDictionary dictionaryWithContentsOfFile:kHALInfoPlist];
    if (halInfo != nil) {
        NSString *version = halInfo[@"CFBundleShortVersionString"];
        NSString *build = halInfo[@"MacFWGitCommit"];
        [info appendFormat:@"  CoreAudio HAL:  %@ build %@\n",
         version != nil ? version : @"unknown",
         build != nil ? build : @"unknown"];
    } else {
        [info appendString:@"  CoreAudio HAL:  not installed\n"];
    }

    int status = 0;
    NSString *transport = RunTool(kStatusTool, @[], &status);
    if (status == 0) {
        NSString *state = ValueAfterPrefix(@"transport state:", transport);
        NSString *requested = ValueAfterPrefix(@"requested rate:", transport);
        NSString *active = ValueAfterPrefix(@"active rate:", transport);
        [info appendFormat:@"  Transport:      %@ (requested %@, active %@)\n",
         state != nil ? state : @"unknown",
         requested != nil ? requested : @"unknown",
         active != nil ? active : @"unknown"];
        [info appendString:@"  Runtime build:  not yet persisted by installer\n"];
    } else {
        [info appendString:@"  Transport:      unavailable\n"];
    }

    [info appendString:@"\nmacOS / Mac hardware\n"];
    NSProcessInfo *processInfo = [NSProcessInfo processInfo];
    [info appendFormat:@"  macOS:           %@\n", processInfo.operatingSystemVersionString];

    NSString *buildVersion = RunTool(@"/usr/bin/sw_vers", @[@"-buildVersion"], &status);
    if (status == 0 && buildVersion.length > 0) [info appendFormat:@"  OS build:        %@\n", buildVersion];

    NSString *model = RunTool(@"/usr/sbin/sysctl", @[@"-n", @"hw.model"], &status);
    if (status == 0 && model.length > 0) [info appendFormat:@"  Mac model:       %@\n", model];

    NSString *cpu = RunTool(@"/usr/sbin/sysctl", @[@"-n", @"machdep.cpu.brand_string"], &status);
    if (status == 0 && cpu.length > 0) [info appendFormat:@"  CPU:             %@\n", cpu];

    NSString *ioreg = RunTool(@"/usr/sbin/ioreg", @[@"-r", @"-c", @"IOFireWireController", @"-l"], &status);
    NSString *controller = status == 0 ? FirstIORegistryObjectName(ioreg) : nil;

    NSString *pci = RunTool(@"/usr/sbin/system_profiler", @[@"SPPCIDataType"], &status);
    NSString *pciController = status == 0 ? FindFireWirePCIController(pci) : nil;
    if (pciController.length > 0) {
        [info appendFormat:@"  FireWire ctrl:   %@\n", pciController];
    } else if (controller.length > 0) {
        [info appendFormat:@"  FireWire ctrl:   %@\n", controller];
    } else {
        [info appendString:@"  FireWire ctrl:   not identified\n"];
    }

    NSString *firewire = RunTool(@"/usr/sbin/system_profiler", @[@"SPFireWireDataType"], &status);
    if (status == 0) {
        NSString *speed = ValueAfterPrefix(@"Maximum Speed:", firewire);
        if (speed.length > 0) [info appendFormat:@"  FireWire bus:    %@\n", speed];
    }

    [info appendString:@"\nConnected FireWire interface\n"];
    NSString *device = RunTool(kDeviceProbe, @[], &status);
    if (status == 0) {
        NSArray<NSArray<NSString *> *> *fields = @[
            @[@"model:", @"Model"],
            @[@"product:", @"Product"],
            @[@"personality:", @"Personality"],
            @[@"GUID:", @"GUID"],
            @[@"vendor ID:", @"Vendor ID"],
            @[@"unit spec ID:", @"Unit spec ID"],
            @[@"unit SW ver:", @"Unit SW version"],
            @[@"macfw id:", @"macfw ID"]
        ];
        BOOL found = NO;
        for (NSArray<NSString *> *field in fields) {
            NSString *value = ValueAfterPrefix(field[0], device);
            if (value.length == 0) continue;
            found = YES;
            [info appendFormat:@"  %-16s %@\n", [field[1] UTF8String], value];
        }
        if (!found) [info appendString:@"  FW410 identity not available\n"];
    } else {
        [info appendString:@"  No supported macfw FireWire device detected\n"];
    }

    self.infoTextView.string = info;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
