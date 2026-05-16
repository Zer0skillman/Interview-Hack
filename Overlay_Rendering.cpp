// Rendering-only translation unit for OverlayWindow.
// Contents factored out of OverlayWindow.cpp to keep that file manageable.
// All symbols here are file-static helpers EXCEPT OverlayWindow::OnPaint,
// which remains a normal member function.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "OverlayWindow.h"
#include "ConfigLoader.h"

// ---------------------------------------------------------------------------
// PaintSeg + segment parsing
// ---------------------------------------------------------------------------

struct PaintSeg {
    std::wstring text;
    bool isCode;
    std::wstring language;  // code-fence language label (e.g. "python"); empty if none
};

// ---------------------------------------------------------------------------
// Color constants
// ---------------------------------------------------------------------------
static const COLORREF kDefaultCodeColor   = RGB(220, 220, 220);
static const COLORREF kUnmatchedColor     = RGB(255,  80,  80);
static const COLORREF kBracketPalette[] = {
    RGB(255, 215,  64),  // yellow
    RGB(255, 120, 200),  // pink
    RGB(120, 200, 255),  // cyan
    RGB(160, 255, 160),  // light green
};
static const COLORREF kKeywordColor = RGB(200, 130, 255);  // purple
static const COLORREF kStringColor  = RGB(255, 170,  80);  // orange
static const COLORREF kNumberColor  = RGB(120, 200, 255);  // cyan
static const COLORREF kCommentColor = RGB(120, 140, 120);  // dim gray-green

// ---------------------------------------------------------------------------
// Keyword tables
// ---------------------------------------------------------------------------

static bool InSet(const std::wstring& w, const wchar_t* const* set, size_t count) {
    for (size_t i = 0; i < count; ++i) if (w == set[i]) return true;
    return false;
}

static bool IsKeyword(const std::wstring& w) {
    static const wchar_t* kw[] = {
        L"if", L"else", L"elif", L"for", L"while", L"do", L"switch", L"case",
        L"break", L"continue", L"return", L"default", L"goto", L"in", L"of", L"is", L"not", L"and", L"or",
        L"def", L"class", L"function", L"fn", L"func", L"var", L"let", L"const",
        L"int", L"void", L"char", L"bool", L"float", L"double", L"long", L"short", L"unsigned", L"signed",
        L"public", L"private", L"protected", L"static", L"virtual", L"override", L"final", L"abstract",
        L"new", L"delete", L"this", L"self", L"super", L"import", L"from", L"package", L"namespace", L"using",
        L"true", L"false", L"null", L"nil", L"None", L"True", L"False", L"undefined",
        L"try", L"catch", L"finally", L"throw", L"throws", L"raise", L"except",
        L"async", L"await", L"yield", L"struct", L"enum", L"interface", L"type", L"impl",
        L"auto", L"std", L"size_t",
    };
    return InSet(w, kw, sizeof(kw) / sizeof(kw[0]));
}

static bool IsKeywordInLang(const std::wstring& w, const std::wstring& lang) {
    if (lang.empty()) return IsKeyword(w);
    std::wstring lk;
    for (wchar_t c : lang) lk += (wchar_t)towlower(c);

    static const wchar_t* py[] = {
        L"if", L"elif", L"else", L"for", L"while", L"break", L"continue",
        L"def", L"class", L"return", L"yield", L"import", L"from", L"as",
        L"try", L"except", L"finally", L"raise", L"with", L"lambda",
        L"True", L"False", L"None", L"and", L"or", L"not", L"in", L"is",
        L"pass", L"global", L"nonlocal", L"async", L"await",
    };
    static const wchar_t* js[] = {
        L"var", L"let", L"const", L"function", L"return", L"if", L"else", L"for", L"while",
        L"do", L"switch", L"case", L"default", L"break", L"continue", L"class", L"extends",
        L"new", L"this", L"typeof", L"instanceof", L"in", L"of", L"async", L"await",
        L"try", L"catch", L"finally", L"throw", L"true", L"false", L"null", L"undefined",
        L"import", L"export", L"from", L"as", L"interface", L"type", L"enum", L"namespace",
    };
    static const wchar_t* c_family[] = {
        L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
        L"continue", L"return", L"goto", L"class", L"struct", L"enum", L"union", L"public",
        L"private", L"protected", L"static", L"virtual", L"override", L"final", L"abstract",
        L"new", L"delete", L"this", L"super", L"void", L"int", L"char", L"float", L"double",
        L"bool", L"long", L"short", L"unsigned", L"signed", L"const", L"volatile", L"auto",
        L"true", L"false", L"null", L"nullptr", L"namespace", L"using", L"typedef",
        L"try", L"catch", L"throw", L"throws", L"template", L"typename", L"sizeof",
    };
    static const wchar_t* rust[] = {
        L"fn", L"let", L"mut", L"if", L"else", L"for", L"while", L"loop", L"match",
        L"return", L"break", L"continue", L"struct", L"enum", L"impl", L"trait",
        L"pub", L"use", L"mod", L"crate", L"self", L"Self", L"as", L"in", L"where",
        L"const", L"static", L"true", L"false", L"None", L"Some", L"Ok", L"Err",
        L"async", L"await", L"move", L"ref", L"dyn", L"box",
    };
    static const wchar_t* go[] = {
        L"func", L"var", L"const", L"type", L"struct", L"interface", L"if", L"else",
        L"for", L"range", L"switch", L"case", L"default", L"break", L"continue",
        L"return", L"go", L"select", L"chan", L"defer", L"map", L"package", L"import",
        L"nil", L"true", L"false", L"iota",
    };

    if (lk == L"py"   || lk == L"python")                                 return InSet(w, py,       sizeof(py)/sizeof(*py));
    if (lk == L"js"   || lk == L"javascript" || lk == L"jsx"
        || lk == L"ts" || lk == L"typescript" || lk == L"tsx")            return InSet(w, js,       sizeof(js)/sizeof(*js));
    if (lk == L"c"    || lk == L"cpp" || lk == L"c++" || lk == L"cxx"
        || lk == L"java" || lk == L"cs"  || lk == L"csharp" || lk == L"c#") return InSet(w, c_family, sizeof(c_family)/sizeof(*c_family));
    if (lk == L"rust" || lk == L"rs")                                     return InSet(w, rust,     sizeof(rust)/sizeof(*rust));
    if (lk == L"go"   || lk == L"golang")                                 return InSet(w, go,       sizeof(go)/sizeof(*go));

    return IsKeyword(w);
}

// ---------------------------------------------------------------------------
// Colorizer (brackets + strings + numbers + comments + keywords)
// ---------------------------------------------------------------------------
static std::vector<COLORREF> ColorizeBrackets(const std::wstring& code, const std::wstring& language = std::wstring()) {
    std::vector<COLORREF> out(code.size(), kDefaultCodeColor);
    std::vector<int> stack;
    int depth = 0;
    constexpr int N = (int)(sizeof(kBracketPalette) / sizeof(kBracketPalette[0]));

    enum State { Default, InStrDQ, InStrSQ, InLineComment, InBlockComment };
    State st = Default;

    size_t i = 0;
    while (i < code.size()) {
        wchar_t c = code[i];
        if (st == InLineComment) {
            out[i] = kCommentColor;
            if (c == L'\n') st = Default;
            i++; continue;
        }
        if (st == InBlockComment) {
            out[i] = kCommentColor;
            if (c == L'*' && i + 1 < code.size() && code[i+1] == L'/') {
                out[i+1] = kCommentColor;
                i += 2;
                st = Default;
                continue;
            }
            i++; continue;
        }
        if (st == InStrDQ || st == InStrSQ) {
            out[i] = kStringColor;
            if (c == L'\\' && i + 1 < code.size()) { out[i+1] = kStringColor; i += 2; continue; }
            if ((st == InStrDQ && c == L'"') || (st == InStrSQ && c == L'\'')) st = Default;
            i++; continue;
        }

        if (c == L'"') { st = InStrDQ; out[i] = kStringColor; i++; continue; }
        if (c == L'\'') { st = InStrSQ; out[i] = kStringColor; i++; continue; }
        if (c == L'/' && i + 1 < code.size() && code[i+1] == L'/') {
            st = InLineComment; out[i] = kCommentColor; i++; continue;
        }
        if (c == L'/' && i + 1 < code.size() && code[i+1] == L'*') {
            st = InBlockComment; out[i] = kCommentColor; out[i+1] = kCommentColor; i += 2; continue;
        }
        if (c == L'#') { st = InLineComment; out[i] = kCommentColor; i++; continue; }

        if (c == L'(' || c == L'[' || c == L'{') {
            out[i] = kBracketPalette[depth % N];
            stack.push_back(depth);
            depth++;
            i++; continue;
        }
        if (c == L')' || c == L']' || c == L'}') {
            if (!stack.empty()) {
                depth = stack.back();
                stack.pop_back();
                out[i] = kBracketPalette[depth % N];
            } else {
                out[i] = kUnmatchedColor;
            }
            i++; continue;
        }

        if (iswdigit(c)) {
            while (i < code.size() && (iswdigit(code[i]) || code[i] == L'.' || code[i] == L'x'
                   || (code[i] >= L'a' && code[i] <= L'f')
                   || (code[i] >= L'A' && code[i] <= L'F'))) {
                out[i] = kNumberColor;
                i++;
            }
            continue;
        }

        if (iswalpha(c) || c == L'_') {
            size_t start = i;
            while (i < code.size() && (iswalnum(code[i]) || code[i] == L'_')) i++;
            std::wstring word = code.substr(start, i - start);
            if (IsKeywordInLang(word, language)) {
                for (size_t k = start; k < i; ++k) out[k] = kKeywordColor;
            }
            continue;
        }

        i++;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Markdown + segment parsing helpers
// ---------------------------------------------------------------------------

static bool HasInlineMd(const std::wstring& s) {
    return s.find(L"**") != std::wstring::npos
        || s.find(L"*")  != std::wstring::npos
        || s.find(L"_")  != std::wstring::npos
        || s.find(L"`")  != std::wstring::npos;
}

struct MdRun { std::wstring text; bool bold; bool italic; };
static std::vector<MdRun> ParseMdRuns(const std::wstring& s) {
    std::vector<MdRun> out;
    bool bold = false, italic = false;
    std::wstring buf;
    auto flush = [&]() { if (!buf.empty()) { out.push_back({ buf, bold, italic }); buf.clear(); } };
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'*' && i + 1 < s.size() && s[i+1] == L'*') {
            flush(); bold = !bold; i += 2; continue;
        }
        if (c == L'*' || c == L'_') {
            bool prevAlnum = (i > 0) && (iswalnum(s[i-1]) || s[i-1] == L'_');
            bool nextAlnum = (i + 1 < s.size()) && (iswalnum(s[i+1]) || s[i+1] == L'_');
            if (!(prevAlnum && nextAlnum)) { flush(); italic = !italic; i++; continue; }
        }
        if (c == L'`') { i++; continue; }
        buf += c;
        i++;
    }
    flush();
    return out;
}

struct MdLayoutResult { int width; int height; };
static MdLayoutResult LayoutMd(
    HDC hdc, const std::vector<MdRun>& runs,
    int xStart, int yStart, int maxWidth, int lineH,
    HFONT fonts[4], COLORREF textColor, bool draw)
{
    int x = xStart;
    int y = yStart;
    int maxX = xStart;
    for (const auto& run : runs) {
        int idx = (run.bold ? 1 : 0) | (run.italic ? 2 : 0);
        SelectObject(hdc, fonts[idx]);
        if (draw) SetTextColor(hdc, textColor);

        size_t i = 0;
        while (i < run.text.size()) {
            size_t ws = i;
            while (i < run.text.size() && iswspace(run.text[i])) {
                if (run.text[i] == L'\n') {
                    if (i > ws) {
                        std::wstring sp = run.text.substr(ws, i - ws);
                        SIZE spSz; GetTextExtentPoint32W(hdc, sp.c_str(), (int)sp.size(), &spSz);
                        if (draw && x > xStart) TextOutW(hdc, x, y, sp.c_str(), (int)sp.size());
                        x += spSz.cx;
                        if (x > maxX) maxX = x;
                    }
                    x = xStart;
                    y += lineH;
                    ws = i + 1;
                }
                i++;
            }
            if (i > ws) {
                std::wstring sp = run.text.substr(ws, i - ws);
                if (!sp.empty() && x > xStart) {
                    SIZE spSz; GetTextExtentPoint32W(hdc, sp.c_str(), (int)sp.size(), &spSz);
                    if (draw) TextOutW(hdc, x, y, sp.c_str(), (int)sp.size());
                    x += spSz.cx;
                }
            }

            size_t wstart = i;
            while (i < run.text.size() && !iswspace(run.text[i])) i++;
            if (i > wstart) {
                std::wstring word = run.text.substr(wstart, i - wstart);
                SIZE sz; GetTextExtentPoint32W(hdc, word.c_str(), (int)word.size(), &sz);
                if (x + sz.cx > xStart + maxWidth && x > xStart) {
                    x = xStart;
                    y += lineH;
                }
                if (draw) TextOutW(hdc, x, y, word.c_str(), (int)word.size());
                x += sz.cx;
                if (x > maxX) maxX = x;
            }
        }
    }
    return { maxX - xStart, (y - yStart) + lineH };
}

static std::vector<PaintSeg> ParseSegments(const std::wstring& raw) {
    std::vector<PaintSeg> out;
    std::wstring buf;
    std::wstring pendingLang;
    bool inCode = false;

    auto trimTrailingNL = [](std::wstring& s) {
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    };
    auto flush = [&](bool isCode) {
        if (isCode) trimTrailingNL(buf);
        if (!buf.empty()) {
            PaintSeg s;
            s.text = buf;
            s.isCode = isCode;
            if (isCode) { s.language = pendingLang; pendingLang.clear(); }
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
                    pendingLang += raw[i];
                    i++;
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

static std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'\n') { out.push_back(cur); cur.clear(); }
        else if (c != L'\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// OnPaint — the heavy lifter
// ---------------------------------------------------------------------------

void OverlayWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    auto theme = ConfigLoader::GetTheme(m_config.theme);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme.prose_text);

    HFONT hProseFont = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseBold = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseItalic = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseBoldIt = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_BOLD, TRUE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hCodeFont = CreateFont(
        m_config.font_size_code, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hProseFont);

    HFONT proseFonts[4] = { hProseFont, hProseBold, hProseItalic, hProseBoldIt };

    HBRUSH hUserBrush = CreateSolidBrush(theme.user_bubble);
    HBRUSH hBotBrush  = CreateSolidBrush(theme.bot_bubble);
    HBRUSH hCodeBrush = CreateSolidBrush(theme.code_bg);
    HPEN   hNullPen   = CreatePen(PS_NULL, 0, 0);
    HPEN   hOldPen    = (HPEN)SelectObject(hdc, hNullPen);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width      = clientRect.right - clientRect.left;
    int fullHeight = clientRect.bottom - clientRect.top;
    const int barHeight = 50;
    int height     = fullHeight - barHeight;

    int bubblePadding = 10;
    int sideMargin    = 10;
    int innerPadX     = 10;
    int innerPadY     = 5;
    int segGap        = 4;
    int codeInsetX    = 4;
    int proseMaxInner = (int)(width * 0.85) - 2 * innerPadX;
    int bubbleMaxW    = width - 2 * sideMargin;

    SelectObject(hdc, hCodeFont);
    SIZE codeCh;
    GetTextExtentPoint32W(hdc, L"M", 1, &codeCh);
    int codeCharW = codeCh.cx;
    TEXTMETRIC codeTm;
    GetTextMetrics(hdc, &codeTm);
    int codeLineH = codeTm.tmHeight + 2;

    SelectObject(hdc, hProseFont);
    TEXTMETRIC proseTm;
    GetTextMetrics(hdc, &proseTm);
    int proseLineH = proseTm.tmHeight + 2;

    // -------- Measure pass --------
    struct MeasuredMsg {
        std::vector<PaintSeg>            segs;
        std::vector<int>                 segHeights;
        std::vector<std::vector<std::wstring>>   codeLines;
        std::vector<std::vector<COLORREF>>       codeColors;
        int innerW;
        int bubbleW;
        int bubbleH;
    };
    std::vector<MeasuredMsg> measured;
    measured.reserve(m_messages.size());

    int totalY = 10;
    for (const auto& msg : m_messages) {
        MeasuredMsg mm;
        mm.segs = ParseSegments(msg.text);
        if (mm.segs.empty()) mm.segs.push_back({ L" ", false });

        mm.innerW = 0;
        int innerH = 0;
        for (size_t si = 0; si < mm.segs.size(); ++si) {
            const auto& seg = mm.segs[si];
            if (seg.isCode) {
                auto lines  = SplitLines(seg.text);
                auto colors = ColorizeBrackets(seg.text, seg.language);
                int maxLineChars = 0;
                for (const auto& ln : lines) {
                    if ((int)ln.size() > maxLineChars) maxLineChars = (int)ln.size();
                }
                int sw = std::min(bubbleMaxW - 2 * (innerPadX + codeInsetX),
                                  maxLineChars * codeCharW + 4);
                int sh = (int)lines.size() * codeLineH;
                mm.segHeights.push_back(sh);
                if (sw > mm.innerW) mm.innerW = sw;
                innerH += sh;
                mm.codeLines.push_back(std::move(lines));
                mm.codeColors.push_back(std::move(colors));
            } else {
                int sw, sh;
                if (HasInlineMd(seg.text)) {
                    auto runs = ParseMdRuns(seg.text);
                    auto res = LayoutMd(hdc, runs, 0, 0, proseMaxInner, proseLineH,
                                        proseFonts, RGB(255,255,255), false);
                    sw = res.width;
                    sh = res.height;
                } else {
                    SelectObject(hdc, hProseFont);
                    RECT r = { 0, 0, proseMaxInner, 0 };
                    DrawText(hdc, seg.text.c_str(), -1, &r,
                             DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    sw = r.right - r.left;
                    sh = r.bottom - r.top;
                }
                mm.segHeights.push_back(sh);
                if (sw > mm.innerW) mm.innerW = sw;
                innerH += sh;
                mm.codeLines.push_back({});
                mm.codeColors.push_back({});
            }
            if (si + 1 < mm.segs.size()) innerH += segGap;
        }

        mm.bubbleW = std::min(mm.innerW + 2 * innerPadX, bubbleMaxW);
        mm.bubbleH = innerH + 2 * innerPadY;
        totalY += mm.bubbleH + bubblePadding;
        measured.push_back(std::move(mm));
    }

    m_contentHeight = totalY;

    int maxScroll = std::max(0, m_contentHeight - height + 20);
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if (m_scrollOffset < 0) m_scrollOffset = 0;

    // -------- Draw pass --------
    m_bubbleBounds.assign(m_messages.size(), RECT{0,0,0,0});

    int drawY = 10 - m_scrollOffset;
    for (size_t i = 0; i < m_messages.size(); ++i) {
        const auto& msg = m_messages[i];
        const auto& mm  = measured[i];

        if (drawY + mm.bubbleH > 0 && drawY < height) {
            int bubbleX = msg.isUser ? (width - mm.bubbleW - sideMargin) : sideMargin;
            m_bubbleBounds[i] = { bubbleX, drawY,
                                  bubbleX + mm.bubbleW, drawY + mm.bubbleH };

            HBRUSH bg = msg.isUser ? hUserBrush : hBotBrush;
            HBRUSH oldBg = (HBRUSH)SelectObject(hdc, bg);
            RoundRect(hdc, bubbleX, drawY,
                      bubbleX + mm.bubbleW, drawY + mm.bubbleH, 10, 10);
            SelectObject(hdc, oldBg);

            int segY = drawY + innerPadY;
            int segLeft  = bubbleX + innerPadX;
            int segRight = bubbleX + mm.bubbleW - innerPadX;

            for (size_t si = 0; si < mm.segs.size(); ++si) {
                const auto& seg = mm.segs[si];
                int sh = mm.segHeights[si];

                if (seg.isCode) {
                    HBRUSH oldB = (HBRUSH)SelectObject(hdc, hCodeBrush);
                    RoundRect(hdc,
                        segLeft - codeInsetX, segY - 2,
                        segRight + codeInsetX, segY + sh + 2,
                        4, 4);
                    SelectObject(hdc, oldB);

                    if (!seg.language.empty()) {
                        HFONT hLangFont = CreateFont(
                            10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                        SelectObject(hdc, hLangFont);
                        SetTextColor(hdc, RGB(120, 200, 255));
                        RECT lr = { segLeft, segY - 1, segRight + codeInsetX - 4, segY + 12 };
                        DrawText(hdc, seg.language.c_str(), -1, &lr,
                                 DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
                        DeleteObject(hLangFont);
                    }

                    SelectObject(hdc, hCodeFont);
                    HRGN clip = CreateRectRgn(segLeft, segY, segRight + codeInsetX, segY + sh + 2);
                    SelectClipRgn(hdc, clip);

                    const auto& lines  = mm.codeLines[si];
                    const auto& colors = mm.codeColors[si];

                    size_t srcIdx = 0;
                    int    lineY  = segY;
                    for (const auto& ln : lines) {
                        std::vector<size_t> idxs;
                        idxs.reserve(ln.size());
                        size_t lnIdx = 0;
                        while (srcIdx < seg.text.size() && lnIdx < ln.size()) {
                            wchar_t sc = seg.text[srcIdx];
                            if (sc == L'\r') { srcIdx++; continue; }
                            if (sc == L'\n') break;
                            idxs.push_back(srcIdx);
                            srcIdx++;
                            lnIdx++;
                        }
                        if (srcIdx < seg.text.size() && seg.text[srcIdx] == L'\r') srcIdx++;
                        if (srcIdx < seg.text.size() && seg.text[srcIdx] == L'\n') srcIdx++;

                        size_t j = 0;
                        int x = segLeft - m_codeScrollX;
                        while (j < ln.size()) {
                            COLORREF c = idxs[j] < colors.size() ? colors[idxs[j]] : kDefaultCodeColor;
                            size_t e = j + 1;
                            while (e < ln.size()) {
                                COLORREF nc = idxs[e] < colors.size() ? colors[idxs[e]] : kDefaultCodeColor;
                                if (nc != c) break;
                                e++;
                            }
                            SetTextColor(hdc, c);
                            int n = (int)(e - j);
                            TextOutW(hdc, x, lineY, ln.data() + j, n);
                            x += n * codeCharW;
                            j = e;
                        }

                        lineY += codeLineH;
                    }

                    SelectClipRgn(hdc, NULL);
                    DeleteObject(clip);
                } else {
                    if (HasInlineMd(seg.text)) {
                        auto runs = ParseMdRuns(seg.text);
                        LayoutMd(hdc, runs, segLeft, segY, segRight - segLeft, proseLineH,
                                 proseFonts, theme.prose_text, true);
                    } else {
                        SelectObject(hdc, hProseFont);
                        SetTextColor(hdc, theme.prose_text);
                        RECT tr = { segLeft, segY, segRight, segY + sh };
                        DrawText(hdc, seg.text.c_str(), -1, &tr,
                                 DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                    }
                }

                segY += sh + segGap;
            }
            SetTextColor(hdc, theme.prose_text);

            if (m_config.show_timestamps && msg.hour >= 0) {
                HFONT hTimeFont = CreateFont(
                    10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                SelectObject(hdc, hTimeFont);
                SetTextColor(hdc, RGB(160, 160, 160));
                wchar_t timeBuf[16];
                wsprintfW(timeBuf, L"%02d:%02d", msg.hour, msg.minute);
                RECT tr = { bubbleX + mm.bubbleW - 42, drawY - 12, bubbleX + mm.bubbleW - 2, drawY + 2 };
                DrawText(hdc, timeBuf, -1, &tr,
                         DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
                DeleteObject(hTimeFont);
            }
        }
        drawY += mm.bubbleH + bubblePadding;
    }

    // -------- Search bar --------
    if (m_searchActive) {
        RECT srcRect = { 0, 0, width, 26 };
        HBRUSH hSb = CreateSolidBrush(RGB(30, 100, 60));
        FillRect(hdc, &srcRect, hSb);
        DeleteObject(hSb);
        HFONT hSearchFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hSearchFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        std::wstring shown = L"Search: " + m_searchQuery + L"_  (Enter = jump, Esc = close)";
        RECT srTr = { 10, 4, width - 10, 22 };
        DrawText(hdc, shown.c_str(), -1, &srTr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hSearchFont);
    }

    // -------- Hotkey hints overlay (F2) --------
    if (m_hintsVisible) {
        const int panelW = std::min(width - 40, 320);
        const int panelH = std::min(fullHeight - 80, 360);
        int px = (width - panelW) / 2;
        int py = (fullHeight - panelH) / 2;

        HBRUSH hPanelBg = CreateSolidBrush(RGB(15, 15, 20));
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, hPanelBg);
        RoundRect(hdc, px, py, px + panelW, py + panelH, 8, 8);
        SelectObject(hdc, oldB);
        DeleteObject(hPanelBg);

        HFONT hHdr = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hHdr);
        SetTextColor(hdc, RGB(120, 200, 255));
        RECT hr = { px + 14, py + 10, px + panelW - 10, py + 30 };
        DrawText(hdc, L"Hotkeys (F2 to close)", -1, &hr,
                 DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hHdr);

        HFONT hRow = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SelectObject(hdc, hRow);
        SetTextColor(hdc, RGB(220, 220, 220));

        int ly = py + 36;
        for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
            std::string b = ConfigLoader::BindingToString(m_config.hotkeys.bindings[i]);
            std::string lbl = ConfigLoader::ActionLabel((HotkeyAction)i);
            while ((int)b.size() < 14) b += ' ';
            std::string row = "  " + b + lbl;
            int sz = MultiByteToWideChar(CP_UTF8, 0, row.data(), (int)row.size(), NULL, 0);
            std::wstring wrow(sz, 0);
            MultiByteToWideChar(CP_UTF8, 0, row.data(), (int)row.size(), &wrow[0], sz);
            RECT lr = { px + 14, ly, px + panelW - 10, ly + 16 };
            DrawText(hdc, wrow.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            ly += 16;
        }
        const wchar_t* fixed[] = {
            L"  F1            About",
            L"  F2            This panel",
            L"  F11           Runtime settings",
            L"  Ctrl+E        Export chat to .md",
            L"  Ctrl+F        Search chat",
            L"  Ctrl+= / -    Font size",
            L"  Ctrl+Shift+R  Regenerate last answer",
            L"  Shift+Left/Right  Scroll code horizontally",
        };
        ly += 4;
        for (auto* l : fixed) {
            RECT lr = { px + 14, ly, px + panelW - 10, ly + 16 };
            DrawText(hdc, l, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            ly += 16;
        }
        DeleteObject(hRow);
    }

    // -------- Mode banner --------
    if (m_moveMode || m_selectMode) {
        RECT banner = { 0, 0, width, 22 };
        COLORREF bgColor = m_selectMode ? RGB(40, 120, 200) : RGB(180, 80, 30);
        HBRUSH hBan = CreateSolidBrush(bgColor);
        FillRect(hdc, &banner, hBan);
        DeleteObject(hBan);

        HFONT hBanFont = CreateFont(
            14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hBanFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        const wchar_t* text = m_selectMode
            ? L"SELECT MODE - click a bot bubble to copy / a user bubble to edit. Esc to cancel."
            : L"MOVE MODE - drag to move, edges to resize, F10 to lock";
        DrawText(hdc, text, -1, &banner, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        DeleteObject(hBanFont);
    }

    // -------- Live transcript bar --------
    {
        RECT barRect = { 0, fullHeight - barHeight, width, fullHeight };

        HBRUSH hBarBg = CreateSolidBrush(theme.bar_bg);
        FillRect(hdc, &barRect, hBarBg);
        DeleteObject(hBarBg);

        HPEN hLinePen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        HPEN oldLinePen = (HPEN)SelectObject(hdc, hLinePen);
        MoveToEx(hdc, 0, barRect.top, NULL);
        LineTo(hdc, width, barRect.top);
        SelectObject(hdc, oldLinePen);
        DeleteObject(hLinePen);

        {
            int dotR = 5;
            int dotX = 12;
            int dotY = barRect.top + 12;
            float bright = std::min(1.0f, m_audioLevel / 0.05f);
            int g = 80 + (int)(bright * 175);
            HBRUSH dot = CreateSolidBrush(RGB(40, g, 40));
            HBRUSH oldDot = (HBRUSH)SelectObject(hdc, dot);
            Ellipse(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR);
            SelectObject(hdc, oldDot);
            DeleteObject(dot);
        }

        if (m_inflightCalls.load() > 0) {
            int dotR = 5;
            int dotX = 28;
            int dotY = barRect.top + 12;
            HBRUSH dot = CreateSolidBrush(RGB(240, 160, 40));
            HBRUSH oldDot = (HBRUSH)SelectObject(hdc, dot);
            Ellipse(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR);
            SelectObject(hdc, oldDot);
            DeleteObject(dot);
        }

        HFONT hBadgeFont = CreateFont(
            13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hBadgeFont);
        const wchar_t* badge = m_autoMode ? L"AUTO ON" : L"AUTO OFF";
        SetTextColor(hdc, m_autoMode ? RGB(120, 230, 120) : RGB(140, 140, 140));
        RECT badgeRect = { width - 80, barRect.top + 4, width - 6, barRect.top + 20 };
        DrawText(hdc, badge, -1, &badgeRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hBadgeFont);

        if (m_tokensIn > 0 || m_tokensOut > 0) {
            HFONT hTokFont = CreateFont(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            SelectObject(hdc, hTokFont);
            SetTextColor(hdc, RGB(140, 140, 140));
            wchar_t tokBuf[64];
            wsprintfW(tokBuf, L"%lldk in / %lldk out",
                      (long long)(m_tokensIn / 1000), (long long)(m_tokensOut / 1000));
            RECT tokRect = { width - 200, barRect.top + 22, width - 90, barRect.bottom - 4 };
            DrawText(hdc, tokBuf, -1, &tokRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(hTokFont);
        }

        HFONT hTransFont = CreateFont(
            14, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hTransFont);
        SetTextColor(hdc, theme.bar_text);

        std::wstring shown = m_lastTranscript.empty() ? std::wstring(L"(press F9 to enable auto-answer)") : m_lastTranscript;
        RECT tRect = { 44, barRect.top + 4, width - 90, barRect.bottom - 4 };
        DrawText(hdc, shown.c_str(), -1, &tRect,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        DeleteObject(hTransFont);
    }

    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hUserBrush);
    DeleteObject(hBotBrush);
    DeleteObject(hCodeBrush);
    DeleteObject(hNullPen);
    DeleteObject(hProseFont);
    DeleteObject(hProseBold);
    DeleteObject(hProseItalic);
    DeleteObject(hProseBoldIt);
    DeleteObject(hCodeFont);

    EndPaint(hwnd, &ps);
}
