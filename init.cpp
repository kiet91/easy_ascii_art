// Setup: creates the data folder, a default config.txt and a copy of the console font in data/ when they are missing.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// Written to config.txt when there is none; same values as CONFIG's defaults in main.cpp.
const char* DEFAULT_CONFIG = R"(# distance: euclidean or l1
distance=euclidean
# dither: 1 = Floyd-Steinberg on, 0 = off
dither=1
# tone_weight: 0 to 1, how much brightness counts; shape weight = 1 - tone_weight
tone_weight=0.6
# fit: 1 = whole image fits the console (scaled up or down), 0 = original size, width capped to the console
fit=1
# cell_aspect: terminal cell height / width, worked out from the font unless set here (e.g. cell_aspect=2.2)
# color: off, separate (glyph from brightness, colour from hue) or joint (best glyph + colour pair, much slower)
color=separate
# color_output: ansi (escape codes, fast) or api (SetConsoleTextAttribute, slow, also works in the old console)
color_output=ansi
output=data/out.txt
)";

std::string FONT;   // the font in data/ the glyphs are measured from, set by init()

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// files in folder with one of these extensions (any case), sorted by name
std::vector<std::string> listFiles(const char* folder, const std::vector<std::string>& exts) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec))
        if (e.is_regular_file() && std::find(exts.begin(), exts.end(), lower(e.path().extension().string())) != exts.end())
            out.push_back(e.path().generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

// font face in Windows Terminal's settings.json: the profile with this guid, else profiles.defaults ("" if neither has one)
std::string wtFontFace(const std::string& json, const std::string& guid) {
    std::vector<std::string> tok;   // strings (without quotes) and the characters {}[]:, ; comments and other values skipped
    std::vector<bool> str;
    for (size_t i = 0; i < json.size(); i++) {
        if (json.compare(i, 2, "//") == 0) { i = json.find('\n', i); if (i == std::string::npos) break; }
        else if (json.compare(i, 2, "/*") == 0) { i = json.find("*/", i + 2); if (i == std::string::npos) break; i++; }
        else if (json[i] == '"') {
            std::string s;
            for (i++; i < json.size() && json[i] != '"'; i++) {
                if (json[i] == '\\' && i + 1 < json.size()) i++;
                s += json[i];
            }
            tok.push_back(s); str.push_back(true);
        }
        else if (std::strchr("{}[]:,", json[i])) { tok.push_back(std::string(1, json[i])); str.push_back(false); }
    }
    auto key = [&](size_t k, const char* name) { return str[k] && tok[k] == name && k + 2 < tok.size() && tok[k + 1] == ":"; };
    auto open = [&](size_t k) { return !str[k] && (tok[k] == "{" || tok[k] == "["); };
    auto shut = [&](size_t k) { return !str[k] && (tok[k] == "}" || tok[k] == "]"); };
    auto faceIn = [&](size_t a) -> std::string {   // first face / fontFace inside the object opening at token a
        int depth = 0;
        for (size_t k = a; k < tok.size(); k++) {
            if (open(k)) depth++;
            else if (shut(k) && --depth == 0) break;
            else if ((key(k, "face") || key(k, "fontFace")) && str[k + 2]) {
                std::string f = tok[k + 2];
                return f.substr(0, f.find(','));   // "Cascadia Mono, Segoe UI Emoji": the first is the font
            }
        }
        return "";
    };
    std::string face;
    for (size_t k = 0; k + 2 < tok.size() && face.empty(); k++)
        if (key(k, "guid") && str[k + 2] && lower(tok[k + 2]) == lower(guid)) {
            size_t s = k;   // walk back to the '{' of the profile holding this guid
            for (int depth = 0; s > 0; ) {
                s--;
                if (shut(s)) depth++;
                else if (open(s) && depth-- == 0) break;
            }
            face = faceIn(s);
        }
    for (size_t k = 0; k + 2 < tok.size() && face.empty(); k++)
        if (key(k, "defaults") && tok[k + 2] == "{") face = faceIn(k + 2);
    return face;
}

// font name in a Mac Terminal profile's archived NSFont (as XML): the first string that is not archive bookkeeping
std::string nsFontName(const std::string& xml) {
    for (size_t i = xml.find("<string>"); i != std::string::npos; i = xml.find("<string>", i + 1)) {
        std::string s = xml.substr(i + 8, xml.find("</string>", i) - i - 8);
        if (!s.empty() && s[0] != '$' && s.compare(0, 2, "NS") != 0) return s;
    }
    return "";
}

#ifndef _WIN32
// output of a shell command, without the trailing newline
std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}
#endif

#ifdef _WIN32
// Windows Terminal: the face set for this profile in its settings.json. Old console: its current font.
std::string consoleFontFace() {
    if (std::getenv("WT_SESSION")) {
        const char* local = std::getenv("LOCALAPPDATA");
        const char* guid = std::getenv("WT_PROFILE_ID");
        for (const char* p : {"\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json",
                              "\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json",
                              "\\Microsoft\\Windows Terminal\\settings.json"}) {
            std::ifstream f(std::string(local ? local : "") + p);
            if (!f) continue;
            std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string face = wtFontFace(json, guid ? guid : "");
            return face.empty() ? "Cascadia Mono" : face;   // Windows Terminal's default
        }
        return "Cascadia Mono";
    }
    CONSOLE_FONT_INFOEX info{};
    info.cbSize = sizeof info;
    char face[LF_FACESIZE * 3] = "";
    if (GetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &info))
        WideCharToMultiByte(CP_UTF8, 0, info.FaceName, -1, face, sizeof face, NULL, NULL);
    return face[0] ? face : "Consolas";
}

// the font file Windows has registered for this face (per-user fonts first), "" if none
std::string fontFileForFace(const std::string& face) {
    std::string want = lower(face);
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key;
        if (RegOpenKeyExA(root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        std::string file;
        for (DWORD i = 0; file.empty(); i++) {
            char name[512], data[MAX_PATH + 1] = "";
            DWORD nameLen = sizeof name, dataLen = MAX_PATH, type;
            LONG r = RegEnumValueA(key, i, name, &nameLen, NULL, &type, (BYTE*)data, &dataLen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || type != REG_SZ) continue;
            std::string n = lower(name);   // "cascadia mono regular (truetype)", "cambria & cambria math (truetype)"
            n = n.substr(0, n.find(" ("));
            for (size_t a = 0, b; a != std::string::npos; a = b == std::string::npos ? b : b + 3) {
                b = n.find(" & ", a);
                std::string part = n.substr(a, b == std::string::npos ? b : b - a);
                if (part == want || part == want + " regular") { file = data; break; }
            }
        }
        RegCloseKey(key);
        if (!file.empty()) {
            if (file.find('\\') != std::string::npos) return file;   // per-user fonts are stored with their full path
            const char* win = std::getenv("WINDIR");
            return std::string(win ? win : "C:\\Windows") + "\\Fonts\\" + file;
        }
    }
    return "";
}
#elif defined(__APPLE__)
// shortcut: only Terminal.app's default profile is read; other terminals get Terminal's default font (SF Mono)
std::string consoleFontFace() {
    const char* tp = std::getenv("TERM_PROGRAM");
    if (tp && std::string(tp) == "Apple_Terminal") {
        std::string profile = run("defaults read com.apple.Terminal 'Default Window Settings' 2>/dev/null");
        std::string xml = run("plutil -extract " + shellQuote("Window Settings." + profile + ".Font") +
                              " raw -o - ~/Library/Preferences/com.apple.Terminal.plist 2>/dev/null | base64 -D 2>/dev/null"
                              " | plutil -convert xml1 -o - - 2>/dev/null");
        std::string name = nsFontName(xml);
        if (!name.empty()) return name;
    }
    return "SFMono-Regular";
}

// font file whose name matches the face (case, spaces and '-' ignored; a "Regular" suffix on either side allowed)
std::string fontFileForFace(const std::string& face) {
    auto squash = [](std::string s) {
        s = lower(s);
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), s.end());
        return s;
    };
    std::string want = squash(face);
    const char* home = std::getenv("HOME");
    for (std::string dir : {std::string(home ? home : "") + "/Library/Fonts", std::string("/Library/Fonts"),
                            std::string("/System/Library/Fonts"), std::string("/System/Library/Fonts/Supplemental"),
                            std::string("/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts"),
                            std::string("/Applications/Utilities/Terminal.app/Contents/Resources/Fonts")}) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string stem = squash(e.path().stem().string());
            if (stem == want || stem + "regular" == want || stem == want + "regular") return e.path().string();
        }
    }
    return "";
}
#else
// shortcut: GNOME's monospace font (what GNOME Terminal and most VTE terminals use by default); other terminals' own settings are not read
std::string consoleFontFace() {
    std::string f = run("gsettings get org.gnome.desktop.interface monospace-font-name 2>/dev/null");   // 'Ubuntu Mono 13'
    if (f.size() > 2 && f.front() == '\'') f = f.substr(1, f.size() - 2);
    size_t sp = f.find_last_of(' ');
    if (sp != std::string::npos && f.find_first_not_of("0123456789.", sp + 1) == std::string::npos) f = f.substr(0, sp);   // drop the size
    return f.empty() ? "monospace" : f;
}

std::string fontFileForFace(const std::string& face) {
    return run("fc-match " + shellQuote(face) + " --format=%{file} 2>/dev/null");
}
#endif

// makes data/ and config.txt if missing, and copies the console's font into data/ if data/ has no font yet;
// leaves whatever is there alone. Only the first call in a run checks.
bool init() {
    static bool done = false;
    if (done) return true;
    std::error_code ec;
    std::filesystem::create_directories("data", ec);   // nothing happens if it exists
    if (ec) { std::cerr << "cannot create data/: " << ec.message() << "\n"; return false; }
    if (!std::filesystem::exists("config.txt")) {
        std::ofstream f("config.txt");
        f << DEFAULT_CONFIG;
        if (!f) { std::cerr << "cannot write config.txt\n"; return false; }
    }
    std::vector<std::string> fonts = listFiles("data", {".ttf", ".otf", ".ttc"});
    if (!fonts.empty()) FONT = fonts[0];
    else {
        std::string face = consoleFontFace(), file = fontFileForFace(face);
        if (file.empty() || !std::filesystem::is_regular_file(file)) {
            std::cerr << "no font in data/ and no file found for the console font \"" << face << "\": copy a .ttf into data/\n";
            return false;
        }
        FONT = "data/" + std::filesystem::path(file).filename().string();
        std::filesystem::copy_file(file, FONT, ec);
        if (ec) { std::cerr << "cannot copy " << file << ": " << ec.message() << "\n"; return false; }
        std::cout << "console font \"" << face << "\" copied to " << FONT << "\n";
    }
    done = true;
    return true;
}
