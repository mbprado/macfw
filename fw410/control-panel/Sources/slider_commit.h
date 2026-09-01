#pragma once
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

// Slider writes must never block AppKit's event loop.  Each logical control owns
// one serial worker.  While a write is in flight, newer values replace the
// pending value so rapid drags cannot build an unbounded queue of fw410ctl
// processes.  The worker always drains the newest pending value before idling.
static inline void MacfwQueueControlWrite(NSString *tool,
                                          NSString *key,
                                          NSArray<NSString *> *args) {
    static dispatch_queue_t stateQueue;
    static NSMutableDictionary<NSString *, NSArray<NSString *> *> *pending;
    static NSMutableSet<NSString *> *running;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        stateQueue = dispatch_queue_create("com.mbprado.macfw.slider-state", DISPATCH_QUEUE_SERIAL);
        pending = [NSMutableDictionary dictionary];
        running = [NSMutableSet set];
    });

    void (^__block drain)(void) = nil;
    drain = ^{
        dispatch_async(stateQueue, ^{
            NSArray<NSString *> *next = pending[key];
            if (!next) {
                [running removeObject:key];
                return;
            }
            [pending removeObjectForKey:key];
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
                drain();
            });
        });
    };

    dispatch_async(stateQueue, ^{
        pending[key] = [args copy];
        if (![running containsObject:key]) {
            [running addObject:key];
            drain();
        }
    });
}
