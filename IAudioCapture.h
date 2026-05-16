#pragma once
#include <string>
#include <vector>
#include <memory>

// Cross-platform audio capture interface.
//
// Implementations:
//   - WindowsAudioCapture (in AudioCapture.cpp) — WASAPI loopback + mic
//   - MacAudioCapture     (forthcoming, macos/MacAudioCapture.mm) — ScreenCaptureKit + CoreAudio
//
// CreateAudioCapture() returns the platform-appropriate impl. The factory is
// the only place that knows which OS we're on; everything else (OverlayWindow)
// holds the interface type and stays platform-agnostic.

struct AudioDeviceInfo {
    std::string id;     // UTF-8 device ID, opaque per-platform string
    std::string name;   // friendly name (e.g. "Speakers (Realtek)")
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    // Begin capture. Loopback is always captured; mic is optional.
    // Device IDs are platform-specific opaque strings; empty = default.
    virtual bool Start(bool withMic,
                       const std::string& loopbackDeviceId,
                       const std::string& micDeviceId) = 0;

    // Stop capture and join any worker threads.
    virtual void Stop() = 0;

    // Returns base64-encoded WAV (16 kHz mono PCM16) of the last N seconds.
    // Empty string if no audio is buffered yet.
    virtual std::string SnapshotAsBase64Wav(int seconds) = 0;

    // Normalized RMS energy (0..1) of the most recent N seconds. Used for VAD
    // gating and the audio-level indicator.
    virtual float RecentEnergy(int seconds) = 0;
};

// Factory — returns the implementation for the current build target.
std::unique_ptr<IAudioCapture> CreateAudioCapture();

// Enumerate render or capture devices for the device picker UI.
// flowIsRender: true for output devices (speakers), false for input (mics).
// Implementation lives in the platform .cpp; this is just the declaration.
std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool flowIsRender);
