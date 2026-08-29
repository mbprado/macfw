#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static NSString *const kMixerControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";

static NSArray<NSString *> *MixerSourceArgs(void) {
    return @[@"analog", @"spdif-in", @"sw1/2", @"sw3/4", @"sw5/6", @"sw7/8", @"sw9/10"];
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

static NSString *RunMixerControl(NSArray<NSString *> *args, int *statusOut) {
    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:kMixerControlTool];
    task.arguments = args;
    task.standardOutput = pipe;
    task.standardError = pipe;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        if (statusOut) *statusOut = 126;
        return error.localizedDescription ?: @"task failed";
    }
    [task waitUntilExit];
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    if (statusOut) *statusOut = task.terminationStatus;
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
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

    NSArray<NSString *> *sources = MixerSourceArgs();
    NSArray<NSString *> *buses = MixerBusArgs();
    for (NSUInteger src = 0; src < sources.count; ++src) {
        for (NSUInteger dst = 0; dst < buses.count; ++dst) {
            int status = 0;
            NSString *text = RunMixerControl(@[@"mixer-route", @"get", sources[src], buses[dst]], &status);
            if (status != 0) continue;
            NSButton *button = buttons[src * buses.count + dst];
            button.state = [text containsString:@": on (1)"] ? NSControlStateValueOn : NSControlStateValueOff;
        }
    }
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
