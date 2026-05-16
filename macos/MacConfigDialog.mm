#import <Cocoa/Cocoa.h>

#include "../ConfigLoader.h"

#include <string>
#include <vector>

// Welcome / configuration dialog (macOS port of ConfigDialog.cpp).
//
// This is the minimal viable settings sheet — provider + model + API key +
// optional Gemini fallback key. The full Win32 dialog has hotkey rebinder,
// device pickers, session manager, theme picker, and a dozen other knobs;
// those are deferred to a follow-up commit. Users can edit llm_config.txt
// directly until then.

@interface ConfigDialogController : NSObject {
@public
    NSWindow* window;
    NSPopUpButton* providerBtn;
    NSPopUpButton* modelBtn;
    NSSecureTextField* apiKeyField;
    NSSecureTextField* fallbackField;
    NSTextField* baseUrlField;
    NSTextField* baseUrlLabel;
    NSButton* micCheckbox;
    NSPopUpButton* themeBtn;
    BOOL accepted;
    LLMConfig* config;
    const std::vector<ModelInfo>* models;
}
- (void)providerChanged:(id)sender;
- (void)startPressed:(id)sender;
- (void)cancelPressed:(id)sender;
- (void)populateModelsForProvider:(const std::string&)pid;
@end

@implementation ConfigDialogController

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

- (std::string)currentProviderId {
    NSInteger idx = [providerBtn indexOfSelectedItem];
    const auto& provs = ConfigLoader::BuiltinProviders();
    if (idx < 0 || (NSUInteger)idx >= provs.size()) return "gemini";
    return provs[idx].id;
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
    // For Gemini, also offer entries from models_list.txt
    if (pid == "gemini" && models) {
        for (const auto& m : *models) {
            NSString* label = [NSString stringWithUTF8String:
                (m.id + " — " + m.name).c_str()];
            // Avoid dupes
            if ([modelBtn indexOfItemWithTitle:label] == -1) {
                [modelBtn addItemWithTitle:label];
            }
        }
    }
}

- (void)startPressed:(id)sender {
    (void)sender;
    accepted = YES;

    config->provider = self.currentProviderId;
    NSString* modelTitle = [modelBtn titleOfSelectedItem];
    if (modelTitle) {
        std::string m = [modelTitle UTF8String];
        size_t dash = m.find(" — ");
        if (dash != std::string::npos) m = m.substr(0, dash);
        config->model = m;
    }
    config->api_key = [[apiKeyField stringValue] UTF8String];
    config->gemini_fallback_key = [[fallbackField stringValue] UTF8String];
    if (config->provider == "custom") {
        config->base_url = [[baseUrlField stringValue] UTF8String];
    }
    config->capture_mic = ([micCheckbox state] == NSControlStateValueOn);
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

@end

namespace MacConfigDialog {

static NSTextField* MakeLabel(NSString* text, NSRect frame) {
    NSTextField* lbl = [[NSTextField alloc] initWithFrame:frame];
    [lbl setStringValue:text];
    [lbl setBezeled:NO];
    [lbl setDrawsBackground:NO];
    [lbl setEditable:NO];
    [lbl setSelectable:NO];
    [lbl setFont:[NSFont systemFontOfSize:12]];
    return lbl;
}

bool Show(LLMConfig& config, const std::vector<ModelInfo>& models) {
    @autoreleasepool {
        ConfigDialogController* ctrl = [[ConfigDialogController alloc] init];
        ctrl->config = &config;
        ctrl->models = &models;
        ctrl->accepted = NO;

        NSRect frame = NSMakeRect(0, 0, 520, 440);
        ctrl->window = [[NSWindow alloc] initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
            backing:NSBackingStoreBuffered
            defer:NO];
        [ctrl->window setTitle:@"Invisible AI Overlay — Setup"];
        [ctrl->window center];
        [ctrl->window setReleasedWhenClosed:NO];

        NSView* root = [ctrl->window contentView];

        // Title / brand
        NSTextField* brand = MakeLabel(@"✨ Invisible AI Overlay",
                                       NSMakeRect(20, 400, 480, 24));
        [brand setFont:[NSFont boldSystemFontOfSize:18]];
        [brand setTextColor:[NSColor controlTextColor]];
        [root addSubview:brand];

        NSTextField* tagline = MakeLabel(@"Configure your LLM provider to start the overlay.",
                                         NSMakeRect(20, 376, 480, 18));
        [tagline setTextColor:[NSColor secondaryLabelColor]];
        [root addSubview:tagline];

        // Provider
        [root addSubview:MakeLabel(@"Provider:", NSMakeRect(20, 340, 100, 20))];
        ctrl->providerBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(120, 336, 380, 26) pullsDown:NO];
        for (const auto& p : ConfigLoader::BuiltinProviders()) {
            [ctrl->providerBtn addItemWithTitle:
                [NSString stringWithUTF8String:p.name.c_str()]];
        }
        // Select current
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

        // Model
        [root addSubview:MakeLabel(@"Model:", NSMakeRect(20, 300, 100, 20))];
        ctrl->modelBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(120, 296, 380, 26) pullsDown:NO];
        [root addSubview:ctrl->modelBtn];
        [ctrl populateModelsForProvider:config.provider];

        // API Key
        [root addSubview:MakeLabel(@"API Key:", NSMakeRect(20, 260, 100, 20))];
        ctrl->apiKeyField = [[NSSecureTextField alloc]
            initWithFrame:NSMakeRect(120, 256, 380, 24)];
        [ctrl->apiKeyField setStringValue:
            [NSString stringWithUTF8String:config.api_key.c_str()]];
        [ctrl->apiKeyField setPlaceholderString:@"Paste your API key"];
        [root addSubview:ctrl->apiKeyField];

        // Custom base URL (visible only when provider == custom)
        ctrl->baseUrlLabel = MakeLabel(@"Base URL:", NSMakeRect(20, 224, 100, 20));
        [root addSubview:ctrl->baseUrlLabel];
        ctrl->baseUrlField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(120, 220, 380, 24)];
        [ctrl->baseUrlField setStringValue:
            [NSString stringWithUTF8String:config.base_url.c_str()]];
        [ctrl->baseUrlField setPlaceholderString:@"https://api.example.com/v1"];
        [root addSubview:ctrl->baseUrlField];
        BOOL isCustom = (config.provider == "custom");
        [ctrl->baseUrlField setHidden:!isCustom];
        [ctrl->baseUrlLabel setHidden:!isCustom];

        // Gemini fallback key (for audio when provider isn't Gemini)
        [root addSubview:MakeLabel(@"Gemini fallback (audio):",
                                   NSMakeRect(20, 188, 180, 20))];
        ctrl->fallbackField = [[NSSecureTextField alloc]
            initWithFrame:NSMakeRect(200, 184, 300, 24)];
        [ctrl->fallbackField setStringValue:
            [NSString stringWithUTF8String:config.gemini_fallback_key.c_str()]];
        [ctrl->fallbackField setPlaceholderString:@"Optional — Gemini key for audio capture"];
        [root addSubview:ctrl->fallbackField];

        // Mic checkbox
        ctrl->micCheckbox = [[NSButton alloc]
            initWithFrame:NSMakeRect(20, 150, 480, 24)];
        [ctrl->micCheckbox setButtonType:NSButtonTypeSwitch];
        [ctrl->micCheckbox setTitle:@"Also capture microphone (your voice)"];
        [ctrl->micCheckbox setState:(config.capture_mic ? NSControlStateValueOn : NSControlStateValueOff)];
        [root addSubview:ctrl->micCheckbox];

        // Theme
        [root addSubview:MakeLabel(@"Theme:", NSMakeRect(20, 116, 100, 20))];
        ctrl->themeBtn = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(120, 112, 200, 26) pullsDown:NO];
        [ctrl->themeBtn addItemsWithTitles:@[@"dark", @"light", @"contrast"]];
        NSString* themeNs = [NSString stringWithUTF8String:config.theme.c_str()];
        if ([ctrl->themeBtn indexOfItemWithTitle:themeNs] >= 0) {
            [ctrl->themeBtn selectItemWithTitle:themeNs];
        }
        [root addSubview:ctrl->themeBtn];

        // Help text
        NSTextField* help = MakeLabel(
            @"Hotkeys (defaults): F8 screen · F7 audio · INS clipboard · DEL hide · END exit\n"
            @"This window is invisible to screen recording (NSWindowSharingNone).",
            NSMakeRect(20, 60, 480, 36));
        [help setTextColor:[NSColor secondaryLabelColor]];
        [help setFont:[NSFont systemFontOfSize:11]];
        [root addSubview:help];

        // Buttons
        NSButton* cancel = [[NSButton alloc] initWithFrame:NSMakeRect(300, 14, 90, 32)];
        [cancel setTitle:@"Cancel"];
        [cancel setBezelStyle:NSBezelStyleRounded];
        [cancel setTarget:ctrl];
        [cancel setAction:@selector(cancelPressed:)];
        [root addSubview:cancel];

        NSButton* start = [[NSButton alloc] initWithFrame:NSMakeRect(400, 14, 100, 32)];
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
