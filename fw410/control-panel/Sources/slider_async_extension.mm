#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "slider_commit.h"
#include <cmath>

static NSString *const kMacfwSliderControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";

@interface AppDelegate : NSObject
- (void)commitStereoControl:(NSString *)control left:(NSSlider *)left right:(NSSlider *)right;
@end

@implementation AppDelegate (MacfwAsyncSliders)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;
    Method original = class_getInstanceMethod(cls, @selector(commitStereoControl:left:right:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfw_commitStereoControl:left:right:));
    if (original && replacement) method_exchangeImplementations(original, replacement);
}

- (void)macfw_commitStereoControl:(NSString *)control
                             left:(NSSlider *)left
                            right:(NSSlider *)right {
    const int leftDb = (int)llround(left.doubleValue);
    const int rightDb = (int)llround(right.doubleValue);
    MacfwQueueControlWrite(kMacfwSliderControlTool,
                           control,
                           @[control, @"set",
                             [NSString stringWithFormat:@"%d", leftDb],
                             [NSString stringWithFormat:@"%d", rightDb]]);
}

@end
