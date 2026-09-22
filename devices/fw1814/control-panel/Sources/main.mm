#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#include "slider_commit.h"
#include "../../version.h"
#include <cmath>
#include <cstdint>

static NSString *const kCtl=@"/Library/Application Support/macfw/fw1814/bin/fw1814ctl";
static NSString *const kState=@"/Library/Application Support/macfw/fw1814/bin/fw1814state";
static NSString *const kLog=@"/Library/Logs/macfw-fw1814-transport.log";
static CFStringRef const kUID=CFSTR("com.mbprado.macfw.fw1814.device");

static NSArray<NSString*> *SW(void){return @[@"sw1/2",@"sw3/4"];}
static NSArray<NSString*> *ANA(void){return @[@"analog1/2",@"analog3/4",@"analog5/6",@"analog7/8"];}
static NSArray<NSString*> *ALL(void){return @[@"sw1/2",@"sw3/4",@"analog1/2",@"analog3/4",@"analog5/6",@"analog7/8"];}
static NSArray<NSString*> *PAIR(void){return @[@"1/2",@"3/4"];}
static NSArray<NSString*> *HP(void){return @[@"1",@"2"];}
static NSArray<NSString*> *TITLE(void){return @[@"SW Return 1/2",@"SW Return 3/4",@"Analog Inputs 1/2",@"Analog Inputs 3/4",@"Analog Inputs 5/6",@"Analog Inputs 7/8"];}
static const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
static NSArray<NSString*> *RATE_LABELS(void){return @[@"44.1 kHz", @"48 kHz", @"88.2 kHz", @"96 kHz", @"176.4 kHz", @"192 kHz"];}

static NSDictionary *Run(NSString *path, NSArray<NSString*> *args){
    if(![[NSFileManager defaultManager] isExecutableFileAtPath:path])
        return @{@"status":@127,@"output":[NSString stringWithFormat:@"Missing executable: %@",path]};
    NSTask *task=[NSTask new]; NSPipe *pipe=[NSPipe pipe];
    task.executableURL=[NSURL fileURLWithPath:path]; task.arguments=args;
    task.standardOutput=pipe; task.standardError=pipe;
    NSError *error=nil;
    if(![task launchAndReturnError:&error])
        return @{@"status":@126,@"output":error.localizedDescription?:@"Launch failed"};
    [task waitUntilExit];
    NSData *data=[[pipe fileHandleForReading] readDataToEndOfFile];
    NSString *out=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return @{@"status":@(task.terminationStatus),@"output":out?:@""};
}
static NSTextField *Lbl(NSString *s,NSRect r){NSTextField *x=[NSTextField labelWithString:s];x.frame=r;return x;}
static NSTextField *Hdr(NSString *s,NSRect r){NSTextField *x=Lbl(s,r);x.font=[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];return x;}
static NSArray<NSNumber*> *RawLevels(NSString *s){
    NSRegularExpression *re=[NSRegularExpression regularExpressionWithPattern:@"raw\\s+(-?\\d+)" options:0 error:nil];
    NSMutableArray *a=[NSMutableArray array];
    for(NSTextCheckingResult *m in [re matchesInString:s options:0 range:NSMakeRange(0,s.length)])
        if(m.numberOfRanges>1)[a addObject:@([[s substringWithRange:[m rangeAtIndex:1]] integerValue])];
    return a;
}
static BOOL RawPan(NSString *s,int16_t *l,int16_t *r){
    NSRegularExpression *re=[NSRegularExpression regularExpressionWithPattern:@"0x([0-9a-fA-F]{8})" options:0 error:nil];
    NSTextCheckingResult *m=[re firstMatchInString:s options:0 range:NSMakeRange(0,s.length)];
    if(!m||m.numberOfRanges<2)return NO;
    unsigned int w=0; if(![[NSScanner scannerWithString:[s substringWithRange:[m rangeAtIndex:1]]] scanHexInt:&w])return NO;
    *l=(int16_t)((w>>16)&0xffffu); *r=(int16_t)(w&0xffffu); return YES;
}
static AudioObjectID FindDevice(void){
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,nullptr,&size)!=noErr)return kAudioObjectUnknown;
    NSMutableData *d=[NSMutableData dataWithLength:size]; auto *dev=static_cast<AudioObjectID*>(d.mutableBytes);
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&size,dev)!=noErr)return kAudioObjectUnknown;
    AudioObjectPropertyAddress u{kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    for(UInt32 i=0;i<size/sizeof(AudioObjectID);++i){CFStringRef uid=nullptr;UInt32 n=sizeof(uid);
        if(AudioObjectGetPropertyData(dev[i],&u,0,nullptr,&n,&uid)!=noErr||!uid)continue;
        BOOL match=CFEqual(uid,kUID);CFRelease(uid);if(match)return dev[i];}
    return kAudioObjectUnknown;
}

@interface AppDelegate:NSObject<NSApplicationDelegate>
@property(nonatomic,strong) NSWindow *window;
@property(nonatomic,strong) NSTabView *tabs;
@property(nonatomic,strong) NSTextField *status;
@property(nonatomic,strong) NSTextView *diagnostics;
@property(nonatomic,strong) NSSegmentedControl *rate;
@property(nonatomic,strong) NSTextField *deviceStatus;
@property(nonatomic,strong) NSTextField *auxNote;
@property(nonatomic,assign) BOOL refreshing;
@property(nonatomic,strong) NSMutableArray<NSButton*> *routes;
@property(nonatomic,strong) NSMutableArray<NSPopUpButton*> *outputSources;
@property(nonatomic,strong) NSMutableArray<NSPopUpButton*> *hpSources;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *swRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *inputRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *outputRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *hpRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *auxRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *panRows;
@property(nonatomic,strong) NSMutableArray<NSDictionary*> *auxMasterRows;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)n{
    (void)n; self.routes=[NSMutableArray array];self.outputSources=[NSMutableArray array];self.hpSources=[NSMutableArray array];
    self.swRows=[NSMutableArray array];self.inputRows=[NSMutableArray array];self.outputRows=[NSMutableArray array];
    self.hpRows=[NSMutableArray array];self.auxRows=[NSMutableArray array];self.panRows=[NSMutableArray array];self.auxMasterRows=[NSMutableArray array];
    NSMenu *main=[NSMenu new],*appMenu=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[main addItem:appItem];
    [appMenu addItemWithTitle:@"About macfw FW1814 Control" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];[appMenu addItemWithTitle:@"Quit macfw FW1814 Control" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu=appMenu;NSApp.mainMenu=main;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,720)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title=@"macfw FW1814 Control";self.window.minSize=NSMakeSize(900,620);[self.window center];
    NSView *v=self.window.contentView;self.status=Lbl(@"Connecting to FW1814 transport…",NSMakeRect(20,684,690,22));
    self.status.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];[v addSubview:self.status];
    NSButton *refresh=[NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refresh:)];refresh.frame=NSMakeRect(785,680,86,28);[v addSubview:refresh];
    NSButton *reset=[NSButton buttonWithTitle:@"Reset Defaults" target:self action:@selector(resetDefaults:)];reset.frame=NSMakeRect(878,680,108,28);[v addSubview:reset];
    self.tabs=[[NSTabView alloc] initWithFrame:NSMakeRect(14,14,972,658)];[v addSubview:self.tabs];
    [self buildMixer];[self buildOutputs];[self buildHeadphones];[self buildAux];[self buildDevice];[self buildDiagnostics];
    [self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
    dispatch_async(dispatch_get_main_queue(),^{[self refresh:nil];});
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer){
        (void)timer;
        if (!self.window.isVisible || self.refreshing || [NSEvent pressedMouseButtons]) return;
        [self refreshLevels:self.hpRows command:@"headphone-volume" args:HP()];
    }];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)s{(void)s;return YES;}
- (NSView*)tab:(NSString*)label id:(NSString*)ident{
    NSTabViewItem *item=[[NSTabViewItem alloc] initWithIdentifier:ident];item.label=label;
    NSView *v=[[NSView alloc] initWithFrame:NSMakeRect(0,0,950,620)];item.view=v;[self.tabs addTabViewItem:item];return v;
}
- (NSView*)scroll:(NSView*)tab height:(CGFloat)h{
    NSScrollView *s=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,950,620)];s.hasVerticalScroller=YES;s.autohidesScrollers=YES;
    NSView *d=[[NSView alloc] initWithFrame:NSMakeRect(0,0,930,h)];s.documentView=d;[tab addSubview:s];return d;
}
- (NSDictionary*)stereo:(NSView*)v y:(CGFloat)y title:(NSString*)title index:(NSInteger)i
                  action:(SEL)a mute:(SEL)ma store:(NSMutableArray*)store{
    [v addSubview:Hdr(title,NSMakeRect(18,y+35,140,22))];
    [v addSubview:Lbl(@"L",NSMakeRect(165,y+39,15,18))];
    NSSlider *l=[NSSlider sliderWithValue:0 minValue:-127 maxValue:0 target:self action:a];l.frame=NSMakeRect(184,y+32,220,28);l.continuous=YES;l.tag=i*2;[v addSubview:l];
    NSTextField *lv=Lbl(@"0 dB",NSMakeRect(408,y+38,70,18));lv.font=[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];[v addSubview:lv];
    [v addSubview:Lbl(@"R",NSMakeRect(480,y+39,15,18))];
    NSSlider *r=[NSSlider sliderWithValue:0 minValue:-127 maxValue:0 target:self action:a];r.frame=NSMakeRect(500,y+32,220,28);r.continuous=YES;r.tag=i*2+1;[v addSubview:r];
    NSTextField *rv=Lbl(@"0 dB",NSMakeRect(724,y+38,70,18));rv.font=[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];[v addSubview:rv];
    NSButton *link=[NSButton checkboxWithTitle:@"Link" target:nil action:nil];link.frame=NSMakeRect(795,y+34,58,24);link.state=NSControlStateValueOn;[v addSubview:link];
    NSButton *mute=[NSButton checkboxWithTitle:@"Mute" target:self action:ma];mute.frame=NSMakeRect(858,y+34,65,24);mute.tag=i;[v addSubview:mute];
    NSDictionary *row=@{@"left":l,@"right":r,@"leftValue":lv,@"rightValue":rv,@"link":link,@"mute":mute};[store addObject:row];return row;
}
- (void)routeButtons:(NSView*)v y:(CGFloat)y source:(NSInteger)i{
    for(NSInteger b=0;b<2;++b){NSButton *x=[NSButton checkboxWithTitle:[NSString stringWithFormat:@"Mix %@",PAIR()[b]] target:self action:@selector(routeChanged:)];
        x.frame=NSMakeRect(18+b*80,y,78,22);x.tag=i*2+b;[v addSubview:x];[self.routes addObject:x];}
}
- (void)panRow:(NSView*)v y:(CGFloat)y index:(NSInteger)i{
    [v addSubview:Lbl(@"Monitor pan",NSMakeRect(184,y+4,90,18))];
    NSSlider *l=[NSSlider sliderWithValue:-100 minValue:-100 maxValue:100 target:self action:@selector(panChanged:)];l.frame=NSMakeRect(275,y-2,185,26);l.continuous=YES;l.tag=i*2;[v addSubview:l];
    NSTextField *lv=Lbl(@"L 100",NSMakeRect(462,y+4,55,18));lv.font=[NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular];[v addSubview:lv];
    NSSlider *r=[NSSlider sliderWithValue:100 minValue:-100 maxValue:100 target:self action:@selector(panChanged:)];r.frame=NSMakeRect(520,y-2,185,26);r.continuous=YES;r.tag=i*2+1;[v addSubview:r];
    NSTextField *rv=Lbl(@"R 100",NSMakeRect(708,y+4,55,18));rv.font=[NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular];[v addSubview:rv];
    [self.panRows addObject:@{@"left":l,@"right":r,@"leftValue":lv,@"rightValue":rv}];
}
- (void)buildMixer{
    NSView *v=[self scroll:[self tab:@"Mixer" id:@"mixer"] height:830];
    [v addSubview:Lbl(@"Route each source to either monitor mix. Hardware changes are saved automatically.",NSMakeRect(18,794,890,20))];
    CGFloat y=700;
    for(NSInteger i=0;i<2;++i){[self stereo:v y:y title:TITLE()[i] index:i action:@selector(swLevel:) mute:@selector(swMute:) store:self.swRows];[self routeButtons:v y:y+8 source:i];y-=90;}
    for(NSInteger i=0;i<4;++i){[self stereo:v y:y title:TITLE()[i+2] index:i action:@selector(inputLevel:) mute:@selector(inputMute:) store:self.inputRows];
        [self routeButtons:v y:y+8 source:i+2];[self panRow:v y:y-18 index:i];y-=125;}
}
- (void)buildOutputs{
    NSView *v=[self tab:@"Outputs" id:@"outputs"];[v addSubview:Lbl(@"Analog output source and hardware output level.",NSMakeRect(22,574,700,20))];CGFloat y=440;
    for(NSInteger i=0;i<2;++i){[self stereo:v y:y title:[NSString stringWithFormat:@"Analog Outputs %@",PAIR()[i]] index:i action:@selector(outputLevel:) mute:@selector(outputMute:) store:self.outputRows];
        NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(18,y+3,130,27) pullsDown:NO];[p addItemsWithTitles:@[@"Mixer",@"AUX"]];p.tag=i;p.target=self;p.action=@selector(outputSource:);[v addSubview:p];[self.outputSources addObject:p];y-=115;}
}
- (void)buildHeadphones{
    NSView *v=[self tab:@"Headphones" id:@"headphones"];[v addSubview:Lbl(@"The FW1814 headphone encoders are digital; source and volume are stored in hardware state.",NSMakeRect(22,574,850,20))];CGFloat y=440;
    for(NSInteger i=0;i<2;++i){[self stereo:v y:y title:[NSString stringWithFormat:@"Headphone Output %@",HP()[i]] index:i action:@selector(hpLevel:) mute:@selector(hpMute:) store:self.hpRows];
        NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(18,y+3,130,27) pullsDown:NO];[p addItemsWithTitles:@[@"Mixer 1/2",@"Mixer 3/4",@"AUX"]];p.tag=i;p.target=self;p.action=@selector(hpSource:);[v addSubview:p];[self.hpSources addObject:p];y-=115;}
}
- (void)buildAux{
    NSView *v=[self scroll:[self tab:@"AUX" id:@"aux"] height:720];self.auxNote=Lbl(@"Independent stereo AUX sends. Select AUX on an output or headphone to hear this bus.",NSMakeRect(18,684,880,20));[v addSubview:self.auxNote];CGFloat y=590;
    for(NSInteger i=0;i<6;++i){[self stereo:v y:y title:[NSString stringWithFormat:@"%@ send",TITLE()[i]] index:i action:@selector(auxLevel:) mute:@selector(auxMute:) store:self.auxRows];y-=82;}
    [self stereo:v y:12 title:@"AUX master" index:0 action:@selector(auxMasterLevel:) mute:@selector(auxMasterMute:) store:self.auxMasterRows];
}
- (void)buildDevice{
    NSView *v=[self tab:@"Device" id:@"device"];[v addSubview:Hdr(@"M-Audio FireWire 1814",NSMakeRect(28,550,350,24))];
    [v addSubview:Lbl(@"Sample-rate changes use the normal CoreAudio device lifecycle.",NSMakeRect(28,520,650,20))];
    self.deviceStatus=Lbl(@"CoreAudio device: checking…",NSMakeRect(28,465,600,24));self.deviceStatus.font=[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];[v addSubview:self.deviceStatus];
    self.rate=[[NSSegmentedControl alloc] initWithFrame:NSMakeRect(28,410,620,30)];self.rate.segmentCount=6;NSArray *labels=RATE_LABELS();for(NSInteger i=0;i<6;++i)[self.rate setLabel:labels[i] forSegment:i];self.rate.target=self;self.rate.action=@selector(rateChanged:);[v addSubview:self.rate];
    NSTextField *note=Lbl(@"All six analog sample rates are available. Digital I/O, MIDI, and headphone-specific paths remain deferred.",NSMakeRect(28,342,850,42));note.maximumNumberOfLines=2;note.textColor=NSColor.secondaryLabelColor;[v addSubview:note];
}
- (void)buildDiagnostics{
    NSView *v=[self tab:@"Diagnostics" id:@"diagnostics"];
    NSButton *copy=[NSButton buttonWithTitle:@"Copy Diagnostics" target:self action:@selector(copyDiagnostics:)];copy.frame=NSMakeRect(22,570,130,28);[v addSubview:copy];
    NSButton *open=[NSButton buttonWithTitle:@"Open Transport Log" target:self action:@selector(openLog:)];open.frame=NSMakeRect(160,570,145,28);[v addSubview:open];
    NSScrollView *s=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,18,914,540)];s.hasVerticalScroller=YES;s.hasHorizontalScroller=YES;s.borderType=NSBezelBorder;
    self.diagnostics=[[NSTextView alloc] initWithFrame:s.bounds];self.diagnostics.editable=NO;self.diagnostics.selectable=YES;self.diagnostics.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];s.documentView=self.diagnostics;[v addSubview:s];
}
- (NSDictionary*)ctl:(NSArray*)a{return Run(kCtl,a);}
- (void)report:(NSDictionary*)r ok:(NSString*)ok{
    if([r[@"status"] integerValue]==0){self.status.stringValue=ok;self.status.textColor=NSColor.labelColor;return;}
    NSString *line=[[r[@"output"] componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] firstObject];
    self.status.stringValue=line.length?line:@"FW1814 command failed";self.status.textColor=NSColor.systemRedColor;NSBeep();
}
- (void)updateLevel:(NSDictionary*)row text:(NSString*)text{
    NSArray *raw=RawLevels(text);if(raw.count<2)return;NSInteger a=[raw[0] integerValue],b=[raw[1] integerValue];
    NSButton *mute=row[@"mute"];mute.state=(a==-32768&&b==-32768)?NSControlStateValueOn:NSControlStateValueOff;
    if(a!=-32768)[row[@"left"] setIntegerValue:a/256];if(b!=-32768)[row[@"right"] setIntegerValue:b/256];
    [row[@"leftValue"] setStringValue:a==-32768?@"−∞":[NSString stringWithFormat:@"%ld dB",(long)(a/256)]];
    [row[@"rightValue"] setStringValue:b==-32768?@"−∞":[NSString stringWithFormat:@"%ld dB",(long)(b/256)]];
}
- (void)refreshLevels:(NSMutableArray*)rows command:(NSString*)cmd args:(NSArray*)args{
    for(NSInteger i=0;i<(NSInteger)rows.count;++i){NSMutableArray *a=[NSMutableArray arrayWithObjects:cmd,@"get",nil];if(i<(NSInteger)args.count)[a addObject:args[i]];
        NSDictionary *r=[self ctl:a];if([r[@"status"] integerValue]==0)[self updateLevel:rows[i] text:r[@"output"]];}
}
- (void)refreshRoutes{
    for(NSInteger i=0;i<6;++i)for(NSInteger b=0;b<2;++b){NSArray *a=i<2?@[@"mixer-route",@"get",SW()[i],PAIR()[b]]:@[@"input-mixer-route",@"get",ANA()[i-2],PAIR()[b]];
        NSDictionary *r=[self ctl:a];if([r[@"status"] integerValue])continue;NSString *s=[r[@"output"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if(i>=2){
            NSRange newline=[s rangeOfString:@"\n"];
            if(newline.location!=NSNotFound)s=[s substringToIndex:newline.location];
        }
        self.routes[i*2+b].state=[s hasSuffix:@": on"]?NSControlStateValueOn:NSControlStateValueOff;}
}
- (void)refreshSources{
    for(NSInteger i=0;i<2;++i){NSDictionary *r=[self ctl:@[@"output-source",@"get",PAIR()[i]]];if(![r[@"status"] integerValue])[self.outputSources[i] selectItemAtIndex:[[r[@"output"] lowercaseString] containsString:@": aux"]?1:0];
        r=[self ctl:@[@"headphone-source",@"get",HP()[i]]];if(![r[@"status"] integerValue]){NSString *s=[r[@"output"] lowercaseString];[self.hpSources[i] selectItemAtIndex:[s containsString:@"aux"]?2:([s containsString:@"3/4"]?1:0)];}}
}
- (void)panLabel:(NSTextField*)l value:(NSInteger)v{l.stringValue=v==0?@"Center":(v<0?[NSString stringWithFormat:@"L %ld",(long)-v]:[NSString stringWithFormat:@"R %ld",(long)v]);}
- (void)refreshPans{
    for(NSInteger i=0;i<4;++i){NSDictionary *r=[self ctl:@[@"input-monitor-pan",@"get",ANA()[i]]];int16_t l=0,q=0;if([r[@"status"] integerValue]||!RawPan(r[@"output"],&l,&q))continue;
        NSInteger lv=lround(-100.0*l/32768.0),rv=lround(-100.0*q/32768.0);NSDictionary *row=self.panRows[i];[row[@"left"] setIntegerValue:lv];[row[@"right"] setIntegerValue:rv];[self panLabel:row[@"leftValue"] value:lv];[self panLabel:row[@"rightValue"] value:rv];}
}
- (void)setAuxAvailable:(BOOL)available{
    for(NSPopUpButton *p in self.outputSources)[p itemAtIndex:1].enabled=available;
    for(NSPopUpButton *p in self.hpSources)[p itemAtIndex:2].enabled=available;
    for(NSDictionary *row in self.auxRows)for(NSString *key in @[@"left",@"right",@"link",@"mute"])[row[key] setEnabled:available];
    for(NSDictionary *row in self.auxMasterRows)for(NSString *key in @[@"left",@"right",@"link",@"mute"])[row[key] setEnabled:available];
    self.auxNote.stringValue=available
        ? @"Independent stereo AUX sends. Select AUX on an output or headphone to hear this bus."
        : @"The FW1814 disables its AUX bus at 176.4/192 kHz. Saved AUX settings are retained for lower rates.";
    self.auxNote.textColor=available?NSColor.labelColor:NSColor.systemOrangeColor;
}
- (void)refreshDevice{
    AudioObjectID d=FindDevice();if(d==kAudioObjectUnknown){self.deviceStatus.stringValue=@"CoreAudio device: unavailable";self.rate.enabled=NO;self.rate.selectedSegment=-1;return;}
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};Float64 rate=0;UInt32 n=sizeof(rate);
    if(AudioObjectGetPropertyData(d,&a,0,nullptr,&n,&rate)!=noErr){self.deviceStatus.stringValue=@"CoreAudio device: rate unavailable";self.rate.enabled=NO;return;}
    self.deviceStatus.stringValue=[NSString stringWithFormat:@"CoreAudio device: connected • %.0f Hz",rate];self.rate.enabled=YES;
    self.rate.selectedSegment=-1;for(NSInteger i=0;i<6;++i)if(std::fabs(rate-kRates[i])<1){self.rate.selectedSegment=i;break;}
    [self setAuxAvailable:rate<176400.0];
}
- (void)refreshDiagnostics{
    NSMutableString *s=[NSMutableString stringWithFormat:@"macfw FW1814 Control %s build %s\n%@\n\n",macfw::fw1814::build::kVersion,macfw::fw1814::build::kGitSha,[NSDate date]];
    for(NSArray *a in @[@[@"engine",@"get"],@[@"routing",@"get"],@[@"capabilities",@"get"]]){NSDictionary *r=[self ctl:a];[s appendFormat:@"$ fw1814ctl %@\n%@\n",[a componentsJoinedByString:@" "],r[@"output"]];}self.diagnostics.string=s;
}
- (void)refresh:(id)sender{
    (void)sender;if(self.refreshing)return;self.refreshing=YES;self.status.stringValue=@"Refreshing FW1814 controls…";
    NSDictionary *e=[self ctl:@[@"engine",@"get"]];if([e[@"status"] integerValue]){[self report:e ok:@""];self.refreshing=NO;[self refreshDevice];return;}
    [self refreshRoutes];[self refreshSources];[self refreshLevels:self.swRows command:@"software-return-level" args:SW()];
    [self refreshLevels:self.inputRows command:@"input-monitor-level" args:ANA()];[self refreshLevels:self.outputRows command:@"output-volume" args:PAIR()];
    [self refreshLevels:self.hpRows command:@"headphone-volume" args:HP()];[self refreshLevels:self.auxRows command:@"aux-send-level" args:ALL()];
    [self refreshLevels:self.auxMasterRows command:@"aux-output-volume" args:@[]];[self refreshPans];[self refreshDevice];[self refreshDiagnostics];
    self.status.stringValue=@"FW1814 controls ready • changes save automatically";self.status.textColor=NSColor.labelColor;self.refreshing=NO;
}
- (void)routeChanged:(NSButton*)s{
    NSInteger i=s.tag/2,b=s.tag%2;NSString *state=s.state==NSControlStateValueOn?@"on":@"off";
    NSArray *a=i<2?@[@"mixer-route",@"set",SW()[i],PAIR()[b],state]:@[@"input-mixer-route",@"set",ANA()[i-2],PAIR()[b],state];
    NSDictionary *r=[self ctl:a];[self report:r ok:@"Routing updated and saved"];if([r[@"status"] integerValue])s.state=s.state==NSControlStateValueOn?NSControlStateValueOff:NSControlStateValueOn;
}
- (void)outputSource:(NSPopUpButton*)s{NSDictionary *r=[self ctl:@[@"output-source",@"set",PAIR()[s.tag],s.indexOfSelectedItem?@"aux":@"mixer"]];[self report:r ok:@"Analog output source updated and saved"];if([r[@"status"] integerValue])[self refreshSources];}
- (void)hpSource:(NSPopUpButton*)s{NSArray *a=@[@"mixer1/2",@"mixer3/4",@"aux"];NSDictionary *r=[self ctl:@[@"headphone-source",@"set",HP()[s.tag],a[s.indexOfSelectedItem]]];[self report:r ok:@"Headphone source updated and saved"];if([r[@"status"] integerValue])[self refreshSources];}
- (NSString*)db:(NSInteger)v{return [NSString stringWithFormat:@"%ld",(long)v];}
- (void)commitLevel:(NSControl*)c rows:(NSMutableArray*)rows command:(NSString*)cmd args:(NSArray*)args{
    NSInteger i=c.tag/2,ch=c.tag%2;if(i>=(NSInteger)rows.count)return;NSDictionary *row=rows[i];NSSlider *l=row[@"left"],*r=row[@"right"];
    if([row[@"link"] state]==NSControlStateValueOn){if(ch==0)r.integerValue=l.integerValue;else l.integerValue=r.integerValue;}
    [row[@"mute"] setState:NSControlStateValueOff];
    [row[@"leftValue"] setStringValue:[NSString stringWithFormat:@"%ld dB",(long)l.integerValue]];
    [row[@"rightValue"] setStringValue:[NSString stringWithFormat:@"%ld dB",(long)r.integerValue]];
    NSMutableArray *a=[NSMutableArray arrayWithObjects:cmd,@"set",nil];
    NSString *key=cmd;
    if(i<(NSInteger)args.count){[a addObject:args[i]];key=[NSString stringWithFormat:@"%@:%@",cmd,args[i]];}
    else key=[NSString stringWithFormat:@"%@:master",cmd];
    [a addObject:[self db:l.integerValue]];[a addObject:[self db:r.integerValue]];
    MacfwQueueControlWrite(kCtl,key,a);
    self.status.stringValue=@"Applying live level…";self.status.textColor=NSColor.labelColor;
}
- (void)commitMute:(NSButton*)c rows:(NSMutableArray*)rows command:(NSString*)cmd args:(NSArray*)args{
    NSInteger i=c.tag;if(i>=(NSInteger)rows.count)return;NSDictionary *row=rows[i];NSMutableArray *a=[NSMutableArray arrayWithObject:cmd];
    if(c.state==NSControlStateValueOn){[a addObject:@"set-all"];if(i<(NSInteger)args.count)[a addObject:args[i]];[a addObject:@"mute"];}
    else{[a addObject:@"set"];if(i<(NSInteger)args.count)[a addObject:args[i]];[a addObject:[self db:[row[@"left"] integerValue]]];[a addObject:[self db:[row[@"right"] integerValue]]];}
    NSDictionary *res=[self ctl:a];[self report:res ok:@"Mute state updated and saved"];if(![res[@"status"] integerValue])[self updateLevel:row text:res[@"output"]];
}
- (void)swLevel:(NSControl*)s{[self commitLevel:s rows:self.swRows command:@"software-return-level" args:SW()];}
- (void)swMute:(NSButton*)s{[self commitMute:s rows:self.swRows command:@"software-return-level" args:SW()];}
- (void)inputLevel:(NSControl*)s{[self commitLevel:s rows:self.inputRows command:@"input-monitor-level" args:ANA()];}
- (void)inputMute:(NSButton*)s{[self commitMute:s rows:self.inputRows command:@"input-monitor-level" args:ANA()];}
- (void)outputLevel:(NSControl*)s{[self commitLevel:s rows:self.outputRows command:@"output-volume" args:PAIR()];}
- (void)outputMute:(NSButton*)s{[self commitMute:s rows:self.outputRows command:@"output-volume" args:PAIR()];}
- (void)hpLevel:(NSControl*)s{[self commitLevel:s rows:self.hpRows command:@"headphone-volume" args:HP()];}
- (void)hpMute:(NSButton*)s{[self commitMute:s rows:self.hpRows command:@"headphone-volume" args:HP()];}
- (void)auxLevel:(NSControl*)s{[self commitLevel:s rows:self.auxRows command:@"aux-send-level" args:ALL()];}
- (void)auxMute:(NSButton*)s{[self commitMute:s rows:self.auxRows command:@"aux-send-level" args:ALL()];}
- (void)auxMasterLevel:(NSControl*)s{[self commitLevel:s rows:self.auxMasterRows command:@"aux-output-volume" args:@[]];}
- (void)auxMasterMute:(NSButton*)s{[self commitMute:s rows:self.auxMasterRows command:@"aux-output-volume" args:@[]];}
- (void)panChanged:(NSSlider*)s{
    NSInteger i=s.tag/2,ch=s.tag%2,v=s.integerValue;
    NSArray *a=@[@"input-monitor-pan",@"set-percent",ANA()[i],ch?@"right":@"left",
                 [NSString stringWithFormat:@"%ld",(long)v]];
    NSString *key=[NSString stringWithFormat:@"input-monitor-pan:%@:%@",ANA()[i],ch?@"right":@"left"];
    MacfwQueueControlWrite(kCtl,key,a);
    [self panLabel:ch?self.panRows[i][@"rightValue"]:self.panRows[i][@"leftValue"] value:v];
    self.status.stringValue=@"Applying live monitor pan…";self.status.textColor=NSColor.labelColor;
}
- (void)rateChanged:(NSSegmentedControl*)s{
    AudioObjectID d=FindDevice();if(d==kAudioObjectUnknown){NSBeep();[self refreshDevice];return;}if(s.selectedSegment<0||s.selectedSegment>=6){[self refreshDevice];return;}Float64 rate=kRates[s.selectedSegment];
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};Boolean settable=false;
    OSStatus st=AudioObjectIsPropertySettable(d,&a,&settable);if(st==noErr&&settable)st=AudioObjectSetPropertyData(d,&a,0,nullptr,sizeof(rate),&rate);
    if(st!=noErr||!settable){self.status.stringValue=[NSString stringWithFormat:@"CoreAudio rejected rate change (OSStatus %d)",(int)st];self.status.textColor=NSColor.systemRedColor;NSBeep();[self refreshDevice];return;}
    self.status.stringValue=@"Sample-rate change requested through CoreAudio…";s.enabled=NO;dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{s.enabled=YES;[self refresh:nil];});
}
- (void)resetDefaults:(id)s{(void)s;NSAlert *a=[NSAlert new];a.messageText=@"Reset FW1814 controls?";a.informativeText=@"This replaces saved routing and levels with the macfw defaults.";[a addButtonWithTitle:@"Reset"];[a addButtonWithTitle:@"Cancel"];
    if([a runModal]!=NSAlertFirstButtonReturn)return;NSDictionary *r=Run(kState,@[@"reset"]);[self report:r ok:@"FW1814 defaults applied and saved"];if(![r[@"status"] integerValue])[self refresh:nil];}
- (void)copyDiagnostics:(id)s{(void)s;[NSPasteboard.generalPasteboard clearContents];[NSPasteboard.generalPasteboard setString:self.diagnostics.string?:@"" forType:NSPasteboardTypeString];self.status.stringValue=@"Diagnostics copied";}
- (void)openLog:(id)s{(void)s;if(![[NSFileManager defaultManager] fileExistsAtPath:kLog]){self.status.stringValue=@"Transport log is not present";NSBeep();return;}[NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:kLog]];}
@end

int main(int argc,const char *argv[]){(void)argc;(void)argv;@autoreleasepool{NSApplication *app=[NSApplication sharedApplication];[app setActivationPolicy:NSApplicationActivationPolicyRegular];AppDelegate *d=[AppDelegate new];app.delegate=d;[app run];}return 0;}
