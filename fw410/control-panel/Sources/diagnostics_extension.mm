#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <dispatch/dispatch.h>

static NSString *const kMacfwRuntimeBuildFile = @"/Library/Application Support/macfw/fw410/runtime-build.conf";
static NSString *const kMacfwDiagnosticsStatusTool = @"/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus";
static NSString *const kMacfwTransportLog = @"/Library/Logs/macfw-fw410-transport.log";

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)refreshInfo;
@end

static NSString *MacfwDiagnosticsRunTool(NSString *path, NSArray<NSString *> *args, int *statusOut) {
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:path]) {
        if (statusOut) *statusOut = 127;
        return @"";
    }

    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = args;
    task.standardOutput = pipe;
    task.standardError = pipe;

    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        if (statusOut) *statusOut = 126;
        return error.localizedDescription ?: @"";
    }

    [task waitUntilExit];
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    if (statusOut) *statusOut = task.terminationStatus;
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return [text ?: @"" stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *MacfwDiagnosticsValue(NSString *prefix, NSString *text) {
    for (NSString *line in [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *trim = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (![trim hasPrefix:prefix]) continue;
        return [[trim substringFromIndex:prefix.length]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    }
    return nil;
}

static NSDictionary<NSString *, NSString *> *MacfwRuntimeBuildMetadata(void) {
    NSString *text = [NSString stringWithContentsOfFile:kMacfwRuntimeBuildFile
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    if (text.length == 0) return @{};

    NSMutableDictionary<NSString *, NSString *> *result = [NSMutableDictionary dictionary];
    for (NSString *line in [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSRange equals = [line rangeOfString:@"="];
        if (equals.location == NSNotFound || equals.location == 0) continue;
        NSString *key = [line substringToIndex:equals.location];
        NSString *value = [line substringFromIndex:equals.location + 1];
        if (key.length && value.length) result[key] = value;
    }
    return result;
}

@implementation AppDelegate (MacfwDiagnostics)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;

    Method original = class_getInstanceMethod(cls, @selector(applicationDidFinishLaunching:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfwDiagnostics_applicationDidFinishLaunching:));
    method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(refreshInfo));
    replacement = class_getInstanceMethod(cls, @selector(macfwDiagnostics_refreshInfo));
    method_exchangeImplementations(original, replacement);
}

- (void)macfwDiagnostics_applicationDidFinishLaunching:(NSNotification *)notification {
    [self macfwDiagnostics_applicationDidFinishLaunching:notification];
    [self macfwBuildDiagnosticsActions];
    [self refreshInfo];
}

- (NSTabView *)macfwDiagnosticsTabView {
    @try { return [self valueForKey:@"tabView"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (NSTextView *)macfwDiagnosticsInfoTextView {
    @try { return [self valueForKey:@"infoTextView"]; }
    @catch (NSException *exception) { (void)exception; return nil; }
}

- (void)macfwBuildDiagnosticsActions {
    NSTabView *tabs = [self macfwDiagnosticsTabView];
    if (!tabs) return;

    NSTabViewItem *infoItem = nil;
    for (NSTabViewItem *item in tabs.tabViewItems) {
        if ([item.identifier isEqual:@"info"]) {
            infoItem = item;
            break;
        }
    }
    if (!infoItem.view) return;

    for (NSView *subview in infoItem.view.subviews) {
        if ([subview isKindOfClass:[NSScrollView class]]) {
            NSRect frame = subview.frame;
            frame.origin.y = 54;
            frame.size.height = 365;
            subview.frame = frame;
            break;
        }
    }

    NSButton *copy = [NSButton buttonWithTitle:@"Copy Diagnostics"
                                         target:self
                                         action:@selector(macfwCopyDiagnostics:)];
    copy.frame = NSMakeRect(14, 14, 132, 28);
    [infoItem.view addSubview:copy];

    NSButton *openLog = [NSButton buttonWithTitle:@"Open Transport Log"
                                            target:self
                                            action:@selector(macfwOpenTransportLog:)];
    openLog.frame = NSMakeRect(154, 14, 145, 28);
    [infoItem.view addSubview:openLog];
}

- (void)macfwDiagnostics_refreshInfo {
    [self macfwDiagnostics_refreshInfo];

    NSTextView *infoView = [self macfwDiagnosticsInfoTextView];
    if (!infoView) return;

    NSMutableString *info = [infoView.string mutableCopy] ?: [NSMutableString string];
    NSDictionary<NSString *, NSString *> *metadata = MacfwRuntimeBuildMetadata();
    NSString *version = metadata[@"version"];
    NSString *build = metadata[@"build"];
    if (version.length && build.length) {
        NSString *runtime = [NSString stringWithFormat:@"Runtime build:  %@ build %@", version, build];
        [info replaceOccurrencesOfString:@"Runtime build:  not yet persisted by installer"
                              withString:runtime
                                 options:0
                                   range:NSMakeRange(0, info.length)];
    }

    int status = 0;
    NSString *transport = MacfwDiagnosticsRunTool(kMacfwDiagnosticsStatusTool, @[], &status);
    if (status == 0 && transport.length) {
        NSString *pid = MacfwDiagnosticsValue(@"engine pid:", transport) ?: @"unknown";
        NSString *transitions = MacfwDiagnosticsValue(@"transitions:", transport) ?: @"unknown";
        NSString *heartbeat = MacfwDiagnosticsValue(@"heartbeat:", transport) ?: @"unknown";
        NSString *captureState = MacfwDiagnosticsValue(@"capture state:", transport) ?: @"unknown";
        NSString *queued = MacfwDiagnosticsValue(@"queued frames:", transport) ?: @"unknown";
        NSString *underruns = MacfwDiagnosticsValue(@"underrun events:", transport) ?: @"unknown";
        NSString *zeroFill = MacfwDiagnosticsValue(@"hal zero fill:", transport) ?: @"unknown";

        [info appendString:@"\n\nRuntime diagnostics\n"];
        [info appendFormat:@"  Engine PID:       %@\n", pid];
        [info appendFormat:@"  Transitions:      %@\n", transitions];
        [info appendFormat:@"  Heartbeat:        %@\n", heartbeat];
        [info appendFormat:@"  Capture:          %@ • %@ queued\n", captureState, queued];
        [info appendFormat:@"  HAL underruns:    %@\n", underruns];
        [info appendFormat:@"  HAL zero fill:    %@ frames\n", zeroFill];
    }

    infoView.string = info;
}

- (void)macfwCopyDiagnostics:(NSButton *)sender {
    NSTextView *infoView = [self macfwDiagnosticsInfoTextView];
    NSMutableString *diagnostics = [NSMutableString string];
    [diagnostics appendString:@"macfw FW410 diagnostics\n=======================\n\n"];
    if (infoView.string.length) [diagnostics appendString:infoView.string];

    int status = 0;
    NSString *transport = MacfwDiagnosticsRunTool(kMacfwDiagnosticsStatusTool, @[], &status);
    [diagnostics appendString:@"\n\ntransportstatus\n---------------\n"];
    [diagnostics appendString:(status == 0 && transport.length) ? transport : @"unavailable"];

    NSString *logTail = MacfwDiagnosticsRunTool(@"/usr/bin/tail", @[@"-n", @"80", kMacfwTransportLog], &status);
    [diagnostics appendString:@"\n\ntransport log (last 80 lines)\n-----------------------------\n"];
    [diagnostics appendString:(status == 0 && logTail.length) ? logTail : @"unavailable"];

    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:diagnostics forType:NSPasteboardTypeString];

    NSString *oldTitle = sender.title;
    sender.title = @"Copied";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.2 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        sender.title = oldTitle;
    });
}

- (void)macfwOpenTransportLog:(id)sender {
    (void)sender;
    if (![[NSFileManager defaultManager] fileExistsAtPath:kMacfwTransportLog]) {
        NSBeep();
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:kMacfwTransportLog]];
}

@end
