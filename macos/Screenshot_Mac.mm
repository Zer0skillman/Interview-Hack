#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "../IScreenshot.h"

#include <memory>
#include <string>

// macOS screenshot implementation.
//
// CGDisplayCreateImage was removed in macOS 15. We use SCScreenshotManager
// (macOS 14+), which is async with a completion handler — we block the
// calling thread with a dispatch_semaphore so the IScreenshot signature
// (synchronous, returns std::string) keeps working.
//
// The user has to grant Screen Recording permission in System Settings the
// first time this runs. Without it, the call returns nil. Our own overlay
// uses NSWindowSharingNone so it doesn't appear in the screenshot (mirror of
// WDA_EXCLUDEFROMCAPTURE on Windows).

namespace {

class MacScreenshot : public IScreenshot {
public:
    std::string CaptureMonitorUnderCursorAsBase64Png() override {
        @autoreleasepool {
            // 1) Pick the display containing the cursor (mirrors Win
            //    MonitorFromPoint/MONITOR_DEFAULTTOPRIMARY behavior).
            NSPoint cursor = [NSEvent mouseLocation];
            NSScreen* target = nil;
            for (NSScreen* s in [NSScreen screens]) {
                if (NSPointInRect(cursor, [s frame])) { target = s; break; }
            }
            if (!target) target = [NSScreen mainScreen];
            if (!target) return std::string();

            CGDirectDisplayID displayID =
                (CGDirectDisplayID)[[[target deviceDescription]
                    objectForKey:@"NSScreenNumber"] unsignedIntValue];

            // 2) Get shareable content + find the matching SCDisplay.
            __block SCShareableContent* content = nil;
            __block NSError* contentErr = nil;
            dispatch_semaphore_t sem1 = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentWithCompletionHandler:^(
                SCShareableContent* c, NSError* e) {
                content = c;
                contentErr = e;
                dispatch_semaphore_signal(sem1);
            }];
            dispatch_semaphore_wait(sem1, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            if (!content) {
                NSLog(@"Screenshot: shareable content unavailable (%@)", contentErr);
                return std::string();
            }

            SCDisplay* scDisplay = nil;
            for (SCDisplay* d in content.displays) {
                if (d.displayID == displayID) { scDisplay = d; break; }
            }
            if (!scDisplay && content.displays.count > 0) {
                scDisplay = content.displays.firstObject;
            }
            if (!scDisplay) return std::string();

            // 3) Snap an image at native resolution.
            SCContentFilter* filter = [[SCContentFilter alloc]
                initWithDisplay:scDisplay excludingWindows:@[]];

            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.width = (size_t)(scDisplay.width * target.backingScaleFactor);
            cfg.height = (size_t)(scDisplay.height * target.backingScaleFactor);
            cfg.showsCursor = YES;

            __block CGImageRef img = NULL;
            __block NSError* shotErr = nil;
            dispatch_semaphore_t sem2 = dispatch_semaphore_create(0);
            [SCScreenshotManager captureImageWithFilter:filter
                                          configuration:cfg
                                      completionHandler:^(CGImageRef i, NSError* e) {
                if (i) img = (CGImageRef)CFRetain(i);
                shotErr = e;
                dispatch_semaphore_signal(sem2);
            }];
            dispatch_semaphore_wait(sem2, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

            if (!img) {
                NSLog(@"Screenshot: capture failed (%@)", shotErr);
                return std::string();
            }

            // 4) PNG-encode via ImageIO.
            NSMutableData* data = [NSMutableData data];
            CGImageDestinationRef dst = CGImageDestinationCreateWithData(
                (__bridge CFMutableDataRef)data,
                (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
            if (!dst) { CFRelease(img); return std::string(); }
            CGImageDestinationAddImage(dst, img, NULL);
            bool ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
            CFRelease(img);
            if (!ok) return std::string();

            NSString* b64 = [data base64EncodedStringWithOptions:0];
            return std::string([b64 UTF8String]);
        }
    }
};

}  // namespace

std::unique_ptr<IScreenshot> CreateScreenshot() {
    return std::make_unique<MacScreenshot>();
}
