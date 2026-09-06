#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "slider_commit.h"
#include <cmath>

static NSString *const kMacfwSliderControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";

@interface AppDelegate : NSObject
- (void)volumeSliderMoved:(NSSlider *)sender;
- (void)auxStreamSliderMoved:(NSSlider *)sender;
- (void)auxOutputSliderMoved:(NSSlider *)sender;
- (void)commitStereoControl:(NSString *)control left:(NSSlider *)left right:(NSSlider *)right;
@end

static void MacfwQueueStereo(id self, NSString *control, NSString *leftKey, NSString *rightKey) {
    NSSlider *left = [self valueForKey:leftKey];
    NSSlider *right = [self valueForKey:rightKey];
    if (!left || !right) return;
    MacfwQueueControlWrite(kMacfwSliderControlTool,
                           control,
                           @[control, @"set",
                             [NSString stringWithFormat:@"%d", (int)llround(left.doubleValue)],
                             [NSString stringWithFormat:@"%d", (int)llround(right.doubleValue)]]);
}

@implementation AppDelegate (MacfwAsyncSliders)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;
    struct { SEL original; SEL replacement; } swaps[] = {
        {@selector(volumeSliderMoved:), @selector(macfw_liveVolumeSliderMoved:)},
        {@selector(auxStreamSliderMoved:), @selector(macfw_liveAuxStreamSliderMoved:)},
        {@selector(auxOutputSliderMoved:), @selector(macfw_liveAuxOutputSliderMoved:)},
        {@selector(commitStereoControl:left:right:), @selector(macfw_commitStereoControl:left:right:)},
    };
    for (const auto &swap : swaps) {
        Method original = class_getInstanceMethod(cls, swap.original);
        Method replacement = class_getInstanceMethod(cls, swap.replacement);
        if (original && replacement) method_exchangeImplementations(original, replacement);
    }
}

- (void)macfw_liveVolumeSliderMoved:(NSSlider *)sender {
    [self macfw_liveVolumeSliderMoved:sender];
    MacfwQueueStereo(self, @"headphone-volume", @"leftSlider", @"rightSlider");
}

- (void)macfw_liveAuxStreamSliderMoved:(NSSlider *)sender {
    [self macfw_liveAuxStreamSliderMoved:sender];
    MacfwQueueStereo(self, @"aux-stream12-volume", @"auxStreamLeftSlider", @"auxStreamRightSlider");
}

- (void)macfw_liveAuxOutputSliderMoved:(NSSlider *)sender {
    [self macfw_liveAuxOutputSliderMoved:sender];
    MacfwQueueStereo(self, @"aux-output-volume", @"auxOutputLeftSlider", @"auxOutputRightSlider");
}

- (void)macfw_commitStereoControl:(NSString *)control
                             left:(NSSlider *)left
                            right:(NSSlider *)right {
    MacfwQueueControlWrite(kMacfwSliderControlTool,
                           control,
                           @[control, @"set",
                             [NSString stringWithFormat:@"%d", (int)llround(left.doubleValue)],
                             [NSString stringWithFormat:@"%d", (int)llround(right.doubleValue)]]);
}

@end
