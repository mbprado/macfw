#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "stereo_link.h"
#include <cmath>

static NSString *const kMacfwControlTool = @"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl";
static NSArray<NSString *> *OutputArgs(void) { return @[@"1/2", @"3/4", @"5/6", @"7/8", @"spdif"]; }
static NSArray<NSString *> *OutputNames(void) { return @[@"Analog 1/2", @"Analog 3/4", @"Analog 5/6", @"Analog 7/8", @"S/PDIF L/R"]; }

static NSString *RunControl(NSArray<NSString *> *args, int *statusOut) {
    NSTask *task = [[NSTask alloc] init];
    NSPipe *pipe = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:kMacfwControlTool];
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

static double ParseDbLine(NSString *line) {
    if ([line containsString:@"-inf"] || [line containsString:@"−∞"]) return -128;
    NSRange colon = [line rangeOfString:@":"];
    NSString *tail = colon.location == NSNotFound ? line : [line substringFromIndex:colon.location + 1];
    NSScanner *scanner = [NSScanner scannerWithString:tail];
    double value = 0;
    return [scanner scanDouble:&value] ? value : 0;
}

static NSString *DbText(double value) {
    return value <= -128 ? @"−∞" : [NSString stringWithFormat:@"%.0f dB", round(value)];
}

@interface AppDelegate : NSObject
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)refresh:(id)sender;
- (void)volumeSliderMoved:(NSSlider *)sender;
@end

static const void *kOutputSourcesKey = &kOutputSourcesKey;
static const void *kOutputLeftKey = &kOutputLeftKey;
static const void *kOutputRightKey = &kOutputRightKey;
static const void *kOutputLeftValueKey = &kOutputLeftValueKey;
static const void *kOutputRightValueKey = &kOutputRightValueKey;
static const void *kOutputLinksKey = &kOutputLinksKey;
static const void *kHeadphoneLinkKey = &kHeadphoneLinkKey;
static const void *kPreviousSliderKey = &kPreviousSliderKey;

@implementation AppDelegate (MacfwOutputs)

+ (void)load {
    Class cls = NSClassFromString(@"AppDelegate");
    if (!cls) return;
    Method original = class_getInstanceMethod(cls, @selector(applicationDidFinishLaunching:));
    Method replacement = class_getInstanceMethod(cls, @selector(macfw_applicationDidFinishLaunching:));
    method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(refresh:));
    replacement = class_getInstanceMethod(cls, @selector(macfw_refresh:));
    method_exchangeImplementations(original, replacement);

    original = class_getInstanceMethod(cls, @selector(volumeSliderMoved:));
    replacement = class_getInstanceMethod(cls, @selector(macfw_volumeSliderMoved:));
    method_exchangeImplementations(original, replacement);
}

- (void)macfw_applicationDidFinishLaunching:(NSNotification *)notification {
    [self macfw_applicationDidFinishLaunching:notification];
    [self macfwBuildOutputsTab];
    [self macfwAddHeadphoneLink];
    [self macfwRefreshOutputs];
}

- (void)macfw_refresh:(id)sender {
    [self macfw_refresh:sender];
    [self macfwRefreshOutputs];
}

- (void)macfwAddHeadphoneLink {
    NSTabView *tabs = [self valueForKey:@"tabView"];
    NSTabViewItem *headphones = nil;
    for (NSTabViewItem *item in tabs.tabViewItems)
        if ([item.identifier isEqual:@"headphones"]) { headphones = item; break; }
    if (!headphones) return;
    NSButton *link = MacfwMakeLinkButton(self, @selector(macfwHeadphoneLinkChanged:), NSMakeRect(490, 276, 80, 24));
    [headphones.view addSubview:link];
    objc_setAssociatedObject(self, kHeadphoneLinkKey, link, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (void)macfwHeadphoneLinkChanged:(NSButton *)sender { (void)sender; }

- (void)macfw_volumeSliderMoved:(NSSlider *)sender {
    NSButton *link = objc_getAssociatedObject(self, kHeadphoneLinkKey);
    NSSlider *left = [self valueForKey:@"leftSlider"];
    NSSlider *right = [self valueForKey:@"rightSlider"];
    if (link.state == NSControlStateValueOn && (sender == left || sender == right)) {
        NSNumber *old = objc_getAssociatedObject(sender, kPreviousSliderKey);
        double previous = old ? old.doubleValue : sender.doubleValue;
        MacfwApplyStereoLink(sender, sender == left ? right : left, YES, previous);
    }
    objc_setAssociatedObject(sender, kPreviousSliderKey, @(sender.doubleValue), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self macfw_volumeSliderMoved:sender];
}

- (void)macfwBuildOutputsTab {
    NSTabView *tabs = [self valueForKey:@"tabView"];
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"outputs"];
    item.label = @"Outputs";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 440)];
    item.view = view;
    [tabs insertTabViewItem:item atIndex:0];

    NSTextField *hint = [NSTextField labelWithString:@"Physical output routing and level. Link preserves the current L/R difference."];
    hint.textColor = NSColor.secondaryLabelColor;
    hint.frame = NSMakeRect(20, 405, 550, 20);
    [view addSubview:hint];

    NSMutableArray *sources = [NSMutableArray array];
    NSMutableArray *lefts = [NSMutableArray array];
    NSMutableArray *rights = [NSMutableArray array];
    NSMutableArray *leftValues = [NSMutableArray array];
    NSMutableArray *rightValues = [NSMutableArray array];
    NSMutableArray *links = [NSMutableArray array];

    for (NSInteger i = 0; i < 5; ++i) {
        CGFloat y = 330 - i * 76;
        NSTextField *name = [NSTextField labelWithString:OutputNames()[(NSUInteger)i]];
        name.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
        name.frame = NSMakeRect(20, y + 35, 100, 20);
        [view addSubview:name];

        NSSegmentedControl *source = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(125, y + 31, 125, 25)];
        source.segmentCount = 2; [source setLabel:@"Mixer" forSegment:0]; [source setLabel:@"AUX" forSegment:1];
        source.tag = i; source.target = self; source.action = @selector(macfwOutputSourceChanged:);
        [view addSubview:source]; [sources addObject:source];

        NSButton *link = MacfwMakeLinkButton(self, @selector(macfwOutputLinkChanged:), NSMakeRect(260, y + 31, 65, 25));
        link.tag = i; [view addSubview:link]; [links addObject:link];

        NSTextField *ll = [NSTextField labelWithString:@"L"];
        ll.frame = NSMakeRect(20, y + 7, 15, 18); [view addSubview:ll];
        NSSlider *left = [[NSSlider alloc] initWithFrame:NSMakeRect(40, y + 4, 205, 20)];
        left.minValue=-128; left.maxValue=0; left.continuous=YES; left.tag=i*2; left.target=self; left.action=@selector(macfwOutputSliderMoved:);
        [view addSubview:left]; [lefts addObject:left];
        NSTextField *lv=[NSTextField labelWithString:@"0 dB"]; lv.alignment=NSTextAlignmentRight; lv.frame=NSMakeRect(248,y+6,58,18); [view addSubview:lv]; [leftValues addObject:lv];

        NSTextField *rl = [NSTextField labelWithString:@"R"];
        rl.frame = NSMakeRect(320, y + 7, 15, 18); [view addSubview:rl];
        NSSlider *right = [[NSSlider alloc] initWithFrame:NSMakeRect(340, y + 4, 165, 20)];
        right.minValue=-128; right.maxValue=0; right.continuous=YES; right.tag=i*2+1; right.target=self; right.action=@selector(macfwOutputSliderMoved:);
        [view addSubview:right]; [rights addObject:right];
        NSTextField *rv=[NSTextField labelWithString:@"0 dB"]; rv.alignment=NSTextAlignmentRight; rv.frame=NSMakeRect(508,y+6,58,18); [view addSubview:rv]; [rightValues addObject:rv];
    }

    objc_setAssociatedObject(self,kOutputSourcesKey,sources,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self,kOutputLeftKey,lefts,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self,kOutputRightKey,rights,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self,kOutputLeftValueKey,leftValues,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self,kOutputRightValueKey,rightValues,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(self,kOutputLinksKey,links,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (BOOL)macfwIsDragging { return NSApp.currentEvent && NSApp.currentEvent.type == NSEventTypeLeftMouseDragged; }
- (void)macfwOutputLinkChanged:(NSButton *)sender { (void)sender; }

- (void)macfwOutputSourceChanged:(NSSegmentedControl *)sender {
    NSString *source = sender.selectedSegment == 1 ? @"aux" : @"mixer";
    int status=0; RunControl(@[@"output-source",@"set",OutputArgs()[(NSUInteger)sender.tag],source],&status);
    [self macfwRefreshOutputs];
}

- (void)macfwOutputSliderMoved:(NSSlider *)sender {
    NSInteger pair=sender.tag/2; BOOL isRight=(sender.tag%2)!=0;
    NSArray<NSSlider *> *lefts=objc_getAssociatedObject(self,kOutputLeftKey), *rights=objc_getAssociatedObject(self,kOutputRightKey);
    NSArray<NSTextField *> *lvs=objc_getAssociatedObject(self,kOutputLeftValueKey), *rvs=objc_getAssociatedObject(self,kOutputRightValueKey);
    NSArray<NSButton *> *links=objc_getAssociatedObject(self,kOutputLinksKey);
    NSSlider *left=lefts[(NSUInteger)pair], *right=rights[(NSUInteger)pair];
    NSNumber *old=objc_getAssociatedObject(sender,kPreviousSliderKey);
    double previous=old?old.doubleValue:sender.doubleValue;
    if(links[(NSUInteger)pair].state==NSControlStateValueOn)
        MacfwApplyStereoLink(sender,isRight?left:right,YES,previous);
    objc_setAssociatedObject(sender,kPreviousSliderKey,@(sender.doubleValue),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    lvs[(NSUInteger)pair].stringValue=DbText(left.doubleValue); rvs[(NSUInteger)pair].stringValue=DbText(right.doubleValue);
    if(![self macfwIsDragging]){
        int status=0;
        RunControl(@[@"output-volume",@"set",OutputArgs()[(NSUInteger)pair],
                     [NSString stringWithFormat:@"%d",(int)llround(left.doubleValue)],
                     [NSString stringWithFormat:@"%d",(int)llround(right.doubleValue)]],&status);
        [self macfwRefreshOutputs];
    }
}

- (void)macfwRefreshOutputs {
    NSArray<NSSegmentedControl *> *sources=objc_getAssociatedObject(self,kOutputSourcesKey);
    NSArray<NSSlider *> *lefts=objc_getAssociatedObject(self,kOutputLeftKey), *rights=objc_getAssociatedObject(self,kOutputRightKey);
    NSArray<NSTextField *> *lvs=objc_getAssociatedObject(self,kOutputLeftValueKey), *rvs=objc_getAssociatedObject(self,kOutputRightValueKey);
    if(!sources) return;
    for(NSUInteger i=0;i<5;++i){
        int status=0; NSString *s=RunControl(@[@"output-source",@"get",OutputArgs()[i]],&status);
        if(status==0) sources[i].selectedSegment=[s containsString:@"aux (1)"]?1:0;
        NSString *v=RunControl(@[@"output-volume",@"get",OutputArgs()[i]],&status);
        if(status==0){
            for(NSString *line in [v componentsSeparatedByString:@"\n"]){
                if([line hasPrefix:@"left:"]){lefts[i].doubleValue=ParseDbLine(line);lvs[i].stringValue=DbText(lefts[i].doubleValue);objc_setAssociatedObject(lefts[i],kPreviousSliderKey,@(lefts[i].doubleValue),OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
                else if([line hasPrefix:@"right:"]){rights[i].doubleValue=ParseDbLine(line);rvs[i].stringValue=DbText(rights[i].doubleValue);objc_setAssociatedObject(rights[i],kPreviousSliderKey,@(rights[i].doubleValue),OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
            }
        }
    }
}
@end
