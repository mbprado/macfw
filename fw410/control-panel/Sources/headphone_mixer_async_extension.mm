#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "slider_commit.h"

static NSString *const kMacfwHeadphoneControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";

@interface AppDelegate : NSObject
- (void)mixerChanged:(NSButton *)sender;
- (void)sourceChanged:(NSSegmentedControl *)sender;
@end

@implementation AppDelegate (MacfwAsyncHeadphoneMixer)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;

    Method original = class_getInstanceMethod(cls, @selector(mixerChanged:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfw_asyncMixerChanged:));
    if (original && replacement) method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(sourceChanged:));
    replacement = class_getInstanceMethod(cls, @selector(macfw_asyncSourceChanged:));
    if (original && replacement) method_exchangeImplementations(original, replacement);
}

- (void)macfw_asyncMixerChanged:(NSButton *)sender {
    static NSArray<NSString *> *labels;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        labels = @[@"1/2", @"3/4", @"5/6", @"7/8", @"9/10"];
    });

    if (sender.tag < 0 || sender.tag >= (NSInteger)labels.count) return;
    NSString *label = labels[(NSUInteger)sender.tag];
    NSString *state = sender.state == NSControlStateValueOn ? @"on" : @"off";
    NSString *key = [@"headphone-mixer:" stringByAppendingString:label];

    // Keep the checkbox responsive immediately. The CLI process runs away from
    // AppKit's event loop and newer clicks for the same route replace any queued
    // intermediate state. A normal Refresh still re-reads the verified device
    // state if the user wants to reconcile after a transport error.
    MacfwQueueControlWrite(kMacfwHeadphoneControlTool,
                           key,
                           @[@"headphone-mixer", @"set", label, state]);
}

- (void)macfw_asyncSourceChanged:(NSSegmentedControl *)sender {
    NSString *source = sender.selectedSegment == 1 ? @"aux" : @"mixer";

    // Source selection is a discrete final state just like a mixer-route click.
    // Keep the segmented control responsive immediately, run fw410ctl off the
    // AppKit thread, and coalesce rapid Mixer/Auxiliary changes to the last one.
    // Do not trigger a full control-panel refresh after every selection.
    MacfwQueueControlWrite(kMacfwHeadphoneControlTool,
                           @"headphone-source",
                           @[@"headphone-source", @"set", source]);
}

@end
