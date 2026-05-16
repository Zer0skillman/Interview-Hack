#define WIN32_LEAN_AND_MEAN
#include "IScreenshot.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// PNG encoder CLSID lookup
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, format) == 0) {
            *pClsid = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

class WindowsScreenshot : public IScreenshot {
public:
    std::string CaptureMonitorUnderCursorAsBase64Png() override {
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (!GetMonitorInfo(hMon, &mi)) return std::string();

        int x = mi.rcMonitor.left;
        int y = mi.rcMonitor.top;
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

        HDC hdcScreen = GetDC(NULL);
        HDC hdcMem    = CreateCompatibleDC(hdcScreen);
        HBITMAP hBmp  = CreateCompatibleBitmap(hdcScreen, w, h);
        HBITMAP hOld  = (HBITMAP)SelectObject(hdcMem, hBmp);

        BOOL ok = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY);

        SelectObject(hdcMem, hOld);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);

        if (!ok) {
            DeleteObject(hBmp);
            return std::string();
        }

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) {
            DeleteObject(hBmp);
            return std::string();
        }

        std::string base64;
        {
            Gdiplus::Bitmap bmp(hBmp, NULL);
            CLSID pngClsid;
            if (GetEncoderClsid(L"image/png", &pngClsid) >= 0 &&
                bmp.Save(stream, &pngClsid, NULL) == Gdiplus::Ok)
            {
                HGLOBAL hg = NULL;
                if (GetHGlobalFromStream(stream, &hg) == S_OK && hg) {
                    SIZE_T sz = GlobalSize(hg);
                    void* p = GlobalLock(hg);
                    if (p && sz > 0) {
                        DWORD b64Len = 0;
                        if (CryptBinaryToStringA((const BYTE*)p, (DWORD)sz,
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                NULL, &b64Len) && b64Len > 0)
                        {
                            base64.resize(b64Len);
                            if (CryptBinaryToStringA((const BYTE*)p, (DWORD)sz,
                                    CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                    &base64[0], &b64Len))
                            {
                                if (!base64.empty() && base64.back() == '\0') base64.pop_back();
                            } else {
                                base64.clear();
                            }
                        }
                        GlobalUnlock(hg);
                    }
                }
            }
        }

        stream->Release();
        DeleteObject(hBmp);
        return base64;
    }
};

}  // namespace

std::unique_ptr<IScreenshot> CreateScreenshot() {
    return std::make_unique<WindowsScreenshot>();
}
