#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

#include "../ConfigLoader.h"
#include "../WinCompat.h"

#include <string>
#include <vector>

// Welcome / configuration dialog (macOS port of ConfigDialog.cpp).
//
// Two-column layout:
//   Left:  Provider · Model · API Key · Base URL (custom only) · Gemini
//          fallback · Mic checkbox · Theme
//   Right: Hotkey rebinder — one row per HotkeyAction, click to capture a
//          new combo. "Reset to Mac defaults" wipes back to ⌘⌥G/A/etc.
//
// The rebinder uses a local NSEvent monitor to intercept the next keyDown
// without firing it through to other apps.

// -----------------------------------------------------------------------------
// NSEvent / Carbon ↔ our VK_* / MOD_* conversion
// -----------------------------------------------------------------------------

static unsigned int CarbonKeyToVk(unsigned short kc) {
    switch (kc) {
        case kVK_F1:  return VK_F1;
        case kVK_F2:  return VK_F2;
        case kVK_F3:  return VK_F3;
        case kVK_F4:  return VK_F4;
        case kVK_F5:  return VK_F5;
        case kVK_F6:  return VK_F6;
        case kVK_F7:  return VK_F7;
        case kVK_F8:  return VK_F8;
        case kVK_F9:  return VK_F9;
        case kVK_F10: return VK_F10;
        case kVK_F11: return VK_F11;
        case kVK_F12: return VK_F12;

        case kVK_ANSI_A: return 'A'; case kVK_ANSI_B: return 'B';
        case kVK_ANSI_C: return 'C'; case kVK_ANSI_D: return 'D';
        case kVK_ANSI_E: return 'E'; case kVK_ANSI_F: return 'F';
        case kVK_ANSI_G: return 'G'; case kVK_ANSI_H: return 'H';
        case kVK_ANSI_I: return 'I'; case kVK_ANSI_J: return 'J';
        case kVK_ANSI_K: return 'K'; case kVK_ANSI_L: return 'L';
        case kVK_ANSI_M: return 'M'; case kVK_ANSI_N: return 'N';
        case kVK_ANSI_O: return 'O'; case kVK_ANSI_P: return 'P';
        case kVK_ANSI_Q: return 'Q'; case kVK_ANSI_R: return 'R';
        case kVK_ANSI_S: return 'S'; case kVK_ANSI_T: return 'T';
        case kVK_ANSI_U: return 'U'; case kVK_ANSI_V: return 'V';
        case kVK_ANSI_W: return 'W'; case kVK_ANSI_X: return 'X';
        case kVK_ANSI_Y: return 'Y'; case kVK_ANSI_Z: return 'Z';
        case kVK_ANSI_0: return '0'; case kVK_ANSI_1: return '1';
        case kVK_ANSI_2: return '2'; case kVK_ANSI_3: return '3';
        case kVK_ANSI_4: return '4'; case kVK_ANSI_5: return '5';
        case kVK_ANSI_6: return '6'; case kVK_ANSI_7: return '7';
        case kVK_ANSI_8: return '8'; case kVK_ANSI_9: return '9';

        case kVK_Return:        return VK_RETURN;
        case kVK_Tab:           return VK_TAB;
        case kVK_Space:         return VK_SPACE;
        case kVK_Escape:        return VK_ESCAPE;
        case kVK_ForwardDelete: return VK_DELETE;
        case kVK_Home:          return VK_HOME;
        case kVK_End:           return VK_END;
        case kVK_PageUp:        return VK_PRIOR;
        case kVK_PageDown:      return VK_NEXT;
        case kVK_Help:          return VK_INSERT;
        default: return 0;
    }
}

static unsigned int EventModsToWin(NSEventModifierFlags f) {
    unsigned int m = 0;
    if (f & NSEventModifierFlagControl) m |= MOD_CONTROL;
    if (f & NSEventModifierFlagOption)  m |= MOD_ALT;
    if (f & NSEventModifierFlagShift)   m |= MOD_SHIFT;
    if (f & NSEventModifierFlagCommand) m |= MOD_WIN;
    return m;
}

// -----------------------------------------------------------------------------
// Controller
// -----------------------------------------------------------------------------

@class RebindButton;

@interface ConfigDialogController : NSObject {
@public
    NSWindow* window;
    NSPopUpButton* providerBtn;
    NSPopUpButton* modelBtn;
    NSTextField* apiKeyField;       // plain field so Cmd+V paste works
    NSTextField* fallbackField;
    NSTextField* baseUrlField;
    NSTextField* baseUrlLabel;
    NSButton* micCheckbox;
    NSButton* soundCheckbox;
    NSPopUpButton* themeBtn;
    NSMutableArray<RebindButton*>* rebindButtons;
    NSTextField* statusLabel;
    BOOL accepted;
    LLMConfig* config;
    const std::vector<ModelInfo>* models;
}
- (void)providerChanged:(id)sender;
- (void)startPressed:(id)sender;
- (void)cancelPressed:(id)sender;
- (void)resetHotkeysPressed:(id)sender;
- (void)populateModelsForProvider:(const std::string&)pid;
- (void)refreshRebindButtonTitles;
- (std::string)currentProviderId;
- (void)setStatus:(NSString*)s color:(NSColor*)c;
@end

@interface RebindButton : NSButton {
@public
    NSInteger actionIndex;
    __weak ConfigDialogController* owner;
    id monitor;
}
- (void)beginCapture;
- (void)endCapture:(BOOL)applied withMods:(unsigned)mods vk:(unsigned)vk;
@end

// -----------------------------------------------------------------------------
// RebindButton impl
// -----------------------------------------------------------------------------

@implementation RebindButton

- (void)beginCapture {
    [self setTitle:@"Press combo…"];
    [self setEnabled:NO];   // visual feedback; we re-enable in endCapture
    [self setBezelColor:[NSColor systemOrangeColor]];

    __weak RebindButton* weakSelf = self;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent*(NSEvent* ev) {
            RebindButton* strongSelf = weakSelf;
            if (!strongSelf) return ev;
            unsigned mods = EventModsToWin(ev.modifierFlags);
            unsigned vk = CarbonKeyToVk(ev.keyCode);
            // Esc cancels the rebind.
            if (ev.keyCode == kVK_Escape) {
                [strongSelf endCapture:NO withMods:0 vk:0];
                return nil;
            }
            // Bare modifier press — ignore, wait for the actual key.
            if (vk == 0) return nil;
            [strongSelf endCapture:YES withMods:mods vk:vk];
            return nil;
        }];
}

- (void)endCapture:(BOOL)applied withMods:(unsigned)mods vk:(unsigned)vk {
    if (monitor) {
        [NSEvent removeMonitor:monitor];
        monitor = nil;
    }
    [self setEnabled:YES];
    [self setBezelColor:nil];

    ConfigDialogController* ctrl = owner;
    if (!ctrl) return;

    if (applied) {
        // Conflict check: any other action with the same binding?
        const HotkeyConfig& current = ctrl->config->hotkeys;
        for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
            if (i == actionIndex) continue;
            if (current.bindings[i].modifiers == mods && current.bindings[i].vk == vk) {
                NSString* label = [NSString stringWithUTF8String:
                    ConfigLoader::ActionLabel((HotkeyAction)i)];
                [ctrl setStatus:[NSString stringWithFormat:@"Conflict with: %@", label]
                          color:[NSColor systemRedColor]];
                [self setTitle:[NSString stringWithUTF8String:
                    ConfigLoader::BindingToString(ctrl->config->hotkeys.bindings[actionIndex]).c_str()]];
                return;
            }
        }
        ctrl->config->hotkeys.bindings[actionIndex].modifiers = mods;
        ctrl->config->hotkeys.bindings[actionIndex].vk = vk;
        [ctrl setStatus:@"Hotkey updated." color:[NSColor systemGreenColor]];
    }
    [self setTitle:[NSString stringWithUTF8String:
        ConfigLoader::BindingToString(ctrl->config->hotkeys.bindings[actionIndex]).c_str()]];
}

@end

// -----------------------------------------------------------------------------
// ConfigDialogController impl
// -----------------------------------------------------------------------------

@implementation ConfigDialogController

- (std::string)currentProviderId {
    NSInteger idx = [providerBtn indexOfSelectedItem];
    const auto& provs = ConfigLoader::BuiltinProviders();
    if (idx < 0 || (NSUInteger)idx >= provs.size()) return "gemini";
    return provs[idx].id;
}

- (void)providerChanged:(id)sender {
    (void)sender;
    const ProviderInfo* p = ConfigLoader::FindProvider(self.currentProviderId);
    if (p) {
        [self populateModelsForProvider:p->id];
        BOOL isCustom = (p->id == "custom");
        [baseUrlField setHidden:!isCustom];
        [baseUrlLabel setHidden:!isCustom];
    }
}

- (void)populateModelsForProvider:(const std::string&)pid {
    [modelBtn removeAllItems];
    const ProviderInfo* p = ConfigLoader::FindProvider(pid);
    if (!p) return;
    for (const auto& m : p->models) {
        NSString* label = [NSString stringWithUTF8String:
            (m.id.empty() ? m.name : (m.id + " — " + m.name)).c_str()];
        [modelBtn addItemWithTitle:label];
    }
    if (pid == "gemini" && models) {
        for (const auto& m : *models) {
            NSString* label = [NSString stringWithUTF8String:
                (m.id + " — " + m.name).c_str()];
            if ([modelBtn indexOfItemWithTitle:label] == -1) {
                [modelBtn addItemWithTitle:label];
            }
        }
    }
}

- (void)refreshRebindButtonTitles {
    for (RebindButton* b in rebindButtons) {
        NSString* title = [NSString stringWithUTF8String:
            ConfigLoader::BindingToString(config->hotkeys.bindings[b->actionIndex]).c_str()];
        [b setTitle:title];
    }
}

- (void)resetHotkeysPressed:(id)sender {
    (void)sender;
    config->hotkeys = ConfigLoader::DefaultHotkeys();
    [self refreshRebindButtonTitles];
    [self setStatus:@"Hotkeys reset to Mac defaults." color:[NSColor systemBlueColor]];
}

- (void)startPressed:(id)sender {
    (void)sender;

    // Block "Start" if the API key field is blank or whitespace-only — the
    // overlay can't reach any LLM without one, and a silent dialog dismiss
    // followed by an "API key missing" error in the overlay is worse than
    // catching it here.
    NSString* keyRaw = [apiKeyField stringValue] ?: @"";
    NSString* keyTrim = [keyRaw stringByTrimmingCharactersInSet:
                          [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (keyTrim.length == 0) {
        [self setStatus:@"API key is required — paste your provider key above."
                  color:[NSColor systemRedColor]];
        [[apiKeyField window] makeFirstResponder:apiKeyField];
        NSBeep();
        return;
    }

    accepted = YES;

    config->provider = self.currentProviderId;
    NSString* modelTitle = [modelBtn titleOfSelectedItem];
    if (modelTitle) {
        std::string m = [modelTitle UTF8String];
        size_t dash = m.find(" — ");
        if (dash != std::string::npos) m = m.substr(0, dash);
        config->model = m;
    }
    config->api_key = [keyTrim UTF8String];
    config->gemini_fallback_key = [[fallbackField stringValue] UTF8String];
    if (config->provider == "custom") {
        config->base_url = [[baseUrlField stringValue] UTF8String];
    }
    config->capture_mic = ([micCheckbox state] == NSControlStateValueOn);
    config->sound_on_auto = ([soundCheckbox state] == NSControlStateValueOn);
    NSString* theme = [themeBtn titleOfSelectedItem];
    if (theme) {
        std::string t = [[theme lowercaseString] UTF8String];
        config->theme = t;
    }

    [NSApp stopModal];
    [window orderOut:nil];
}

- (void)cancelPressed:(id)sender {
    (void)sender;
    accepted = NO;
    [NSApp stopModal];
    [window orderOut:nil];
}

- (void)setStatus:(NSString*)s color:(NSColor*)c {
    [statusLabel setStringValue:s];
    [statusLabel setTextColor:c];
}

@end

// -----------------------------------------------------------------------------
// Public entry
// -----------------------------------------------------------------------------

namespace MacConfigDialog {

static NSTextField* MakeLabel(NSString* text, NSRect frame, CGFloat fontSize) {
    NSTextField* lbl = [[NSTextField alloc] initWithFrame:frame];
    [lbl setStringValue:text];
    [lbl setBezeled:NO];
    [lbl setDrawsBackground:NO];
    [lbl setEditable:NO];
    [lbl setSelectable:NO];
    [lbl setFont:[NSFont systemFontOfSize:fontSize]];
    return lbl;
}

static NSBox* MakeDivider(NSRect frame) {
    NSBox* line = [[NSBox alloc] initWithFrame:frame];
    [line setBoxType:NSBoxSeparator];
    return line;
}

bool Show(LLMConfig& config, const std::vector<ModelInfo>& models) {
    @autoreleasepool {
        ConfigDialogController* ctrl = [[ConfigDialogController alloc] init];
        ctrl->config = &config;
        ctrl->models = &models;
        ctrl->accepted = NO;
        ctrl->rebindButtons = [NSMutableArray array];

        const CGFloat W = 760, H = 680;
        const CGFloat LCX = 20;     // left column x
        const CGFloat LCW = 340;    // left column width
        const CGFloat RCX = 400;    // right column x
        const CGFloat RCW = 340;    // right column width
        const CGFloat RB = 30;      // rebind button width per-row offset

        NSRect frame = NSMakeRect(0, 0, W, H);
        ctrl->window = [[NSWindow alloc] initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
            backing:NSBackingStoreBuffered
            defer:NO];
        [ctrl->window setTitle:@"Invisible AI Overlay — Setup"];
        [ctrl->window center];
        [ctrl->window setReleasedWhenClosed:NO];

        NSView* root = [ctrl->window contentView];

        // Header
        NSTextField* brand = MakeLabel(@"✨ Invisible AI Overlay",
                                       NSMakeRect(LCX, H - 44, W - 40, 28), 22);
        [brand setFont:[NSFont boldSystemFontOfSize:22]];
        [root addSubview:brand];

        NSTextField* tagline = MakeLabel(
            @"Configure your LLM provider and hotkeys. Click any binding to record a new combo.",
            NSMakeRect(LCX, H - 68, W - 40, 18), 12);
        [tagline setTextColor:[NSColor secondaryLabelColor]];
        [root addSubview:tagline];

        [root addSubview:MakeDivider(NSMakeRect(LCX, H - 84, W - 40, 1))];

        // ---- LEFT COLUMN ----
        CGFloat y = H - 120;

        [root addSubview:MakeLabel(@"Provider", NSMakeRect(LCX, y, 80, 18), 12)];
        ctrl->providerBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(LCX + 90, y - 5, LCW - 90, 26) pullsDown:NO];
        for (const auto& p : ConfigLoader::BuiltinProviders()) {
            [ctrl->providerBtn addItemWithTitle:
                [NSString stringWithUTF8String:p.name.c_str()]];
        }
        {
            const auto& provs = ConfigLoader::BuiltinProviders();
            for (size_t i = 0; i < provs.size(); ++i) {
                if (provs[i].id == config.provider) {
                    [ctrl->providerBtn selectItemAtIndex:(NSInteger)i];
                    break;
                }
            }
        }
        [ctrl->providerBtn setTarget:ctrl];
        [ctrl->providerBtn setAction:@selector(providerChanged:)];
        [root addSubview:ctrl->providerBtn];
        y -= 38;

        [root addSubview:MakeLabel(@"Model", NSMakeRect(LCX, y, 80, 18), 12)];
        ctrl->modelBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(LCX + 90, y - 5, LCW - 90, 26) pullsDown:NO];
        [root addSubview:ctrl->modelBtn];
        [ctrl populateModelsForProvider:config.provider];
        y -= 38;

        [root addSubview:MakeLabel(@"API Key", NSMakeRect(LCX, y, 80, 18), 12)];
        // Plain NSTextField (not NSSecureTextField) — paste via Cmd+V or
        // right-click works without quirks, and the user can visually confirm
        // they pasted the right thing.
        ctrl->apiKeyField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(LCX + 90, y - 4, LCW - 90, 24)];
        [ctrl->apiKeyField setStringValue:
            [NSString stringWithUTF8String:config.api_key.c_str()]];
        [ctrl->apiKeyField setPlaceholderString:@"Paste your API key"];
        [[ctrl->apiKeyField cell] setUsesSingleLineMode:YES];
        [root addSubview:ctrl->apiKeyField];
        y -= 38;

        // Base URL — visible only when "custom" is selected
        ctrl->baseUrlLabel = MakeLabel(@"Base URL", NSMakeRect(LCX, y, 80, 18), 12);
        [root addSubview:ctrl->baseUrlLabel];
        ctrl->baseUrlField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(LCX + 90, y - 4, LCW - 90, 24)];
        [ctrl->baseUrlField setStringValue:
            [NSString stringWithUTF8String:config.base_url.c_str()]];
        [ctrl->baseUrlField setPlaceholderString:@"https://api.example.com/v1"];
        [root addSubview:ctrl->baseUrlField];
        BOOL isCustom = (config.provider == "custom");
        [ctrl->baseUrlField setHidden:!isCustom];
        [ctrl->baseUrlLabel setHidden:!isCustom];
        y -= 38;

        [root addSubview:MakeLabel(@"Gemini fallback", NSMakeRect(LCX, y, 110, 18), 12)];
        ctrl->fallbackField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(LCX + 120, y - 4, LCW - 120, 24)];
        [ctrl->fallbackField setStringValue:
            [NSString stringWithUTF8String:config.gemini_fallback_key.c_str()]];
        [ctrl->fallbackField setPlaceholderString:@"Optional — Gemini key for audio"];
        [[ctrl->fallbackField cell] setUsesSingleLineMode:YES];
        [root addSubview:ctrl->fallbackField];
        y -= 38;

        [root addSubview:MakeLabel(@"Theme", NSMakeRect(LCX, y, 80, 18), 12)];
        ctrl->themeBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(LCX + 90, y - 5, 200, 26) pullsDown:NO];
        [ctrl->themeBtn addItemsWithTitles:@[@"dark", @"light", @"contrast"]];
        NSString* themeNs = [NSString stringWithUTF8String:config.theme.c_str()];
        if ([ctrl->themeBtn indexOfItemWithTitle:themeNs] >= 0) {
            [ctrl->themeBtn selectItemWithTitle:themeNs];
        }
        [root addSubview:ctrl->themeBtn];
        y -= 38;

        ctrl->micCheckbox = [[NSButton alloc]
            initWithFrame:NSMakeRect(LCX, y, LCW, 22)];
        [ctrl->micCheckbox setButtonType:NSButtonTypeSwitch];
        [ctrl->micCheckbox setTitle:@"Also capture microphone (your voice)"];
        [ctrl->micCheckbox setState:(config.capture_mic ? NSControlStateValueOn : NSControlStateValueOff)];
        [root addSubview:ctrl->micCheckbox];
        y -= 30;

        ctrl->soundCheckbox = [[NSButton alloc]
            initWithFrame:NSMakeRect(LCX, y, LCW, 22)];
        [ctrl->soundCheckbox setButtonType:NSButtonTypeSwitch];
        [ctrl->soundCheckbox setTitle:@"Play a sound when auto-answer fires"];
        [ctrl->soundCheckbox setState:(config.sound_on_auto ? NSControlStateValueOn : NSControlStateValueOff)];
        [root addSubview:ctrl->soundCheckbox];

        // ---- RIGHT COLUMN: Hotkey rebinder ----
        NSTextField* hkHeader = MakeLabel(@"Hotkeys",
                                          NSMakeRect(RCX, H - 120, RCW, 22), 15);
        [hkHeader setFont:[NSFont boldSystemFontOfSize:15]];
        [root addSubview:hkHeader];

        NSTextField* hkSub = MakeLabel(
            @"Click any binding to record. Press Esc to cancel.",
            NSMakeRect(RCX, H - 140, RCW, 16), 11);
        [hkSub setTextColor:[NSColor secondaryLabelColor]];
        [root addSubview:hkSub];

        CGFloat ry = H - 168;
        const CGFloat rowH = 28;
        for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
            NSString* label = [NSString stringWithUTF8String:
                ConfigLoader::ActionLabel((HotkeyAction)i)];
            [root addSubview:MakeLabel(label,
                NSMakeRect(RCX, ry, 180, 18), 12)];

            RebindButton* btn = [[RebindButton alloc]
                initWithFrame:NSMakeRect(RCX + 180, ry - 5, RCW - 180, 26)];
            btn->actionIndex = i;
            btn->owner = ctrl;
            [btn setBezelStyle:NSBezelStyleRoundRect];
            [btn setTitle:[NSString stringWithUTF8String:
                ConfigLoader::BindingToString(config.hotkeys.bindings[i]).c_str()]];
            [btn setTarget:btn];
            [btn setAction:@selector(beginCapture)];
            [root addSubview:btn];
            [ctrl->rebindButtons addObject:btn];

            (void)RB;
            ry -= rowH;
        }

        // Reset hotkeys button
        NSButton* resetBtn = [[NSButton alloc]
            initWithFrame:NSMakeRect(RCX, ry - 10, RCW, 24)];
        [resetBtn setBezelStyle:NSBezelStyleRoundRect];
        [resetBtn setTitle:@"Reset hotkeys to Mac defaults"];
        [resetBtn setTarget:ctrl];
        [resetBtn setAction:@selector(resetHotkeysPressed:)];
        [root addSubview:resetBtn];

        // ---- BOTTOM: status + action buttons ----
        [root addSubview:MakeDivider(NSMakeRect(LCX, 78, W - 40, 1))];

        NSTextField* help = MakeLabel(
            @"First use: grant Screen Recording in System Settings → Privacy & Security.\n"
            @"The overlay window is invisible to screen recording (NSWindowSharingNone).",
            NSMakeRect(LCX, 36, W - 200, 36), 11);
        [help setTextColor:[NSColor secondaryLabelColor]];
        [root addSubview:help];

        ctrl->statusLabel = MakeLabel(@"", NSMakeRect(LCX, 14, W - 240, 18), 12);
        [root addSubview:ctrl->statusLabel];

        NSButton* cancel = [[NSButton alloc] initWithFrame:NSMakeRect(W - 220, 12, 90, 32)];
        [cancel setTitle:@"Cancel"];
        [cancel setBezelStyle:NSBezelStyleRounded];
        [cancel setTarget:ctrl];
        [cancel setAction:@selector(cancelPressed:)];
        [root addSubview:cancel];

        NSButton* start = [[NSButton alloc] initWithFrame:NSMakeRect(W - 120, 12, 100, 32)];
        [start setTitle:@"Start Overlay"];
        [start setBezelStyle:NSBezelStyleRounded];
        [start setKeyEquivalent:@"\r"];
        [start setTarget:ctrl];
        [start setAction:@selector(startPressed:)];
        [root addSubview:start];

        [ctrl->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp runModalForWindow:ctrl->window];

        return ctrl->accepted == YES;
    }
}

}  // namespace MacConfigDialog
