#import <Cocoa/Cocoa.h>

#include "../ConfigLoader.h"
#include "../Parsers.h"
#include "MacOverlayWindow.h"

#include <string>
#include <vector>

// Cocoa chat renderer.
//
// First-cut port of Overlay_Rendering.cpp (~35KB of GDI code) into AppKit. The
// segment / code-vs-prose split is reused from Parsers.h; only the drawing
// layer changes. Visual fidelity matches the dark-mode look the Windows side
// uses (rounded bubbles, blue user, grey bot, monospace code blocks, status
// bar at the top). Inline-markdown polish (bold/italic, bracket colorizing) is
// deferred — added once the rest of the port is shipping. StripInlineMd keeps
// prose readable in the meantime.

// -----------------------------------------------------------------------------
// Helpers (declared before use)
// -----------------------------------------------------------------------------

static NSColor* ColorFromRGB(unsigned long rgb) {
    CGFloat r = (CGFloat)(rgb & 0xFF) / 255.0;
    CGFloat g = (CGFloat)((rgb >> 8)  & 0xFF) / 255.0;
    CGFloat b = (CGFloat)((rgb >> 16) & 0xFF) / 255.0;
    return [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

// std::wstring -> NSString via UTF-8 (handles UTF-16 surrogate pairs on
// platforms where wchar_t is 2 bytes; on macOS it's 4 bytes and the loop
// just decodes scalars directly).
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
// ChatView — the NSView that paints messages
// -----------------------------------------------------------------------------

@interface ChatView : NSView {
    @public
    const std::vector<ChatMessage>* messages;
    NSString*  transcript;
    ConfigLoader::ThemeColors theme;
    std::string themeId;
    BOOL autoMode;
    BOOL moveMode;
    BOOL selectMode;
    CGFloat scrollOffset;
    CGFloat contentHeight;
}
- (void)scrollByDelta:(CGFloat)delta;
- (void)setMessagesPointer:(const std::vector<ChatMessage>*)m;
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
        scrollOffset = 0;
        contentHeight = 0;
        messages = nullptr;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }   // y grows downward — easier for chat scroll

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

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    @autoreleasepool {
        NSRect bounds = self.bounds;
        CGFloat opacity = 0.85;

        // Rounded translucent background
        NSBezierPath* bg = [NSBezierPath bezierPathWithRoundedRect:bounds
                                                          xRadius:14
                                                          yRadius:14];
        [[ColorFromRGB(theme.bg) colorWithAlphaComponent:opacity] set];
        [bg fill];

        // Top status / transcript bar
        CGFloat barH = 28;
        NSRect barRect = NSMakeRect(0, 0, bounds.size.width, barH);
        [[ColorFromRGB(theme.bar_bg) colorWithAlphaComponent:0.95] set];
        NSRectFill(barRect);

        NSMutableString* status = [NSMutableString string];
        if (autoMode)   [status appendString:@"[AUTO] "];
        if (moveMode)   [status appendString:@"[MOVE] "];
        if (selectMode) [status appendString:@"[SELECT] "];
        if (transcript && transcript.length > 0) [status appendString:transcript];
        if (status.length == 0) {
            [status appendString:@"Invisible AI Overlay — F8 screen · F7 audio · INS clipboard · DEL hide · END exit"];
        }

        NSDictionary* barAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: ColorFromRGB(theme.bar_text),
        };
        NSRect barTextRect = NSInsetRect(barRect, 12, 6);
        [status drawInRect:barTextRect withAttributes:barAttrs];

        // Empty state
        if (!messages || messages->empty()) {
            contentHeight = 0;
            NSString* hint = @"Press F8 to capture the screen, F7 for last 30s audio, "
                             @"INS to send clipboard text.\n"
                             @"This window is invisible to screen capture.";
            NSDictionary* hintAttrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:14],
                NSForegroundColorAttributeName:
                    [ColorFromRGB(theme.prose_text) colorWithAlphaComponent:0.6],
            };
            NSRect hintRect = NSMakeRect(20, barH + 20,
                                         bounds.size.width - 40,
                                         bounds.size.height - barH - 40);
            [hint drawInRect:hintRect withAttributes:hintAttrs];
            return;
        }

        // Bubbles — measure each then draw
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
