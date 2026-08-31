#pragma once

#import <Foundation/Foundation.h>

static NSString *const kMacfwStateTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state";

static inline void MacfwPersistControlState(NSString *key, NSArray<NSString *> *controlArgs) {
    if (key.length == 0 || controlArgs.count == 0) return;
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:kMacfwStateTool]) return;

    NSMutableArray<NSString *> *args = [NSMutableArray arrayWithObjects:@"record", key, nil];
    [args addObjectsFromArray:controlArgs];

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:kMacfwStateTool];
    task.arguments = args;
    task.standardOutput = [NSPipe pipe];
    task.standardError = [NSPipe pipe];

    NSError *error = nil;
    if (![task launchAndReturnError:&error]) return;
    [task waitUntilExit];
}
