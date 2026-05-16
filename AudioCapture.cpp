#define WIN32_LEAN_AND_MEAN
#include "AudioCapture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "ole32.lib")

// KSDATAFORMAT_SUBTYPE_* GUIDs are normally provided by ksuser.lib. Define them
// locally to avoid a separate link dependency.
static const GUID kSubtypePCM   = { 0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// PKEY_Device_FriendlyName, same trick — provide locally to avoid linking propsys.
static const PROPERTYKEY kPKEY_Device_FriendlyName_Local = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

// Subset of WAVEFORMATEXTENSIBLE so we don't need <ksmedia.h>
struct AC_WAVEFORMATEXTENSIBLE {
    WAVEFORMATEX Format;
    union { WORD wValidBitsPerSample; WORD wSamplesPerBlock; WORD wReserved; } Samples;
    DWORD dwChannelMask;
    GUID  SubFormat;
};
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

namespace {

constexpr REFERENCE_TIME kRefPerSec = 10000000LL;
constexpr REFERENCE_TIME kBufferDuration = 2 * kRefPerSec;  // 2 seconds buffer

// Resolve the actual sample format from a WAVEFORMATEX, handling EXTENSIBLE.
// Returns true on success and fills outIsFloat / outBits / outRate / outChannels.
bool DescribeFormat(const WAVEFORMATEX* fmt, bool& outIsFloat, int& outBits, int& outRate, int& outChannels) {
    if (!fmt) return false;
    outBits     = fmt->wBitsPerSample;
    outRate     = (int)fmt->nSamplesPerSec;
    outChannels = fmt->nChannels;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        outIsFloat = true;
        return outBits == 32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        outIsFloat = false;
        return outBits == 16 || outBits == 32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const AC_WAVEFORMATEXTENSIBLE* ex = reinterpret_cast<const AC_WAVEFORMATEXTENSIBLE*>(fmt);
        if (IsEqualGUID(ex->SubFormat, kSubtypeFloat)) {
            outIsFloat = true;
            return outBits == 32;
        }
        if (IsEqualGUID(ex->SubFormat, kSubtypePCM)) {
            outIsFloat = false;
            return outBits == 16 || outBits == 32;
        }
    }
    return false;
}

// Append a little-endian integer to a byte buffer.
void Put16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}
void Put32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    DWORD b64Len = 0;
    if (!CryptBinaryToStringA(data, (DWORD)len,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64Len) || b64Len == 0) {
        return std::string();
    }
    std::string out(b64Len, '\0');
    if (!CryptBinaryToStringA(data, (DWORD)len,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &b64Len)) {
        return std::string();
    }
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// AudioCapture
// -----------------------------------------------------------------------------

AudioCapture::AudioCapture() {
    m_ring.assign((size_t)kTargetRate * kMaxSeconds, 0.0f);
}

AudioCapture::~AudioCapture() {
    Stop();
}

bool AudioCapture::Start(bool withMic,
                         const std::string& loopbackDeviceId,
                         const std::string& micDeviceId) {
    if (m_running.load()) return true;
    m_withMic = withMic;
    m_loopbackDeviceId = loopbackDeviceId;
    m_micDeviceId = micDeviceId;
    m_running.store(true);
    m_thread = std::thread(&AudioCapture::CaptureLoop, this);
    if (m_withMic) {
        m_micThread = std::thread(&AudioCapture::MicCaptureLoop, this);
    }
    return true;
}

// Convert UTF-8 ID string to wchar_t* for WASAPI calls
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], sz);
    return w;
}

static std::string WToUtf8(LPCWSTR w) {
    if (!w || !*w) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (sz <= 1) return std::string();
    std::string s(sz - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], sz, NULL, NULL);
    return s;
}

std::vector<AudioDeviceInfo> AudioCapture::EnumerateDevices(bool flowIsRender) {
    std::vector<AudioDeviceInfo> out;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) break;
        EDataFlow flow = flowIsRender ? eRender : eCapture;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) break;

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev))) continue;

            AudioDeviceInfo info;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) {
                info.id = WToUtf8(id);
                CoTaskMemFree(id);
            }

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName_Local, &pv)) && pv.vt == VT_LPWSTR) {
                    info.name = WToUtf8(pv.pwszVal);
                }
                PropVariantClear(&pv);
                props->Release();
            }

            if (!info.id.empty()) out.push_back(info);
            dev->Release();
        }
    } while (false);

    if (collection) collection->Release();
    if (enumerator) enumerator->Release();
    if (comInited && hr != RPC_E_CHANGED_MODE) CoUninitialize();
    return out;
}

void AudioCapture::Stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_micThread.joinable()) m_micThread.join();
}

void AudioCapture::CaptureLoop() {
    // COM is initialized per-thread for the audio worker.
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    WAVEFORMATEX*        mixFormat  = nullptr;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) break;

        // Use specified device if configured, else default
        if (!m_loopbackDeviceId.empty()) {
            std::wstring widId = Utf8ToW(m_loopbackDeviceId);
            if (FAILED(enumerator->GetDevice(widId.c_str(), &device))) {
                // Fall back to default if the saved device isn't available anymore
                if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) break;
            }
        } else {
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) break;
        }
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client))) break;
        if (FAILED(client->GetMixFormat(&mixFormat))) break;

        bool isFloat;
        int  bits, rate, chans;
        if (!DescribeFormat(mixFormat, isFloat, bits, rate, chans)) break;

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_sourceRate     = rate;
            m_sourceChannels = chans;
            m_sourceIsFloat  = isFloat;
            // Reset ring on (re)start
            m_writePos = 0;
            m_totalWritten = 0;
            std::fill(m_ring.begin(), m_ring.end(), 0.0f);
        }

        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration, 0, mixFormat, NULL))) break;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture))) break;
        if (FAILED(client->Start())) break;

        const float rateRatio = (float)rate / (float)kTargetRate;

        while (m_running.load()) {
            UINT32 packetSize = 0;
            if (FAILED(capture->GetNextPacketSize(&packetSize))) break;
            if (packetSize == 0) {
                Sleep(10);
                continue;
            }

            BYTE*  pData      = nullptr;
            UINT32 numFrames  = 0;
            DWORD  flags      = 0;
            if (FAILED(capture->GetBuffer(&pData, &numFrames, &flags, NULL, NULL))) break;

            // If the meeting is silent, WASAPI signals "silent" — write zeros.
            std::vector<float> mono;
            mono.reserve((size_t)((float)numFrames / rateRatio) + 8);

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || pData == nullptr) {
                // Decimated zeros
                size_t outCount = (size_t)((float)numFrames / rateRatio);
                mono.assign(outCount, 0.0f);
            } else {
                // Convert -> mono float -> nearest-neighbor downsample to 16kHz
                // (good enough for ASR; voice energy is well under 8kHz)
                float invRatio = (float)kTargetRate / (float)rate;
                double outFrac = 0.0;
                for (UINT32 i = 0; i < numFrames; ++i) {
                    float monoSample = 0.0f;
                    if (isFloat) {
                        const float* src = reinterpret_cast<const float*>(pData) + (size_t)i * chans;
                        for (int c = 0; c < chans; ++c) monoSample += src[c];
                        monoSample /= (float)chans;
                    } else if (bits == 16) {
                        const int16_t* src = reinterpret_cast<const int16_t*>(pData) + (size_t)i * chans;
                        int32_t sum = 0;
                        for (int c = 0; c < chans; ++c) sum += src[c];
                        monoSample = (float)sum / ((float)chans * 32768.0f);
                    } else {  // 32-bit PCM
                        const int32_t* src = reinterpret_cast<const int32_t*>(pData) + (size_t)i * chans;
                        int64_t sum = 0;
                        for (int c = 0; c < chans; ++c) sum += src[c];
                        monoSample = (float)((double)sum / ((double)chans * 2147483648.0));
                    }

                    outFrac += invRatio;
                    while (outFrac >= 1.0) {
                        mono.push_back(monoSample);
                        outFrac -= 1.0;
                    }
                }
            }

            capture->ReleaseBuffer(numFrames);

            // Write mono samples into the ring
            if (!mono.empty()) {
                std::lock_guard<std::mutex> lk(m_mutex);
                const size_t cap = m_ring.size();
                for (float s : mono) {
                    m_ring[m_writePos] = s;
                    m_writePos = (m_writePos + 1) % cap;
                    m_totalWritten++;
                }
            }
        }

        client->Stop();
    } while (false);

    if (capture)    capture->Release();
    if (client)     client->Release();
    if (device)     device->Release();
    if (enumerator) enumerator->Release();
    if (mixFormat)  CoTaskMemFree(mixFormat);
    if (comInited)  CoUninitialize();
}

// Mic capture: separate WASAPI stream from the default capture endpoint, samples
// converted to mono 16 kHz and ADDITIVELY mixed into the ring at the current
// m_writePos. Imperfect timing alignment but adequate for ASR.
void AudioCapture::MicCaptureLoop() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    WAVEFORMATEX*        mixFormat  = nullptr;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) break;

        if (!m_micDeviceId.empty()) {
            std::wstring widId = Utf8ToW(m_micDeviceId);
            if (FAILED(enumerator->GetDevice(widId.c_str(), &device))) {
                if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) break;
            }
        } else {
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) break;
        }
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client))) break;
        if (FAILED(client->GetMixFormat(&mixFormat))) break;

        bool isFloat; int bits, rate, chans;
        if (!DescribeFormat(mixFormat, isFloat, bits, rate, chans)) break;

        // Mic capture is NOT loopback — pass 0 for flags
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mixFormat, NULL))) break;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture))) break;
        if (FAILED(client->Start())) break;

        while (m_running.load()) {
            UINT32 packetSize = 0;
            if (FAILED(capture->GetNextPacketSize(&packetSize))) break;
            if (packetSize == 0) { Sleep(10); continue; }

            BYTE* pData = nullptr; UINT32 numFrames = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&pData, &numFrames, &flags, NULL, NULL))) break;

            std::vector<float> mono;
            float invRatio = (float)kTargetRate / (float)rate;
            mono.reserve((size_t)((float)numFrames * invRatio) + 8);

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && pData != nullptr) {
                double outFrac = 0.0;
                for (UINT32 i = 0; i < numFrames; ++i) {
                    float monoSample = 0.0f;
                    if (isFloat) {
                        const float* src = reinterpret_cast<const float*>(pData) + (size_t)i * chans;
                        for (int c = 0; c < chans; ++c) monoSample += src[c];
                        monoSample /= (float)chans;
                    } else if (bits == 16) {
                        const int16_t* src = reinterpret_cast<const int16_t*>(pData) + (size_t)i * chans;
                        int32_t sum = 0;
                        for (int c = 0; c < chans; ++c) sum += src[c];
                        monoSample = (float)sum / ((float)chans * 32768.0f);
                    } else {
                        const int32_t* src = reinterpret_cast<const int32_t*>(pData) + (size_t)i * chans;
                        int64_t sum = 0;
                        for (int c = 0; c < chans; ++c) sum += src[c];
                        monoSample = (float)((double)sum / ((double)chans * 2147483648.0));
                    }
                    outFrac += invRatio;
                    while (outFrac >= 1.0) { mono.push_back(monoSample); outFrac -= 1.0; }
                }
            }

            capture->ReleaseBuffer(numFrames);

            if (!mono.empty()) {
                std::lock_guard<std::mutex> lk(m_mutex);
                const size_t cap = m_ring.size();
                // Start mixing from a position roughly matching the loopback's
                // recent writes — best-effort alignment
                size_t pos = (m_writePos + cap - mono.size()) % cap;
                for (float s : mono) {
                    float v = m_ring[pos] + s * 0.7f;  // attenuate mic to avoid clipping
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    m_ring[pos] = v;
                    pos = (pos + 1) % cap;
                }
            }
        }

        client->Stop();
    } while (false);

    if (capture)    capture->Release();
    if (client)     client->Release();
    if (device)     device->Release();
    if (enumerator) enumerator->Release();
    if (mixFormat)  CoTaskMemFree(mixFormat);
    if (comInited)  CoUninitialize();
}

float AudioCapture::RecentEnergy(int seconds) {
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

std::string AudioCapture::SnapshotAsBase64Wav(int seconds) {
    if (seconds <= 0) return std::string();
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;

    std::vector<float> mono;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const size_t cap     = m_ring.size();
        const size_t want    = (size_t)seconds * kTargetRate;
        const size_t have    = std::min<size_t>(m_totalWritten, std::min(want, cap));
        if (have == 0) return std::string();

        mono.resize(have);
        // Read 'have' samples ending at m_writePos
        size_t start = (m_writePos + cap - have) % cap;
        for (size_t i = 0; i < have; ++i) {
            mono[i] = m_ring[(start + i) % cap];
        }
    }

    // Convert float -> int16 PCM
    std::vector<int16_t> pcm(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) {
        float v = mono[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)(v * 32767.0f);
    }

    // Build RIFF/WAVE header (44 bytes) + PCM bytes
    const uint32_t sampleRate    = (uint32_t)kTargetRate;
    const uint16_t numChannels   = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate      = sampleRate * numChannels * bitsPerSample / 8;
    const uint16_t blockAlign    = numChannels * bitsPerSample / 8;
    const uint32_t dataBytes     = (uint32_t)(pcm.size() * sizeof(int16_t));
    const uint32_t riffSize      = 36 + dataBytes;

    std::vector<uint8_t> wav;
    wav.reserve(44 + dataBytes);

    auto putStr = [&](const char* s, size_t n) { for (size_t i = 0; i < n; ++i) wav.push_back((uint8_t)s[i]); };

    putStr("RIFF", 4);
    Put32LE(wav, riffSize);
    putStr("WAVE", 4);
    putStr("fmt ", 4);
    Put32LE(wav, 16);                 // fmt chunk size
    Put16LE(wav, 1);                  // PCM
    Put16LE(wav, numChannels);
    Put32LE(wav, sampleRate);
    Put32LE(wav, byteRate);
    Put16LE(wav, blockAlign);
    Put16LE(wav, bitsPerSample);
    putStr("data", 4);
    Put32LE(wav, dataBytes);

    const uint8_t* pcmBytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pcmBytes, pcmBytes + dataBytes);

    return Base64Encode(wav.data(), wav.size());
}
