#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

static NSString *const kControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";
static NSString *const kStatusTool = @"/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus";

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
            return launchError.localizedDescription ?: @"task failed";
        }
        [task waitUntilExit];
    } @catch (NSException *exception) {
        if (statusOut) *statusOut = 126;
        return exception.reason ?: @"task failed";
    }
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    if (statusOut) *statusOut = task.terminationStatus;
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSTextField *statusLabel;
@property NSSegmentedControl *sourceControl;
@property NSSlider *leftSlider;
@property NSSlider *rightSlider;
@property NSTextField *leftValue;
@property NSTextField *rightValue;
@property NSArray<NSButton *> *mixerButtons;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSRect frame = NSMakeRect(0, 0, 560, 470);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"macfw FW410 Control";
    [self.window center];

    NSView *content = self.window.contentView;
    CGFloat y = 425;

    NSTextField *title = [NSTextField labelWithString:@"M-Audio FireWire 410"];
    title.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(24, y, 320, 28);
    [content addSubview:title];

    self.statusLabel = [NSTextField labelWithString:@"Checking device…"];
    self.statusLabel.textColor = NSColor.secondaryLabelColor;
    self.statusLabel.frame = NSMakeRect(24, y - 28, 430, 20);
    [content addSubview:self.statusLabel];

    NSButton *refresh = [NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refresh:)];
    refresh.frame = NSMakeRect(455, y - 2, 80, 28);
    [content addSubview:refresh];

    y -= 80;
    NSTextField *sourceTitle = [NSTextField labelWithString:@"Headphone Source"];
    sourceTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    sourceTitle.frame = NSMakeRect(24, y, 180, 20);
    [content addSubview:sourceTitle];

    self.sourceControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(24, y - 38, 300, 28)];
    self.sourceControl.segmentCount = 2;
    [self.sourceControl setLabel:@"Mixer" forSegment:0];
    [self.sourceControl setLabel:@"Auxiliary" forSegment:1];
    self.sourceControl.target = self;
    self.sourceControl.action = @selector(sourceChanged:);
    [content addSubview:self.sourceControl];

    y -= 90;
    NSTextField *levelTitle = [NSTextField labelWithString:@"Headphone Level"];
    levelTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    levelTitle.frame = NSMakeRect(24, y, 180, 20);
    [content addSubview:levelTitle];

    [self addSliderTo:content label:@"L" y:y - 38 left:YES];
    [self addSliderTo:content label:@"R" y:y - 76 left:NO];

    y -= 125;
    NSTextField *mixerTitle = [NSTextField labelWithString:@"Mixer Sources"];
    mixerTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    mixerTitle.frame = NSMakeRect(24, y, 180, 20);
    [content addSubview:mixerTitle];

    NSArray<NSString *> *labels = @[@"1/2", @"3/4", @"5/6", @"7/8", @"9/10"];
    NSMutableArray *buttons = [NSMutableArray array];
    CGFloat x = 24;
    for (NSInteger i = 0; i < labels.count; ++i) {
        NSButton *b = [NSButton checkboxWithTitle:labels[i] target:self action:@selector(mixerChanged:)];
        b.tag = i;
        b.frame = NSMakeRect(x, y - 38, 82, 24);
        [content addSubview:b];
        [buttons addObject:b];
        x += 98;
    }
    self.mixerButtons = buttons;

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self refresh:nil];
}

- (void)addSliderTo:(NSView *)content label:(NSString *)label y:(CGFloat)y left:(BOOL)isLeft {
    NSTextField *lab = [NSTextField labelWithString:label];
    lab.frame = NSMakeRect(24, y + 2, 20, 20);
    lab.textColor = NSColor.secondaryLabelColor;
    [content addSubview:lab];

    NSSlider *slider = [[NSSlider alloc] initWithFrame:NSMakeRect(50, y, 390, 24)];
    slider.minValue = -128;
    slider.maxValue = 0;
    slider.numberOfTickMarks = 0;
    slider.continuous = YES;
    slider.target = self;
    slider.action = @selector(volumeSliderMoved:);
    [content addSubview:slider];

    NSTextField *value = [NSTextField labelWithString:@"0 dB"];
    value.alignment = NSTextAlignmentRight;
    value.frame = NSMakeRect(450, y + 2, 80, 20);
    [content addSubview:value];

    if (isLeft) {
        self.leftSlider = slider;
        self.leftValue = value;
    } else {
        self.rightSlider = slider;
        self.rightValue = value;
    }
}

- (void)refresh:(id)sender {
    int st = 0;
    NSString *source = RunTool(kControlTool, @[@"headphone-source", @"get"], &st);
    if (st != 0) {
        self.statusLabel.stringValue = [NSString stringWithFormat:@"Unavailable — %@", source];
        return;
    }
    self.sourceControl.selectedSegment = [source hasPrefix:@"aux"] ? 1 : 0;

    NSString *volume = RunTool(kControlTool, @[@"headphone-volume", @"get"], &st);
    if (st == 0) [self parseVolume:volume];

    NSString *mixer = RunTool(kControlTool, @[@"headphone-mixer", @"get"], &st);
    if (st == 0) [self parseMixer:mixer];

    NSString *status = RunTool(kStatusTool, @[], &st);
    NSString *state = @"Control online";
    NSString *rate = @"";
    if (st == 0) {
        for (NSString *line in [status componentsSeparatedByString:@"\n"]) {
            NSString *trim = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if ([trim hasPrefix:@"transport state:"]) state = [[trim substringFromIndex:16] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if ([trim hasPrefix:@"active rate:"]) rate = [[trim substringFromIndex:12] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }
    }
    self.statusLabel.stringValue = rate.length ? [NSString stringWithFormat:@"Connected • %@ • %@", state, rate] : [NSString stringWithFormat:@"Connected • %@", state];
}

- (void)parseVolume:(NSString *)text {
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"left:"]) {
            double v = [self dbFromLine:line];
            self.leftSlider.doubleValue = v;
            self.leftValue.stringValue = [self displayDb:v];
        } else if ([line hasPrefix:@"right:"]) {
            double v = [self dbFromLine:line];
            self.rightSlider.doubleValue = v;
            self.rightValue.stringValue = [self displayDb:v];
        }
    }
}

- (double)dbFromLine:(NSString *)line {
    if ([line containsString:@"-inf"]) return -128;

    NSRange colon = [line rangeOfString:@":"];
    if (colon.location == NSNotFound || colon.location + 1 >= line.length) return 0;

    NSString *tail = [[line substringFromIndex:colon.location + 1]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSScanner *scanner = [NSScanner scannerWithString:tail];
    double value = 0;
    if ([scanner scanDouble:&value]) return value;
    return 0;
}

- (NSString *)displayDb:(double)v {
    return v <= -128 ? @"−∞" : [NSString stringWithFormat:@"%.0f dB", v];
}

- (void)parseMixer:(NSString *)text {
    NSArray *lines = [text componentsSeparatedByString:@"\n"];
    for (NSInteger i = 0; i < MIN((NSInteger)lines.count, (NSInteger)self.mixerButtons.count); ++i) {
        self.mixerButtons[i].state = [lines[i] hasSuffix:@"on"] ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (void)sourceChanged:(NSSegmentedControl *)sender {
    NSString *value = sender.selectedSegment == 1 ? @"aux" : @"mixer";
    int st = 0;
    RunTool(kControlTool, @[@"headphone-source", @"set", value], &st);
    [self refresh:nil];
}

- (void)volumeSliderMoved:(NSSlider *)sender {
    self.leftValue.stringValue = [self displayDb:round(self.leftSlider.doubleValue)];
    self.rightValue.stringValue = [self displayDb:round(self.rightSlider.doubleValue)];

    NSEvent *event = NSApp.currentEvent;
    if (event && event.type == NSEventTypeLeftMouseDragged) return;

    [self commitVolume];
}

- (void)commitVolume {
    int l = (int)llround(self.leftSlider.doubleValue);
    int r = (int)llround(self.rightSlider.doubleValue);
    int st = 0;
    RunTool(kControlTool,
            @[@"headphone-volume", @"set",
              [NSString stringWithFormat:@"%d", l],
              [NSString stringWithFormat:@"%d", r]],
            &st);
    [self refresh:nil];
}

- (void)mixerChanged:(NSButton *)sender {
    NSArray *labels = @[@"1/2", @"3/4", @"5/6", @"7/8", @"9/10"];
    NSString *state = sender.state == NSControlStateValueOn ? @"on" : @"off";
    int st = 0;
    RunTool(kControlTool, @[@"headphone-mixer", @"set", labels[sender.tag], state], &st);
    [self refresh:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
