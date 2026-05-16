#import <Cocoa/Cocoa.h>

#include "../ConfigLoader.h"
#include "../Parsers.h"
#include "MacOverlayWindow.h"

#include <string>
#include <vector>

// Cocoa chat renderer.
//
// Port of Overlay_Rendering.cpp into AppKit. Reads message text shape via
// parsers::ParseSegments from Parsers.h so prose vs. code splitting is the
// same code path as the Windows side; only the draw layer changes.
//
// Visual responsibilities here:
//   - rounded translucent background + status bar with live transcript
//   - chat bubbles (user right, bot left), with code block insets
//   - "thinking…" indicator while an LLM call is in flight
//   - audio level dot pulsing with RecentEnergy()
//   - hotkey hints overlay panel (toggled by Cmd+Option+/)
//
// Inline-markdown polish (bold/italic, syntax highlighting) is deferred —
// StripInlineMd keeps prose readable until that lands.

// -----------------------------------------------------------------------------
// Helpers (forward-declared so drawRect can use them)
// -----------------------------------------------------------------------------

static NSColor* ColorFromRGB(unsigned long rgb) {
    CGFloat r = (CGFloat)(rgb & 0xFF) / 255.0;
    CGFloat g = (CGFloat)((rgb >> 8)  & 0xFF) / 255.0;
    CGFloat b = (CGFloat)((rgb >> 16) & 0xFF) / 255.0;
    return [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

static NSString* WideToNS(const std::wstring& w) {
    if (w.empty()) return @"";
    std::string utf8;
    utf8.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t low = (uint32_t)w[i+1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) utf8 += (char)cp;
        else if (cp < 0x800) {
            utf8 += (char)(0xC0 | (cp >> 6));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8 += (char)(0xE0 | (cp >> 12));
            utf8 += (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else {
            utf8 += (char)(0xF0 | (cp >> 18));
            utf8 += (char)(0x80 | ((cp >> 12) & 0x3F));
            utf8 += (char)(0x80 | ((cp >> 6)  & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        }
    }
    return [NSString stringWithUTF8String:utf8.c_str()] ?: @"";
}

// -----------------------------------------------------------------------------
// ChatView
// -----------------------------------------------------------------------------

@interface ChatView : NSView {
    @public
    const std::vector<ChatMessage>* messages;
    const LLMConfig* configRef;    // weak — owned by MacOverlayWindowImpl
    NSString*  transcript;
    ConfigLoader::ThemeColors theme;
    std::string themeId;
    BOOL autoMode;
    BOOL moveMode;
    BOOL selectMode;
    BOOL thinking;
    BOOL hintsVisible;
    float audioLevel;
    CGFloat scrollOffset;
    CGFloat contentHeight;
}
- (void)scrollByDelta:(CGFloat)delta;
- (void)setMessagesPointer:(const std::vector<ChatMessage>*)m;
- (void)drawHintsPanel:(NSRect)bounds;
@end

@implementation ChatView

- (instancetype)initWithFrame:(NSRect)frame {
    if (self = [super initWithFrame:frame]) {
        themeId = "dark";
        theme = ConfigLoader::GetTheme(themeId);
        transcript = @"";
        autoMode = NO;
        moveMode = NO;
        selectMode = NO;
        thinking = NO;
        hintsVisible = NO;
        audioLevel = 0;
        scrollOffset = 0;
        contentHeight = 0;
        messages = nullptr;
        configRef = nullptr;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)setMessagesPointer:(const std::vector<ChatMessage>*)m {
    messages = m;
}

- (void)scrollByDelta:(CGFloat)delta {
    scrollOffset += delta;
    if (scrollOffset < 0) scrollOffset = 0;
    CGFloat maxOff = MAX(0.0, contentHeight - self.bounds.size.height);
    if (scrollOffset > maxOff) scrollOffset = maxOff;
    [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent*)event {
    [self scrollByDelta:-event.scrollingDeltaY];
}

// Build a short "G grab · A audio · V clip · D hide · X exit" string from the
// live bindings, so it stays accurate after a rebind.
- (NSString*)defaultHintString {
    if (!configRef) {
        return @"⌘⌥G screen · ⌘⌥A audio · ⌘⌥V clipboard · ⌘⌥D hide · ⌘⌥X exit";
    }
    auto fmt = [&](HotkeyAction a) {
        std::string s = ConfigLoader::BindingToString(configRef->hotkeys.bindings[(int)a]);
        return [NSString stringWithUTF8String:s.c_str()];
    };
    return [NSString stringWithFormat:@"%@ screen · %@ audio · %@ clip · %@ hide · %@ exit",
        fmt(HotkeyAction::SendScreen),
        fmt(HotkeyAction::SendAudio),
        fmt(HotkeyAction::SendText),
        fmt(HotkeyAction::ToggleVisibility),
        fmt(HotkeyAction::ExitApp)];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    @autoreleasepool {
        NSRect bounds = self.bounds;

        // Rounded translucent background
        NSBezierPath* bg = [NSBezierPath bezierPathWithRoundedRect:bounds
                                                          xRadius:14
                                                          yRadius:14];
        [[ColorFromRGB(theme.bg) colorWithAlphaComponent:0.88] set];
        [bg fill];

        // Top status bar
        CGFloat barH = 30;
        NSRect barRect = NSMakeRect(0, 0, bounds.size.width, barH);
        [[ColorFromRGB(theme.bar_bg) colorWithAlphaComponent:0.95] set];
        NSRectFill(barRect);

        // Mode badges + transcript / hint
        NSMutableString* status = [NSMutableString string];
        if (autoMode)   [status appendString:@"[AUTO] "];
        if (moveMode)   [status appendString:@"[MOVE] "];
        if (selectMode) [status appendString:@"[SELECT] "];
        if (thinking)   [status appendString:@"thinking… "];
        if (transcript && transcript.length > 0) [status appendString:transcript];
        if (status.length == 0) [status appendString:[self defaultHintString]];

        NSDictionary* barAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: ColorFromRGB(theme.bar_text),
        };
        // Leave room on the right for the audio level dot.
        NSRect barTextRect = NSMakeRect(12, 7, bounds.size.width - 40, 18);
        [status drawInRect:barTextRect withAttributes:barAttrs];

        // Audio level dot (top-right of bar). Radius grows with energy.
        {
            CGFloat lvl = audioLevel;
            if (lvl < 0) lvl = 0; if (lvl > 1) lvl = 1;
            CGFloat radius = 3 + lvl * 6;
            NSColor* dotColor = lvl > 0.02
                ? [NSColor systemGreenColor]
                : [[NSColor systemGrayColor] colorWithAlphaComponent:0.5];
            NSRect dot = NSMakeRect(bounds.size.width - 18 - radius,
                                    (barH - 2 * radius) / 2,
                                    2 * radius, 2 * radius);
            [dotColor set];
            [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
        }

        // Empty state
        if (!messages || messages->empty()) {
            contentHeight = 0;
            NSString* hint = [NSString stringWithFormat:
                @"Press %@ to capture the screen.\n"
                @"Press %@ for the last 30s of audio.\n"
                @"Press %@ to send clipboard text.\n"
                @"This window is invisible to screen capture (NSWindowSharingNone).",
                configRef ? [NSString stringWithUTF8String:
                    ConfigLoader::BindingToString(configRef->hotkeys.bindings[(int)HotkeyAction::SendScreen]).c_str()]
                          : @"⌘⌥G",
                configRef ? [NSString stringWithUTF8String:
                    ConfigLoader::BindingToString(configRef->hotkeys.bindings[(int)HotkeyAction::SendAudio]).c_str()]
                          : @"⌘⌥A",
                configRef ? [NSString stringWithUTF8String:
                    ConfigLoader::BindingToString(configRef->hotkeys.bindings[(int)HotkeyAction::SendText]).c_str()]
                          : @"⌘⌥V"];
            NSDictionary* hintAttrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:14],
                NSForegroundColorAttributeName:
                    [ColorFromRGB(theme.prose_text) colorWithAlphaComponent:0.6],
            };
            NSRect hintRect = NSMakeRect(20, barH + 20,
                                         bounds.size.width - 40,
                                         bounds.size.height - barH - 40);
            [hint drawInRect:hintRect withAttributes:hintAttrs];
            if (hintsVisible) [self drawHintsPanel:bounds];
            return;
        }

        // Chat bubbles
        const CGFloat margin = 14;
        const CGFloat bubbleMaxWidth = bounds.size.width - 2 * margin - 20;
        const CGFloat bubblePadX = 12;
        const CGFloat bubblePadY = 8;
        CGFloat y = barH + 12 - scrollOffset;

        NSFont* proseFont = [NSFont systemFontOfSize:14];
        NSFont* codeFont  = [NSFont fontWithName:@"Menlo" size:13]
                            ?: [NSFont userFixedPitchFontOfSize:13];

        for (const auto& m : *messages) {
            std::vector<parsers::Segment> segs = parsers::ParseSegments(m.text);
            if (segs.empty()) continue;

            std::vector<NSAttributedString*> segStrings;
            std::vector<NSSize> segSizes;
            CGFloat innerHeight = 0;
            CGFloat innerWidth = 0;

            for (const auto& seg : segs) {
                NSString* s;
                NSFont* f;
                NSColor* color;
                if (seg.isCode) {
                    s = WideToNS(seg.text);
                    f = codeFont;
                    color = ColorFromRGB(theme.code_text);
                } else {
                    std::wstring stripped = parsers::StripInlineMd(seg.text);
                    s = WideToNS(stripped);
                    f = proseFont;
                    color = ColorFromRGB(theme.prose_text);
                }
                NSDictionary* attrs = @{
                    NSFontAttributeName: f,
                    NSForegroundColorAttributeName: color,
                };
                NSAttributedString* attr = [[NSAttributedString alloc]
                    initWithString:s attributes:attrs];

                NSSize boxSize = NSMakeSize(bubbleMaxWidth - 2 * bubblePadX, CGFLOAT_MAX);
                NSSize sz = [attr boundingRectWithSize:boxSize
                                              options:NSStringDrawingUsesLineFragmentOrigin
                                              context:nil].size;
                segStrings.push_back(attr);
                segSizes.push_back(sz);
                innerHeight += sz.height + 4;
                if (seg.isCode) innerHeight += 8;
                if (sz.width > innerWidth) innerWidth = sz.width;
            }

            CGFloat bubbleHeight = innerHeight + 2 * bubblePadY;
            if (bubbleHeight < 32) bubbleHeight = 32;
            CGFloat bubbleWidth = innerWidth + 2 * bubblePadX;
            if (bubbleWidth > bubbleMaxWidth) bubbleWidth = bubbleMaxWidth;
            if (bubbleWidth < 80) bubbleWidth = 80;

            CGFloat x = m.isUser
                ? (bounds.size.width - margin - bubbleWidth)
                : margin;

            NSRect bubbleRect = NSMakeRect(x, y, bubbleWidth, bubbleHeight);
            NSColor* fill = m.isUser
                ? [ColorFromRGB(theme.user_bubble) colorWithAlphaComponent:0.95]
                : [ColorFromRGB(theme.bot_bubble)  colorWithAlphaComponent:0.95];
            NSBezierPath* bubble = [NSBezierPath bezierPathWithRoundedRect:bubbleRect
                                                                   xRadius:10
                                                                   yRadius:10];
            [fill set];
            [bubble fill];

            CGFloat textY = y + bubblePadY;
            for (size_t i = 0; i < segStrings.size(); ++i) {
                const auto& seg = segs[i];
                NSAttributedString* attr = segStrings[i];
                NSSize sz = segSizes[i];
                NSRect tRect;
                if (seg.isCode) {
                    NSRect codeBg = NSMakeRect(x + 6, textY - 4, bubbleWidth - 12, sz.height + 8);
                    [[ColorFromRGB(theme.code_bg) colorWithAlphaComponent:0.85] set];
                    NSBezierPath* cb = [NSBezierPath bezierPathWithRoundedRect:codeBg
                                                                       xRadius:6
                                                                       yRadius:6];
                    [cb fill];
                    tRect = NSMakeRect(x + bubblePadX + 4, textY,
                                       bubbleWidth - 2 * bubblePadX - 8, sz.height);
                    [attr drawInRect:tRect];
                    textY += sz.height + 8;
                } else {
                    tRect = NSMakeRect(x + bubblePadX, textY,
                                       bubbleWidth - 2 * bubblePadX, sz.height);
                    [attr drawInRect:tRect];
                    textY += sz.height + 4;
                }
            }

            y += bubbleHeight + 10;
        }

        contentHeight = (y + scrollOffset) - (barH + 12);

        if (hintsVisible) [self drawHintsPanel:bounds];
    }
}

// Overlay panel that lists every current hotkey binding. Toggled by the
// Cmd+Option+/ hardcoded hotkey from MacOverlayWindow.
- (void)drawHintsPanel:(NSRect)bounds {
    const CGFloat panelW = MIN(420.0, bounds.size.width - 40);
    const CGFloat panelH = MIN(440.0, bounds.size.height - 50);
    NSRect panel = NSMakeRect((bounds.size.width - panelW) / 2,
                              (bounds.size.height - panelH) / 2,
                              panelW, panelH);
    [[NSColor colorWithWhite:0 alpha:0.9] set];
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:panel
                                                      xRadius:12 yRadius:12];
    [p fill];
    [[NSColor whiteColor] set];
    p.lineWidth = 1;
    [p stroke];

    NSDictionary* titleAttrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:16],
        NSForegroundColorAttributeName: [NSColor whiteColor],
    };
    NSDictionary* rowAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: [NSColor whiteColor],
    };
    NSDictionary* keyAttrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor systemTealColor],
    };

    [@"Hotkeys" drawAtPoint:NSMakePoint(panel.origin.x + 16, panel.origin.y + 14)
              withAttributes:titleAttrs];

    [@"Press F2 (or ⌘⌥/) to close · ⌘⌥, to re-open settings · F1 for About" drawAtPoint:
        NSMakePoint(panel.origin.x + 16, panel.origin.y + 38)
                                                  withAttributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:11],
        NSForegroundColorAttributeName: [NSColor lightGrayColor],
    }];

    CGFloat ry = panel.origin.y + 64;
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        NSString* label = [NSString stringWithUTF8String:
            ConfigLoader::ActionLabel((HotkeyAction)i)];
        NSString* keyStr = configRef
            ? [NSString stringWithUTF8String:
                ConfigLoader::BindingToString(configRef->hotkeys.bindings[i]).c_str()]
            : @"(unset)";
        [label  drawAtPoint:NSMakePoint(panel.origin.x + 16, ry) withAttributes:rowAttrs];
        [keyStr drawAtPoint:NSMakePoint(panel.origin.x + 230, ry) withAttributes:keyAttrs];
        ry += 22;
    }
}

@end

// -----------------------------------------------------------------------------
// C bridge — called from MacOverlayWindow.mm
// -----------------------------------------------------------------------------

extern "C" NSView* CreateChatView(NSRect frame) {
    return [[ChatView alloc] initWithFrame:frame];
}

extern "C" void ChatViewSetMessages(NSView* view, const std::vector<ChatMessage>* msgs) {
    if ([view isKindOfClass:[ChatView class]]) {
        [(ChatView*)view setMessagesPointer:msgs];
    }
}

extern "C" void ChatViewSetTranscript(NSView* view, NSString* text) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->transcript = text ?: @"";
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewSetTheme(NSView* view, const char* themeId) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->themeId = std::string(themeId ? themeId : "dark");
        cv->theme = ConfigLoader::GetTheme(cv->themeId);
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewSetMode(NSView* view, BOOL autoMode, BOOL moveMode, BOOL selectMode) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->autoMode = autoMode;
        cv->moveMode = moveMode;
        cv->selectMode = selectMode;
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewScroll(NSView* view, int delta) {
    if ([view isKindOfClass:[ChatView class]]) {
        [(ChatView*)view scrollByDelta:(CGFloat)delta];
    }
}

extern "C" void ChatViewSetConfig(NSView* view, const LLMConfig* cfg) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->configRef = cfg;
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewSetAudioLevel(NSView* view, float level) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->audioLevel = level;
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewSetThinking(NSView* view, BOOL thinking) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->thinking = thinking;
        [cv setNeedsDisplay:YES];
    }
}

extern "C" void ChatViewToggleHints(NSView* view) {
    if ([view isKindOfClass:[ChatView class]]) {
        ChatView* cv = (ChatView*)view;
        cv->hintsVisible = !cv->hintsVisible;
        [cv setNeedsDisplay:YES];
    }
}
