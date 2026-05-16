// Standalone test runner. Builds as a separate console executable (tests.exe).
// Run with no args to execute all tests; non-zero exit code on failure.
//
// Build (handled by build_release.ps1):
//   g++ tests.cpp -o tests.exe -std=c++17 -DUNICODE -D_UNICODE
//
// Adding tests: write a TEST(name) { ... CHECK(expr); ... } block.

#include <iostream>
#include <vector>
#include <string>
#include <functional>

#include "Parsers.h"

// -------- Minimal test framework --------
static int s_passed = 0;
static int s_failed = 0;
struct TestEntry { const char* name; std::function<void()> fn; };
static std::vector<TestEntry>& Tests() {
    static std::vector<TestEntry> v; return v;
}
struct Reg { Reg(const char* n, std::function<void()> f) { Tests().push_back({n, f}); } };
#define TEST(name) \
    static void test_##name(); \
    static Reg reg_##name(#name, test_##name); \
    static void test_##name()
#define CHECK(expr) do { \
        if (expr) ++s_passed; \
        else { ++s_failed; std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                << "  " << #expr << "\n"; } \
    } while (0)

// -------- Helper: build wstring from narrow literal (ASCII-safe) --------
static std::wstring W(const char* s) {
    std::wstring w;
    while (*s) w += (wchar_t)(unsigned char)*s++;
    return w;
}

// ===========================================================================
// ExtractJsonStringField
// ===========================================================================

TEST(extract_simple) {
    auto v = parsers::ExtractJsonStringField(W("{\"name\":\"hello\"}"), W("name"));
    CHECK(v == W("hello"));
}

TEST(extract_with_whitespace) {
    auto v = parsers::ExtractJsonStringField(W("{ \"name\"  :   \"hello\" }"), W("name"));
    CHECK(v == W("hello"));
}

TEST(extract_with_escapes) {
    auto v = parsers::ExtractJsonStringField(W("{\"name\":\"line1\\nline2\\ttab\\\"q\\\"\"}"), W("name"));
    CHECK(v == W("line1\nline2\ttab\"q\""));
}

TEST(extract_missing_returns_empty) {
    auto v = parsers::ExtractJsonStringField(W("{\"other\":\"x\"}"), W("name"));
    CHECK(v.empty());
}

TEST(extract_finds_first_of_many) {
    auto v = parsers::ExtractJsonStringField(W("{\"x\":\"first\",\"x\":\"second\"}"), W("x"));
    CHECK(v == W("first"));
}

TEST(extract_skips_non_string_value) {
    // First match is a number, skip and find the string match
    auto v = parsers::ExtractJsonStringField(W("{\"name\":42,\"name\":\"actual\"}"), W("name"));
    CHECK(v == W("actual"));
}

// ===========================================================================
// StripInlineMd
// ===========================================================================

TEST(strip_bold) {
    CHECK(parsers::StripInlineMd(W("hello **world**!")) == W("hello world!"));
}

TEST(strip_italic) {
    CHECK(parsers::StripInlineMd(W("This is *important* text")) == W("This is important text"));
}

TEST(strip_inline_code) {
    CHECK(parsers::StripInlineMd(W("call `printf` to print")) == W("call printf to print"));
}

TEST(strip_preserves_math_asterisk) {
    // a*b should NOT have * stripped (both sides alnum)
    CHECK(parsers::StripInlineMd(W("compute a*b here")) == W("compute a*b here"));
}

TEST(strip_preserves_underscore_in_identifier) {
    CHECK(parsers::StripInlineMd(W("var snake_case_name = 1")) == W("var snake_case_name = 1"));
}

TEST(strip_mixed) {
    CHECK(parsers::StripInlineMd(W("**bold** and *italic* and `code`"))
          == W("bold and italic and code"));
}

// ===========================================================================
// ParseSegments
// ===========================================================================

TEST(parse_prose_only) {
    auto segs = parsers::ParseSegments(W("hello world"));
    CHECK(segs.size() == 1);
    CHECK(segs[0].isCode == false);
    CHECK(segs[0].text == W("hello world"));
}

TEST(parse_one_code_block) {
    auto segs = parsers::ParseSegments(W("before\n```\ncode here\n```\nafter"));
    CHECK(segs.size() == 3);
    CHECK(segs[0].isCode == false);
    CHECK(segs[1].isCode == true);
    CHECK(segs[1].text == W("code here"));
    CHECK(segs[2].isCode == false);
}

TEST(parse_captures_language) {
    auto segs = parsers::ParseSegments(W("```python\nprint(1)\n```"));
    CHECK(segs.size() == 1);
    CHECK(segs[0].isCode == true);
    CHECK(segs[0].language == W("python"));
    CHECK(segs[0].text == W("print(1)"));
}

TEST(parse_unmatched_open_treated_as_code) {
    auto segs = parsers::ParseSegments(W("text\n```\nint x = 1;"));
    CHECK(segs.size() == 2);
    CHECK(segs[0].isCode == false);
    CHECK(segs[1].isCode == true);
}

// ===========================================================================
// Test runner main
// ===========================================================================

int main() {
    std::cout << "Running " << Tests().size() << " tests...\n";
    for (auto& t : Tests()) {
        std::cout << "  " << t.name << "...\n";
        t.fn();
    }
    std::cout << "\n" << s_passed << " checks passed, " << s_failed << " failed\n";
    return s_failed == 0 ? 0 : 1;
}
