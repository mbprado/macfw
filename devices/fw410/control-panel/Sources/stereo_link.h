#pragma once
#import <AppKit/AppKit.h>

// GUI-only stereo-link helper. Enabling the link never changes hardware state.
// While linked, moving either channel applies the same delta to its partner,
// preserving any pre-existing L/R offset and clamping to the slider range.
static inline double MacfwClampSlider(NSSlider *slider, double value) {
    return fmax(slider.minValue, fmin(slider.maxValue, value));
}

static inline void MacfwApplyStereoLink(NSSlider *moved,
                                         NSSlider *other,
                                         BOOL linked,
                                         double previousMovedValue) {
    if (!linked) return;
    const double delta = moved.doubleValue - previousMovedValue;
    other.doubleValue = MacfwClampSlider(other, other.doubleValue + delta);
}

static inline NSButton *MacfwMakeLinkButton(id target, SEL action, NSRect frame) {
    NSButton *button = [NSButton buttonWithTitle:@"Link" target:target action:action];
    button.buttonType = NSButtonTypePushOnPushOff;
    button.bezelStyle = NSBezelStyleRounded;
    button.frame = frame;
    button.toolTip = @"Link left and right levels while preserving their current difference";
    return button;
}
