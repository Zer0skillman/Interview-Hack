#pragma once
#include "IAudioCapture.h"
#include <mutex>
#include <atomic>
#include <thread>

// Windows-side audio capture: WASAPI loopback on the default render endpoint,
// optional mic mixed in additively at 70% gain, 60s ring buffer downsampled
// to 16 kHz mono. Implements IAudioCapture.
class WindowsAudioCapture : public IAudioCapture {
public:
    WindowsAudioCapture();
    ~WindowsAudioCapture() override;

    bool  Start(bool withMic,
                const std::string& loopbackDeviceId,
                const std::string& micDeviceId) override;
    void  Stop() override;
    std::string SnapshotAsBase64Wav(int seconds) override;
    float RecentEnergy(int seconds) override;

private:
    void CaptureLoop();
    void MicCaptureLoop();

    int  m_sourceRate     = 48000;
    int  m_sourceChannels = 2;
    bool m_sourceIsFloat  = true;

    std::vector<float> m_ring;
    size_t             m_writePos = 0;
    size_t             m_totalWritten = 0;
    std::mutex         m_mutex;

    std::thread        m_thread;
    std::thread        m_micThread;
    std::atomic<bool>  m_running { false };

    bool        m_withMic = false;
    std::string m_loopbackDeviceId;
    std::string m_micDeviceId;

    static constexpr int kTargetRate = 16000;
    static constexpr int kMaxSeconds = 60;
};
