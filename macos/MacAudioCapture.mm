#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>

#include "../IAudioCapture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// macOS audio capture.
//
// System audio (the "loopback" that powers the meeting-audio capture) goes
// through ScreenCaptureKit (macOS 13+), which captures display audio with
// user-granted Screen Recording permission. The mic, when enabled, comes from
// AVCaptureSession's default audio input.
//
// Both streams downsample to 16 kHz mono float and mix into a ring buffer
// matching the Windows impl's contract (60 seconds, kTargetRate = 16000).

class MacAudioCapture;

// -----------------------------------------------------------------------------
// Objective-C helper: ScreenCaptureKit stream delegate
// -----------------------------------------------------------------------------

API_AVAILABLE(macos(13.0))
@interface AudioStreamDelegate : NSObject <SCStreamDelegate, SCStreamOutput>
{
    @public
    MacAudioCapture* owner;
}
@end

API_AVAILABLE(macos(13.0))
@interface MicCaptureDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
{
    @public
    MacAudioCapture* owner;
}
@end

// -----------------------------------------------------------------------------
// C++ class
// -----------------------------------------------------------------------------

class MacAudioCapture : public IAudioCapture {
public:
    MacAudioCapture();
    ~MacAudioCapture() override;

    bool  Start(bool withMic,
                const std::string& loopbackDeviceId,
                const std::string& micDeviceId) override;
    void  Stop() override;
    std::string SnapshotAsBase64Wav(int seconds) override;
    float RecentEnergy(int seconds) override;

    // Called from the Obj-C delegates on background queues.
    void AppendLoopbackSamples(const float* mono, size_t count);
    void MixMicSamples(const float* mono, size_t count);

private:
    static constexpr int kTargetRate = 16000;
    static constexpr int kMaxSeconds = 60;

    std::vector<float> m_ring;
    size_t             m_writePos = 0;
    size_t             m_totalWritten = 0;
    std::mutex         m_mutex;

    std::atomic<bool>  m_running{false};
    bool               m_withMic = false;

    // ARC-managed Obj-C objects held via __strong via a void*+bridge_retained
    void* m_scStream = nullptr;   // SCStream*
    void* m_scDelegate = nullptr; // AudioStreamDelegate*
    void* m_captureSession = nullptr; // AVCaptureSession*
    void* m_micDelegate = nullptr;    // MicCaptureDelegate*

    // For PCM conversion
    AudioConverterRef m_loopbackConverter = nullptr;
    AudioConverterRef m_micConverter = nullptr;
};

// -----------------------------------------------------------------------------
// Sample buffer → mono 16 kHz float
// -----------------------------------------------------------------------------

namespace {

// Convert a CMSampleBufferRef into mono float samples at 16 kHz. The source
// can be any sample rate / channel count / format SCK or AVFoundation gives us.
// Uses AudioConverter for sample rate conversion + de-interleaving.
static std::vector<float> SampleBufferToMono16k(CMSampleBufferRef sampleBuffer,
                                                AudioConverterRef* cachedConverter)
{
    std::vector<float> out;
    if (!sampleBuffer) return out;

    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!fmt) return out;

    const AudioStreamBasicDescription* srcAsbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
    if (!srcAsbd) return out;

    CMItemCount numSamples = CMSampleBufferGetNumSamples(sampleBuffer);
    if (numSamples == 0) return out;

    // Get the data as a contiguous block of source samples
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!block) return out;

    size_t srcLen = 0;
    char*  srcPtr = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, NULL, &srcLen, &srcPtr) != kCMBlockBufferNoErr) {
        return out;
    }
    if (!srcPtr || srcLen == 0) return out;

    // Set up the target format: 16 kHz mono float32 interleaved
    AudioStreamBasicDescription dstAsbd{};
    dstAsbd.mSampleRate       = 16000.0;
    dstAsbd.mFormatID         = kAudioFormatLinearPCM;
    dstAsbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    dstAsbd.mChannelsPerFrame = 1;
    dstAsbd.mBitsPerChannel   = 32;
    dstAsbd.mBytesPerFrame    = 4;
    dstAsbd.mBytesPerPacket   = 4;
    dstAsbd.mFramesPerPacket  = 1;

    // (Re)create converter when the source format changes. For SCK these
    // settings are stable across the stream lifetime so the cached converter
    // is reused after the first sample buffer.
    if (*cachedConverter == nullptr) {
        if (AudioConverterNew(srcAsbd, &dstAsbd, cachedConverter) != noErr) {
            return out;
        }
    }

    // Estimate output capacity. Source frames * (16000/srcRate) + slack.
    UInt32 srcFrames = (UInt32)numSamples;
    double ratio = 16000.0 / srcAsbd->mSampleRate;
    UInt32 dstCapacityFrames = (UInt32)((double)srcFrames * ratio) + 64;
    out.resize(dstCapacityFrames);

    AudioBufferList dstBufs{};
    dstBufs.mNumberBuffers = 1;
    dstBufs.mBuffers[0].mNumberChannels = 1;
    dstBufs.mBuffers[0].mDataByteSize = dstCapacityFrames * 4;
    dstBufs.mBuffers[0].mData = out.data();

    // Provide source data via a one-shot callback that returns the whole block once.
    struct InputCtx {
        AudioBufferList srcBufs;
        AudioStreamBasicDescription srcAsbd;
        UInt32 framesRemaining;
        char*  data;
        bool   consumed;
    };
    InputCtx ctx{};
    ctx.srcAsbd = *srcAsbd;
    ctx.framesRemaining = srcFrames;
    ctx.data = srcPtr;
    ctx.consumed = false;
    ctx.srcBufs.mNumberBuffers = 1;
    ctx.srcBufs.mBuffers[0].mNumberChannels = srcAsbd->mChannelsPerFrame;
    ctx.srcBufs.mBuffers[0].mDataByteSize = (UInt32)srcLen;
    ctx.srcBufs.mBuffers[0].mData = srcPtr;

    auto inputProc = [](AudioConverterRef, UInt32* ioNumberDataPackets,
                        AudioBufferList* ioData, AudioStreamPacketDescription**,
                        void* inUserData) -> OSStatus {
        auto* c = static_cast<InputCtx*>(inUserData);
        if (c->consumed) {
            *ioNumberDataPackets = 0;
            ioData->mBuffers[0].mData = nullptr;
            ioData->mBuffers[0].mDataByteSize = 0;
            return noErr;
        }
        UInt32 framesToGive = std::min<UInt32>(*ioNumberDataPackets, c->framesRemaining);
        UInt32 bytes = framesToGive * c->srcAsbd.mBytesPerFrame;
        ioData->mNumberBuffers = c->srcBufs.mNumberBuffers;
        ioData->mBuffers[0].mNumberChannels = c->srcBufs.mBuffers[0].mNumberChannels;
        ioData->mBuffers[0].mDataByteSize   = bytes;
        ioData->mBuffers[0].mData           = c->data;
        *ioNumberDataPackets = framesToGive;
        c->consumed = true;
        return noErr;
    };

    UInt32 outFrames = dstCapacityFrames;
    OSStatus st = AudioConverterFillComplexBuffer(*cachedConverter, inputProc, &ctx,
                                                  &outFrames, &dstBufs, NULL);
    if (st != noErr) {
        out.clear();
        return out;
    }

    out.resize(outFrames);
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// Obj-C delegate impls
// -----------------------------------------------------------------------------

@implementation AudioStreamDelegate

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeAudio || !owner) return;
    // Run on whatever queue SCK delivers — appending is mutex-guarded.
    static AudioConverterRef converter = nullptr;
    std::vector<float> mono = SampleBufferToMono16k(sampleBuffer, &converter);
    if (!mono.empty()) {
        owner->AppendLoopbackSamples(mono.data(), mono.size());
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream; (void)error;
}

@end

@implementation MicCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output; (void)connection;
    if (!owner) return;
    static AudioConverterRef converter = nullptr;
    std::vector<float> mono = SampleBufferToMono16k(sampleBuffer, &converter);
    if (!mono.empty()) {
        owner->MixMicSamples(mono.data(), mono.size());
    }
}

@end

// -----------------------------------------------------------------------------
// MacAudioCapture impl
// -----------------------------------------------------------------------------

MacAudioCapture::MacAudioCapture() {
    m_ring.assign((size_t)kTargetRate * kMaxSeconds, 0.0f);
}

MacAudioCapture::~MacAudioCapture() {
    Stop();
}

bool MacAudioCapture::Start(bool withMic,
                            const std::string& /*loopbackDeviceId*/,
                            const std::string& /*micDeviceId*/)
{
    if (m_running.exchange(true)) return true;
    m_withMic = withMic;

    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            // 1) Start system-audio capture via ScreenCaptureKit
            AudioStreamDelegate* del = [[AudioStreamDelegate alloc] init];
            del->owner = this;
            m_scDelegate = (__bridge_retained void*)del;

            // Discover shareable content. The completion is async; we have to
            // block until it's done so Start() can return a meaningful bool.
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block SCShareableContent* content = nil;
            __block NSError* contentErr = nil;
            [SCShareableContent getShareableContentWithCompletionHandler:^(
                SCShareableContent* c, NSError* e) {
                content = c;
                contentErr = e;
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

            if (!content || content.displays.count == 0) {
                NSLog(@"SCShareableContent failed: %@", contentErr);
                // Permission missing or denied. Don't crash — we'll silently
                // produce zero audio until the user grants permission.
                return true;
            }

            SCDisplay* display = content.displays.firstObject;
            SCContentFilter* filter = [[SCContentFilter alloc]
                initWithDisplay:display excludingWindows:@[]];

            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.capturesAudio = YES;
            cfg.sampleRate = 48000;
            cfg.channelCount = 2;
            // Minimize video work — we don't need video, but SCStream needs
            // a valid video config. Tiny size + low frame rate.
            cfg.width = 2;
            cfg.height = 2;
            cfg.minimumFrameInterval = CMTimeMake(1, 1);
            cfg.queueDepth = 6;

            SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                                  configuration:cfg
                                                       delegate:del];
            NSError* err = nil;
            dispatch_queue_t audioQ = dispatch_queue_create("ai.overlay.audio", DISPATCH_QUEUE_SERIAL);
            if (![stream addStreamOutput:del
                                    type:SCStreamOutputTypeAudio
                      sampleHandlerQueue:audioQ
                                   error:&err]) {
                NSLog(@"addStreamOutput failed: %@", err);
            }

            __block BOOL startedOk = NO;
            dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
            [stream startCaptureWithCompletionHandler:^(NSError* e) {
                startedOk = (e == nil);
                if (e) NSLog(@"SCStream startCapture failed: %@", e);
                dispatch_semaphore_signal(startSem);
            }];
            dispatch_semaphore_wait(startSem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

            m_scStream = (__bridge_retained void*)stream;
            (void)startedOk;
        }
    }

    // 2) Optional mic capture via AVCaptureSession
    if (m_withMic) {
        @autoreleasepool {
            AVCaptureSession* session = [[AVCaptureSession alloc] init];
            AVCaptureDevice* mic = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
            if (!mic) {
                NSLog(@"No default mic available");
            } else {
                NSError* err = nil;
                AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:mic error:&err];
                if (input && [session canAddInput:input]) {
                    [session addInput:input];

                    MicCaptureDelegate* mdel = [[MicCaptureDelegate alloc] init];
                    mdel->owner = this;

                    AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
                    dispatch_queue_t micQ = dispatch_queue_create("ai.overlay.mic", DISPATCH_QUEUE_SERIAL);
                    [output setSampleBufferDelegate:mdel queue:micQ];
                    if ([session canAddOutput:output]) {
                        [session addOutput:output];
                    }
                    [session startRunning];

                    m_captureSession = (__bridge_retained void*)session;
                    m_micDelegate = (__bridge_retained void*)mdel;
                }
            }
        }
    }

    return true;
}

void MacAudioCapture::Stop() {
    if (!m_running.exchange(false)) return;

    if (@available(macOS 13.0, *)) {
        if (m_scStream) {
            SCStream* stream = (__bridge_transfer SCStream*)m_scStream;
            m_scStream = nullptr;
            [stream stopCaptureWithCompletionHandler:^(NSError*) {}];
        }
    }
    if (m_scDelegate) {
        if (@available(macOS 13.0, *)) {
            AudioStreamDelegate* del = (__bridge_transfer AudioStreamDelegate*)m_scDelegate;
            del->owner = nullptr;
        }
        m_scDelegate = nullptr;
    }

    if (m_captureSession) {
        AVCaptureSession* session = (__bridge_transfer AVCaptureSession*)m_captureSession;
        m_captureSession = nullptr;
        [session stopRunning];
    }
    if (m_micDelegate) {
        if (@available(macOS 13.0, *)) {
            MicCaptureDelegate* mdel = (__bridge_transfer MicCaptureDelegate*)m_micDelegate;
            mdel->owner = nullptr;
        }
        m_micDelegate = nullptr;
    }

    if (m_loopbackConverter) {
        AudioConverterDispose(m_loopbackConverter);
        m_loopbackConverter = nullptr;
    }
    if (m_micConverter) {
        AudioConverterDispose(m_micConverter);
        m_micConverter = nullptr;
    }
}

void MacAudioCapture::AppendLoopbackSamples(const float* mono, size_t count) {
    if (count == 0) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    const size_t cap = m_ring.size();
    for (size_t i = 0; i < count; ++i) {
        m_ring[m_writePos] = mono[i];
        m_writePos = (m_writePos + 1) % cap;
        m_totalWritten++;
    }
}

void MacAudioCapture::MixMicSamples(const float* mono, size_t count) {
    if (count == 0) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    const size_t cap = m_ring.size();
    size_t pos = (m_writePos + cap - count) % cap;
    for (size_t i = 0; i < count; ++i) {
        float v = m_ring[pos] + mono[i] * 0.7f;  // match Windows attenuation
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        m_ring[pos] = v;
        pos = (pos + 1) % cap;
    }
}

float MacAudioCapture::RecentEnergy(int seconds) {
    if (seconds <= 0) return 0.0f;
    std::lock_guard<std::mutex> lk(m_mutex);
    const size_t cap  = m_ring.size();
    const size_t want = (size_t)seconds * kTargetRate;
    const size_t have = std::min<size_t>(m_totalWritten, std::min(want, cap));
    if (have == 0) return 0.0f;

    size_t start = (m_writePos + cap - have) % cap;
    double sum = 0.0;
    for (size_t i = 0; i < have; ++i) {
        float s = m_ring[(start + i) % cap];
        sum += (double)s * (double)s;
    }
    double rms = std::sqrt(sum / (double)have);
    return (float)std::min(rms, 1.0);
}

// -----------------------------------------------------------------------------
// WAV + base64 snapshot (same RIFF layout as the Windows impl)
// -----------------------------------------------------------------------------

namespace {

static void Put16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}
static void Put32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

static std::string Base64(const uint8_t* data, size_t len) {
    NSData* d = [NSData dataWithBytes:data length:len];
    NSString* s = [d base64EncodedStringWithOptions:0];
    return std::string([s UTF8String]);
}

}  // namespace

std::string MacAudioCapture::SnapshotAsBase64Wav(int seconds) {
    if (seconds <= 0) return std::string();
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;

    std::vector<float> mono;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const size_t cap  = m_ring.size();
        const size_t want = (size_t)seconds * kTargetRate;
        const size_t have = std::min<size_t>(m_totalWritten, std::min(want, cap));
        if (have == 0) return std::string();

        mono.resize(have);
        size_t start = (m_writePos + cap - have) % cap;
        for (size_t i = 0; i < have; ++i) {
            mono[i] = m_ring[(start + i) % cap];
        }
    }

    std::vector<int16_t> pcm(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) {
        float v = mono[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)(v * 32767.0f);
    }

    const uint32_t sampleRate    = (uint32_t)kTargetRate;
    const uint16_t numChannels   = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate      = sampleRate * numChannels * bitsPerSample / 8;
    const uint16_t blockAlign    = numChannels * bitsPerSample / 8;
    const uint32_t dataBytes     = (uint32_t)(pcm.size() * sizeof(int16_t));
    const uint32_t riffSize      = 36 + dataBytes;

    std::vector<uint8_t> wav;
    wav.reserve(44 + dataBytes);

    auto putStr = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) wav.push_back((uint8_t)s[i]);
    };

    putStr("RIFF", 4);
    Put32LE(wav, riffSize);
    putStr("WAVE", 4);
    putStr("fmt ", 4);
    Put32LE(wav, 16);
    Put16LE(wav, 1);
    Put16LE(wav, numChannels);
    Put32LE(wav, sampleRate);
    Put32LE(wav, byteRate);
    Put16LE(wav, blockAlign);
    Put16LE(wav, bitsPerSample);
    putStr("data", 4);
    Put32LE(wav, dataBytes);

    const uint8_t* pcmBytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pcmBytes, pcmBytes + dataBytes);

    return Base64(wav.data(), wav.size());
}

// -----------------------------------------------------------------------------
// Factory + device enumeration
// -----------------------------------------------------------------------------

std::unique_ptr<IAudioCapture> CreateAudioCapture() {
    return std::make_unique<MacAudioCapture>();
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool flowIsRender) {
    std::vector<AudioDeviceInfo> out;
    @autoreleasepool {
        // Get the device ID list
        UInt32 dataSize = 0;
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &dataSize) != noErr)
            return out;

        UInt32 count = dataSize / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> ids(count);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                       &dataSize, ids.data()) != noErr) {
            return out;
        }

        const AudioObjectPropertyScope scope = flowIsRender
            ? kAudioDevicePropertyScopeOutput
            : kAudioDevicePropertyScopeInput;

        for (AudioDeviceID id : ids) {
            // Skip devices without channels for this scope (input or output)
            AudioObjectPropertyAddress streamAddr = {
                kAudioDevicePropertyStreams,
                scope,
                kAudioObjectPropertyElementMain
            };
            UInt32 streamsSize = 0;
            if (AudioObjectGetPropertyDataSize(id, &streamAddr, 0, NULL, &streamsSize) != noErr
                || streamsSize == 0) {
                continue;
            }

            // UID (stable id)
            CFStringRef uidRef = NULL;
            UInt32 uidSize = sizeof(uidRef);
            AudioObjectPropertyAddress uidAddr = {
                kAudioDevicePropertyDeviceUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(id, &uidAddr, 0, NULL, &uidSize, &uidRef) != noErr) {
                continue;
            }

            // Friendly name
            CFStringRef nameRef = NULL;
            UInt32 nameSize = sizeof(nameRef);
            AudioObjectPropertyAddress nameAddr = {
                kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(id, &nameAddr, 0, NULL, &nameSize, &nameRef) != noErr) {
                CFRelease(uidRef);
                continue;
            }

            AudioDeviceInfo info;
            info.id   = std::string([(__bridge NSString*)uidRef UTF8String]);
            info.name = std::string([(__bridge NSString*)nameRef UTF8String]);
            CFRelease(uidRef);
            CFRelease(nameRef);
            if (!info.id.empty()) out.push_back(info);
        }
    }
    return out;
}
