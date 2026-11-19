// Image side: loading, resizing to the console, border, dithering, colour, and turning an image into an ASCII string.
#pragma once
#include "ascii_map.cpp"   // glyph sets and matchGlyph; also brings stb_image_write and stb_truetype
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif

const int BORDER = GRID / 2; // border = 1px for 3x3

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING  
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

enum COLORMODE { COLOR_OFF, COLOR_SEPARATE, COLOR_JOINT };

struct CONSOLECOLOR { float rgb[3]; int ansi; unsigned short attr; };

// Campell pallette
const CONSOLECOLOR PALETTE[] = {
    {{242, 242, 242}, 97, 15},   // bright white
    {{231,  72,  86}, 91, 12},   // bright red
    {{ 22, 198,  12}, 92, 10},   // bright green
    {{249, 241, 165}, 93, 14},   // bright yellow
    {{ 59, 120, 255}, 94,  9},   // bright blue
    {{180,   0, 158}, 95, 13},   // bright magenta
    {{ 97, 214, 214}, 96, 11},   // bright cyan
    {{204, 204, 204}, 37,  7},   // white
    {{118, 118, 118}, 90,  8},   // gray (bright black)
    {{197,  15,  31}, 31,  4},   // red
    {{ 19, 161,  14}, 32,  2},   // green
    {{193, 156,   0}, 33,  6},   // yellow
    {{  0,  55, 218}, 34,  1},   // blue
    {{136,  23, 152}, 35,  5},   // magenta
    {{ 58, 150, 221}, 36,  3},   // cyan
};
const int COLORS = sizeof(PALETTE) / sizeof(PALETTE[0]);

std::pair<int,int> getConsoleSize() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO c{}; GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &c);
    return {c.srWindow.Right - c.srWindow.Left + 1, c.srWindow.Bottom - c.srWindow.Top + 1};
#else
    winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    return {ws.ws_col, ws.ws_row};
#endif
}

void clearScreen() {
#ifdef _WIN32
    std::system("cls");
#else
    std::cout << "\x1b[2J\x1b[H";   // clear, cursor to top-left
#endif
}

// returns a w x h x comp buffer to free with free().
unsigned char* loadForConsole(const char* path, int cols, int rows, bool fit, float cellAspect, int comp, int& w, int& h) {
    int iw, ih, n;
    unsigned char* img = stbi_load(path, &iw, &ih, &n, comp);
    if (!img) { std::cerr << "load failed: " << stbi_failure_reason() << "\n"; return nullptr; }
    double rowsH = ih / cellAspect;   // the image's height in rows at full width
    double scale = iw > cols ? (double)cols / iw : 1;
    if (fit) scale = (double)cols / iw < rows / rowsH ? (double)cols / iw : rows / rowsH;
    w = (int)std::lround(iw * scale);
    h = (int)std::lround(rowsH * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned char* out = stbir_resize_uint8_srgb(img, iw, ih, 0, NULL, w, h, 0, comp == 3 ? STBIR_RGB : STBIR_1CHANNEL);
    stbi_image_free(img);
    if (!out) std::cerr << "resize failed\n";
    return out;
}

std::vector<float> padEdge(const unsigned char* img, int w, int h, int pad, int comp = 1) {
    int pw = w + 2 * pad, ph = h + 2 * pad;
    std::vector<float> out((size_t)pw * ph * comp);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int sx = x - pad, sy = y - pad;
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            if (sy < 0) sy = 0;
            if (sy >= h) sy = h - 1;
            for (int c = 0; c < comp; c++)
                out[((size_t)y * pw + x) * comp + c] = img[((size_t)sy * w + sx) * comp + c];
        }
    return out;
}

// Floyd-Steinberg dithering error
// check center pixel to mean
void diffuseError(std::vector<float>& buf, int w, int h, int x, int y, const float* err, int comp) {
    const int dx[4] = {1, -1, 0, 1}, dy[4] = {0, 1, 1, 1};
    const float part[4] = {7 / 16.0f, 3 / 16.0f, 5 / 16.0f, 1 / 16.0f};
    int pw = w + 2 * BORDER;
    for (int k = 0; k < 4; k++) {
        int nx = x + dx[k], ny = y + dy[k];
        if (nx >= 0 && nx < w && ny < h)
            for (int c = 0; c < comp; c++)
                buf[((size_t)(ny + BORDER) * pw + nx + BORDER) * comp + c] += err[c] * part[k];
    }
}

// for a one-value-per-pixel (brightness) buffer.
void diffuseError(std::vector<float>& buf, int w, int h, int x, int y, float err) {
    diffuseError(buf, w, h, x, y, &err, 1);
}

// threads OpenMP can use, 1 when built without -fopenmp
int maxThreads() {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

// runs step(x, y) for every pixel, rows shared out between threads (row y on thread y % threads)
// with dither, each row waits until the row above is BORDER + 2 pixels ahead, so every value a window reads
// is already final: same result as one thread
template <class STEP> void wavefront(int w, int h, bool dither, int threads, STEP step) {
    std::vector<std::atomic<int>> done(h);   // pixels finished per row
#ifdef _OPENMP
#pragma omp parallel for schedule(static, 1) num_threads(threads)
#else
    (void)threads;
#endif
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int need = x + BORDER + 2 < w ? x + BORDER + 2 : w;
            if (dither && y > 0)
                while (done[y - 1].load(std::memory_order_acquire) < need) {}
            step(x, y);
            done[y].store(x + 1, std::memory_order_release);
        }
    }
}

// left to right
std::string imageToAscii(const unsigned char* img, int w, int h, const std::unordered_map<char, float>& tones, const std::unordered_map<char, float*>& shapes, DISTANCE mode, bool dither, int threads = 1) {
    std::vector<float> buf = padEdge(img, w, h, BORDER);
    int pw = w + 2 * BORDER;
    std::string out(h > 0 ? (size_t)(w + 1) * h - 1 : 0, '\n');   // rows joined with '\n'
    wavefront(w, h, dither, threads, [&](int x, int y) {
        float window[CELLS];
        for (int i = 0; i < CELLS; i++)   // window top-left in the bordered buffer is (x, y)
            window[i] = buf[(size_t)(y + i / GRID) * pw + x + i % GRID];
        char c = matchGlyph(window, tones, shapes, mode);
        out[(size_t)y * (w + 1) + x] = c;
        if (dither) diffuseError(buf, w, h, x, y, window[CELLS / 2] - tones.at(c));
    });
    return out;
}

float grayOf(const float* rgb) {
    return (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) / 256;
}

void toHue(const float* rgb, float* hue) {
    float m = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
    if (rgb[2] > m) m = rgb[2];
    for (int i = 0; i < 3; i++) hue[i] = m > 0 ? rgb[i] * 255 / m : 255;
}

int nearestColor(const float* hue, DISTANCE mode) {
    int best = 0;
    float bestD = 1e30f;
    for (int c = 0; c < COLORS; c++) {
        float ph[3];
        toHue(PALETTE[c].rgb, ph);
        float d = distance(hue, ph, 3, mode);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

void matchJoint(const float* window, const float* hue, const std::unordered_map<char, float>& tones, const std::unordered_map<char, float*>& shapes, DISTANCE mode, char& glyph, int& color) {
    float tone, shape[CELLS], hueD[COLORS], light[COLORS];
    meanRemoved(window, CELLS, tone, shape);
    for (int c = 0; c < COLORS; c++) {
        float ph[3];
        toHue(PALETTE[c].rgb, ph);
        hueD[c] = distance(hue, ph, 3, mode);
        light[c] = grayOf(PALETTE[c].rgb) / 255;
    }
    float best = 1e30f;
    glyph = ' ';
    color = 0;
    for (auto& t : tones) {
        const float* gs = shapes.at(t.first);
        for (int c = 0; c < COLORS; c++) {
            float lit = t.second * light[c], s[CELLS];
            for (int i = 0; i < CELLS; i++) s[i] = gs[i] * light[c];
            float score = TONE_WEIGHT * (distance(&tone, &lit, 1, mode) + hueD[c]) / 2 + SHAPE_WEIGHT * distance(shape, s, CELLS, mode);
            if (score < best) { best = score; glyph = t.first; color = c; }
        }
    }
}

// color overload 
std::string imageToAscii(const unsigned char* rgb, int w, int h, const std::unordered_map<char, float>& tones, const std::unordered_map<char, float*>& shapes, DISTANCE mode, bool dither, COLORMODE cm, std::vector<int>& colors, int threads = 1) {
    std::vector<unsigned char> gray((size_t)w * h);
    for (size_t i = 0; i < gray.size(); i++) {
        float px[3] = {(float)rgb[i * 3], (float)rgb[i * 3 + 1], (float)rgb[i * 3 + 2]};
        gray[i] = (unsigned char)grayOf(px);
    }
    std::vector<float> buf = padEdge(gray.data(), w, h, BORDER), hues = padEdge(rgb, w, h, BORDER, 3);
    for (size_t i = 0; i < hues.size(); i += 3) toHue(&hues[i], &hues[i]);
    int pw = w + 2 * BORDER;
    std::string out(h > 0 ? (size_t)(w + 1) * h - 1 : 0, '\n');   // rows joined with '\n'
    colors.assign(out.size(), -1);                                  // -1 stays on the newlines
    wavefront(w, h, dither, threads, [&](int x, int y) {
        float window[CELLS], hue[3] = {0, 0, 0};
        for (int i = 0; i < CELLS; i++) {
            size_t p = (size_t)(y + i / GRID) * pw + x + i % GRID;
            window[i] = buf[p];
            for (int k = 0; k < 3; k++) hue[k] += hues[p * 3 + k] / CELLS;   // mean hue of the window
        }
        char c;
        int col;
        if (cm == COLOR_JOINT) matchJoint(window, hue, tones, shapes, mode, c, col);
        else { c = matchGlyph(window, tones, shapes, mode); col = nearestColor(hue, mode); }
        out[(size_t)y * (w + 1) + x] = c;
        colors[(size_t)y * (w + 1) + x] = col;
        if (dither) {
            float lit = cm == COLOR_JOINT ? tones.at(c) * grayOf(PALETTE[col].rgb) / 255 : tones.at(c);
            diffuseError(buf, w, h, x, y, window[CELLS / 2] - lit);
            size_t p = ((size_t)(y + BORDER) * pw + x + BORDER) * 3;
            float ph[3], err[3];
            toHue(PALETTE[col].rgb, ph);
            for (int k = 0; k < 3; k++) err[k] = hues[p + k] - ph[k];
            diffuseError(hues, w, h, x, y, err, 3);
        }
    });
    return out;
}

std::string ansiString(const std::string& art, const std::vector<int>& colors) {
    std::string out;
    int current = -1;
    for (size_t i = 0; i < art.size(); i++) {
        if (art[i] != ' ' && art[i] != '\n' && colors[i] != current) {
            current = colors[i];
            out += "\x1b[" + std::to_string(PALETTE[current].ansi) + "m";
        }
        out += art[i];
    }
    return out + "\x1b[0m";
}

void printColored(const std::string& art, const std::vector<int>& colors, bool ansi) {
#ifndef _WIN32
    (void)ansi;   // no console API outside Windows: always escape codes
    std::cout << ansiString(art, colors) << std::flush;
#else
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (ansi) {
        DWORD m = 0;
        if (GetConsoleMode(out, &m)) SetConsoleMode(out, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        std::cout << ansiString(art, colors) << std::flush;
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{}; // old console support (real slow)
    GetConsoleScreenBufferInfo(out, &info);
    size_t start = 0;
    int current = -1;
    for (size_t i = 0; i < art.size(); i++) {
        if (art[i] == ' ' || art[i] == '\n' || colors[i] == current) continue;
        std::cout.write(art.data() + start, i - start).flush();
        SetConsoleTextAttribute(out, (info.wAttributes & 0xF0) | PALETTE[colors[i]].attr);
        current = colors[i];
        start = i;
    }
    std::cout.write(art.data() + start, art.size() - start).flush();
    SetConsoleTextAttribute(out, info.wAttributes);
#endif
}
