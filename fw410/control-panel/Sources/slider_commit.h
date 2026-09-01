#pragma once
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

// Slider writes never block AppKit's event loop. Each logical control has at
// most one fw410ctl process in flight. New values replace the pending value,
// so a fast drag cannot build an unbounded process queue and the final value
// is always drained after the current write completes.
static dispatch_queue_t MacfwSliderStateQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("com.mbprado.macfw.slider-state", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static NSMutableDictionary<NSString *, NSArray<NSString *> *> *MacfwSliderPending(void) {
    static NSMutableDictionary<NSString *, NSArray<NSString *> *> *values;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ values = [NSMutableDictionary dictionary]; });
    return values;
}

static NSMutableSet<NSString *> *MacfwSliderRunning(void) {
    static NSMutableSet<NSString *> *values;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ values = [NSMutableSet set]; });
    return values;
}

static void MacfwDrainControlWrite(NSString *tool, NSString *key);

static void MacfwDrainControlWrite(NSString *tool, NSString *key) {
    dispatch_async(MacfwSliderStateQueue(), ^{
        NSArray<NSString *> *next = MacfwSliderPending()[key];
        if (!next) {
            [MacfwSliderRunning() removeObject:key];
            return;
        }
        [MacfwSliderPending() removeObjectForKey:key];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                NSTask *task = [[NSTask alloc] init];
                task.executableURL = [NSURL fileURLWithPath:tool];
                task.arguments = next;
                task.standardOutput = [NSPipe pipe];
                task.standardError = [NSPipe pipe];
                NSError *error = nil;
                if ([task launchAndReturnError:&error]) [task waitUntilExit];
            }
            MacfwDrainControlWrite(tool, key);
        });
    });
}

static inline void MacfwQueueControlWrite(NSString *tool,
                                          NSString *key,
                                          NSArray<NSString *> *args) {
    dispatch_async(MacfwSliderStateQueue(), ^{
        MacfwSliderPending()[key] = [args copy];
        if (![MacfwSliderRunning() containsObject:key]) {
            [MacfwSliderRunning() addObject:key];
            MacfwDrainControlWrite(tool, key);
        }
    });
}
