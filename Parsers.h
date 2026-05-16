#pragma once
// Inline copies of the small parser functions used in the app. They live here
// so the test runner can call them without linking the whole Win32 codebase.
//
// Duplication note: OverlayWindow.cpp and LLMClient.cpp currently have their
// own private copies of these. When the OverlayWindow refactor lands we'll
// switch those over to use Parsers.h directly.
#include <string>
#include <vector>
#include <cwctype>

namespace parsers {

// Find the first JSON string-field by name, return its value. Honors \\, \n,
// \r, \t, \", \/ and \uXXXX escapes. Returns empty wstring if not found.
inline std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field) {
    std::wstring needle = L"\"" + field + L"\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        size_t i = pos + needle.length();
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L':') { pos++; continue; }
        i++;
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L'"') { pos++; continue; }
        i++;
        std::wstring result;
        bool escape = false;
        for (; i < json.length(); i++) {
            wchar_t c = json[i];
            if (escape) {
                if (c == L'n') result += L'\n';
                else if (c == L'r') result += L'\r';
                else if (c == L't') result += L'\t';
                else if (c == L'"') result += L'"';
                else if (c == L'\\') result += L'\\';
                else if (c == L'/') result += L'/';
                else if (c == L'u' && i + 4 < json.length()) {
                    try {
                        result += static_cast<wchar_t>(std::stoi(json.substr(i + 1, 4), nullptr, 16));
                        i += 4;
                    } catch (...) { result += L"\\u"; }
                }
                else result += c;
                escape = false;
            } else {
                if (c == L'\\') escape = true;
                else if (c == L'"') return result;
                else result += c;
            }
        }
        return result;
    }
    return L"";
}

// Strip inline markdown markers (**, *, _, `) from prose for display.
inline std::wstring StripInlineMd(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'*' && i + 1 < s.size() && s[i+1] == L'*') { i += 2; continue; }
        if (c == L'*' || c == L'_') {
            bool prevAlnum = (i > 0) && (iswalnum(s[i-1]) || s[i-1] == L'_');
            bool nextAlnum = (i + 1 < s.size()) && (iswalnum(s[i+1]) || s[i+1] == L'_');
            if (!(prevAlnum && nextAlnum)) { i++; continue; }
        }
        if (c == L'`') { i++; continue; }
        out += c;
        i++;
    }
    return out;
}

struct Segment {
    std::wstring text;
    bool isCode;
    std::wstring language;
};

// Split text into alternating prose / code segments using ``` fences. The
// optional language label after the opening fence is captured into language.
inline std::vector<Segment> ParseSegments(const std::wstring& raw) {
    std::vector<Segment> out;
    std::wstring buf;
    std::wstring pendingLang;
    bool inCode = false;

    auto trimTrailingNL = [](std::wstring& s) {
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    };
    auto flush = [&](bool isCodeSeg) {
        if (isCodeSeg) trimTrailingNL(buf);
        if (!buf.empty()) {
            Segment s;
            s.text = buf;
            s.isCode = isCodeSeg;
            if (isCodeSeg) { s.language = pendingLang; pendingLang.clear(); }
            out.push_back(s);
            buf.clear();
        }
    };

    size_t i = 0;
    while (i < raw.size()) {
        if (i + 2 < raw.size() && raw[i] == L'`' && raw[i+1] == L'`' && raw[i+2] == L'`') {
            flush(inCode);
            i += 3;
            if (!inCode) {
                pendingLang.clear();
                while (i < raw.size() && raw[i] != L'\n' && raw[i] != L'\r') {
                    pendingLang += raw[i]; i++;
                }
                if (i < raw.size() && raw[i] == L'\r') i++;
                if (i < raw.size() && raw[i] == L'\n') i++;
            }
            inCode = !inCode;
        } else {
            buf += raw[i++];
        }
    }
    flush(inCode);
    return out;
}

}  // namespace parsers
