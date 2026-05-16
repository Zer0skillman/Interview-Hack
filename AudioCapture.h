#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

struct AudioDeviceInfo {
    std::string id;     // UTF-8 WASAPI device ID, e.g. "{0.0.0.00000000}.{guid}"
    std::string name;   // friendly name (e.g. "Speakers (Realtek)")
};

// Captures system audio via WASAPI loopback (what other people say in your
// Zoom/Meet/Teams call). Stores the last ~30s in a ring buffer; on demand,
// SnapshotAsBase64Wav() returns a base64-encoded 16kHz mono 16-bit WAV.
//
// Notes:
//   - Loopback only — your microphone is NOT captured. This is intentional
//     for the interview-assist use case (you don't want it answering questions
//     YOU asked).
//   - The capture thread runs continuously while Start() is active.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Begins WASAPI loopback capture. By default uses the system default render
    // device; pass a specific UTF-8 device ID to capture from a different output.
    // If `withMic` is true, also captures from the mic device and mixes it in.
    // Returns false on failure.
    bool Start(bool withMic = false,
               const std::string& loopbackDeviceId = "",
               const std::string& micDeviceId = "");

    // Enumerate render or capture devices for the device picker UI.
    // flowIsRender: true for output devices (speakers), false for input (mics).
    static std::vector<AudioDeviceInfo> EnumerateDevices(bool flowIsRender);

    // Stops capture and joins the worker thread.
    void Stop();

    // Returns base64-encoded WAV (16kHz mono PCM16) of the last `seconds` of
    // captured audio, or empty string if no audio is available.
    std::string SnapshotAsBase64Wav(int seconds = 30);

    // Normalized RMS energy (0..1) of the last `seconds` of captured audio.
    // Used for voice activity detection and the audio level indicator.
    float RecentEnergy(int seconds = 2);

private:
    void CaptureLoop();
    void MicCaptureLoop();
    bool m_withMic = false;
    std::string m_loopbackDeviceId;
    std::string m_micDeviceId;
    std::thread m_micThread;

    // Capture-side state — written only by the worker thread, read under m_mutex
    int  m_sourceRate    = 48000;
    int  m_sourceChannels = 2;
    bool m_sourceIsFloat = true;

    // Ring buffer of mono 16kHz float samples (the worker downsamples as it writes)
    std::vector<float> m_ring;       // fixed capacity
    size_t             m_writePos = 0;
    size_t             m_totalWritten = 0;
    std::mutex         m_mutex;

    std::thread        m_thread;
    std::atomic<bool>  m_running { false };

    static constexpr int kTargetRate   = 16000;
    static constexpr int kMaxSeconds   = 60;  // ring capacity
};
