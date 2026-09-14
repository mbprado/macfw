#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

@interface AppDelegate : NSObject
- (void)macfwRefreshDevice;
@end

@implementation AppDelegate (MacfwDeviceLatencyDisplay)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;

    Method original = class_getInstanceMethod(cls, @selector(macfwRefreshDevice));
    Method replacement = class_getInstanceMethod(cls, @selector(macfwLatencyDisplay_macfwRefreshDevice));
    if (original && replacement)
        method_exchangeImplementations(original, replacement);
}

- (void)macfwLatencyDisplay_macfwRefreshDevice {
    [self macfwLatencyDisplay_macfwRefreshDevice];

    NSTabView *tabs = nil;
    @try { tabs = [self valueForKey:@"tabView"]; }
    @catch (NSException *exception) { (void)exception; return; }
    if (!tabs) return;

    NSTabViewItem *deviceItem = nil;
    for (NSTabViewItem *item in tabs.tabViewItems) {
        if ([item.identifier isEqual:@"device"]) {
            deviceItem = item;
            break;
        }
    }
    if (!deviceItem.view) return;

    for (NSView *subview in deviceItem.view.subviews) {
        if (![subview isKindOfClass:[NSTextField class]]) continue;
        NSTextField *field = (NSTextField *)subview;

        if ([field.stringValue isEqualToString:@"0 frames (0.00 ms)"] ||
            [field.stringValue isEqualToString:@"out 0 / in 0 frames"]) {
            field.stringValue = @"Not reported by HAL";
            continue;
        }

        if ([field.stringValue isEqualToString:
             @"Latency values are currently HAL-reported diagnostics, not calibrated end-to-end measurements."]) {
            field.stringValue = @"Latency/safety values are shown only when the HAL reports a non-zero value; end-to-end latency is not yet calibrated.";
        }
    }
}

@end
