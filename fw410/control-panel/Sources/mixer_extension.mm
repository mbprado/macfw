#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static NSString *const kMixerControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";
static NSString *const kMixerStateTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state";

// GUI rows are presented in CoreAudio/Logic order. The FW410 AV/C software-return
// identifiers are rotated relative to macfw's AMDTP channel order, so translate
// the four analog playback pairs plus S/PDIF here rather than exposing the raw
// AV/C naming to the user.
static NSArray<NSString *> *MixerSourceArgs(void) {
    return @[@"analog", @"spdif-in", @"sw3/4", @"sw5/6", @"sw7/8", @"sw9/10", @"sw1/2"];
}

static NSArray<NSString *> *MixerSourceNames(void) {
    return @[@"Analog In 1/2", @"S/PDIF In L/R", @"SW Return 1/2", @"SW Return 3/4",
             @"SW Return 5/6", @"SW Return 7/8", @"SW Return 9/10"];
}

static NSArray<NSString *> *MixerBusArgs(void) {
    return @[@"1/2", @"3/4", @"5/6", @"7/8", @"spdif"];
}

static NSArray<NSString *> *MixerBusNames(void) {
    return @[@"1/2", @"3/4", @"5/6", @"7/8", @"SPD"];
}

static NSString *RunMixerTool(NSString *path, NSArray<NSString *> *args, int *statusOut) {
    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = args;
    task.standardOutput = pipe;
    task.standardError = pipe;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        if (statusOut) *statusOut = 126;
        NSString *description = error.localizedDescription;
        return description != nil ? description : @"task failed";
    }
    [task waitUntilExit];
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString *decoded = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    NSString *text = decoded != nil ? decoded : @"";
    if (statusOut) *statusOut = task.terminationStatus;
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *RunMixerControl(NSArray<NSString *> *args, int *statusOut) {
    return RunMixerTool(kMixerControlTool, args, statusOut);
}

// `fw410ctl mixer get` obtains the complete matrix from the transport's cached
// MAIN_MIXER state in one socket transaction. Keep the GUI's CoreAudio-facing
// row order separate from the raw AV/C source order printed by fw410ctl.
static BOOL ParseMainMixerState(NSString *text, BOOL routes[7][5]) {
    NSArray<NSString *> *lines = [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    NSMutableArray<NSArray<NSNumber *> *> *rawRows = [NSMutableArray arrayWithCapacity:7];
    for (NSString *line in lines) {
        NSRange colon = [line rangeOfString:@":"];
        if (colon.location == NSNotFound || [line containsString:@"FW410 main mixer"]) continue;
        NSString *values = [line substringFromIndex:colon.location + 1];
        NSArray<NSString *> *parts = [values componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSMutableArray<NSNumber *> *row = [NSMutableArray arrayWithCapacity:5];
        for (NSString *part in parts) {
            if (part.length == 0) continue;
            NSRange equals = [part rangeOfString:@"="];
            if (equals.location == NSNotFound) continue;
            NSString *state = [part substringFromIndex:equals.location + 1];
            if (![state isEqualToString:@"on"] && ![state isEqualToString:@"off"]) return NO;
            [row addObject:@([state isEqualToString:@"on"] )];
        }
        if (row.count != 5) return NO;
        [rawRows addObject:row];
    }
    if (rawRows.count != 7) return NO;

    // fw410ctl raw order: analog, spdif-in, sw1/2, sw3/4, sw5/6, sw7/8, sw9/10.
    // GUI/CoreAudio order: analog, spdif-in, sw3/4, sw5/6, sw7/8, sw9/10, sw1/2.
    static const NSUInteger guiToRaw[7] = {0, 1, 3, 4, 5, 6, 2};
    for (NSUInteger guiSrc = 0; guiSrc < 7; ++guiSrc)
        for (NSUInteger dst = 0; dst < 5; ++dst)
            routes[guiSrc][dst] = rawRows[guiToRaw[guiSrc]][dst].boolValue;
    return YES;
}

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)refresh:(id)sender;
@end

static const void *kMainMixerButtonsKey = &kMainMixerButtonsKey;

@implementation AppDelegate (MacfwMainMixer)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;
    Method original = class_getInstanceMethod(cls, @selector(applicationDidFinishLaunching:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfwMixer_applicationDidFinishLaunching:));
    method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(refresh:));
    replacement = class_getInstanceMethod(cls, @selector(macfwMixer_refresh:));
    method_exchangeImplementations(original, replacement);
}

- (void)macfwMixer_applicationDidFinishLaunching:(NSNotification *)notification {
    [self macfwMixer_applicationDidFinishLaunching:notification];
    [self macfwBuildMainMixerTab];
    [self macfwAddResetDefaultsButton];
    [self macfwRefreshMainMixer];
}

- (void)macfwMixer_refresh:(id)sender {
    [self macfwMixer_refresh:sender];
    [self macfwRefreshMainMixer];
}

- (NSTabView *)macfwMixerTabView {
    @try { return [self valueForKey:@"tabView"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (NSWindow *)macfwMixerWindow {
    @try { return [self valueForKey:@"window"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (void)macfwAddResetDefaultsButton {
    NSWindow *window = [self macfwMixerWindow];
    if (!window.contentView) return;
    NSButton *reset = [NSButton buttonWithTitle:@"Reset Defaults" target:self action:@selector(macfwResetDefaults:)];
    reset.frame = NSMakeRect(430, 530, 105, 28);
    reset.toolTip = @"Restore the validated macfw routing and level defaults and replace the saved control state.";
    [window.contentView addSubview:reset];
}

- (void)macfwResetDefaults:(id)sender {
    (void)sender;
    NSAlert *confirm = [[NSAlert alloc] init];
    confirm.messageText = @"Reset FW410 controls to defaults?";
    confirm.informativeText = @"This resets mixer routing, output routing and levels, headphone controls, and AUX levels. The defaults will also become the saved state used after reconnect or reboot.";
    [confirm addButtonWithTitle:@"Reset Defaults"];
    [confirm addButtonWithTitle:@"Cancel"];
    confirm.alertStyle = NSAlertStyleWarning;
    if ([confirm runModal] != NSAlertFirstButtonReturn) return;

    int status = 0;
    NSString *result = RunMixerTool(kMixerStateTool, @[@"reset"], &status);
    if (status != 0) {
        NSAlert *error = [[NSAlert alloc] init];
        error.messageText = @"Could not reset FW410 controls";
        error.informativeText = result.length > 0 ? result : @"The persistent state helper returned an error.";
        error.alertStyle = NSAlertStyleCritical;
        [error runModal];
        return;
    }
    [self refresh:nil];
}

- (void)macfwBuildMainMixerTab {
    NSTabView *tabs = [self macfwMixerTabView];
    if (!tabs) return;

    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"mixer"];
    item.label = @"Mixer";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;
    [tabs insertTabViewItem:item atIndex:0];

    NSTextField *description = [NSTextField labelWithString:@"Main mixer routing — each source can feed any mixer bus."];
    description.textColor = NSColor.secondaryLabelColor;
    description.frame = NSMakeRect(20, 397, 550, 20);
    [view addSubview:description];

    NSArray<NSString *> *busNames = MixerBusNames();
    CGFloat labelWidth = 145.0;
    CGFloat buttonWidth = 70.0;
    CGFloat gap = 10.0;
    CGFloat startX = 20.0 + labelWidth;
    for (NSUInteger dst = 0; dst < busNames.count; ++dst) {
        NSTextField *header = [NSTextField labelWithString:busNames[dst]];
        header.alignment = NSTextAlignmentCenter;
        header.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
        header.frame = NSMakeRect(startX + dst * (buttonWidth + gap), 360, buttonWidth, 20);
        [view addSubview:header];
    }

    NSMutableArray<NSButton *> *buttons = [NSMutableArray arrayWithCapacity:35];
    NSArray<NSString *> *sourceNames = MixerSourceNames();
    for (NSUInteger src = 0; src < sourceNames.count; ++src) {
        CGFloat y = 316.0 - src * 43.0;
        NSTextField *label = [NSTextField labelWithString:sourceNames[src]];
        label.frame = NSMakeRect(20, y + 3, labelWidth - 8, 20);
        [view addSubview:label];

        for (NSUInteger dst = 0; dst < busNames.count; ++dst) {
            NSButton *button = [NSButton checkboxWithTitle:@"" target:self action:@selector(macfwMainMixerChanged:)];
            button.tag = (NSInteger)(src * busNames.count + dst);
            button.frame = NSMakeRect(startX + dst * (buttonWidth + gap) + 24, y, 24, 24);
            button.toolTip = [NSString stringWithFormat:@"%@ → Mixer %@", sourceNames[src], busNames[dst]];
            [view addSubview:button];
            [buttons addObject:button];
        }
    }
    objc_setAssociatedObject(self, kMainMixerButtonsKey, buttons, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (void)macfwRefreshMainMixer {
    NSArray<NSButton *> *buttons = objc_getAssociatedObject(self, kMainMixerButtonsKey);
    if (buttons.count != 35) return;

    int status = 0;
    NSString *text = RunMixerControl(@[@"mixer", @"get"], &status);
    BOOL routes[7][5] = {};
    if (status != 0 || !ParseMainMixerState(text, routes)) return;

    for (NSUInteger src = 0; src < 7; ++src)
        for (NSUInteger dst = 0; dst < 5; ++dst)
            buttons[src * 5 + dst].state = routes[src][dst] ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)macfwMainMixerChanged:(NSButton *)sender {
    NSArray<NSString *> *sources = MixerSourceArgs();
    NSArray<NSString *> *buses = MixerBusArgs();
    NSInteger src = sender.tag / (NSInteger)buses.count;
    NSInteger dst = sender.tag % (NSInteger)buses.count;
    if (src < 0 || dst < 0 || src >= (NSInteger)sources.count || dst >= (NSInteger)buses.count) return;

    NSString *state = sender.state == NSControlStateValueOn ? @"on" : @"off";
    int status = 0;
    RunMixerControl(@[@"mixer-route", @"set", sources[(NSUInteger)src], buses[(NSUInteger)dst], state], &status);
    if (status != 0) NSBeep();
    [self macfwRefreshMainMixer];
}

@end