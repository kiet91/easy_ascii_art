// Glyph side: measures each ASCII glyph, builds the tone and shape sets, caches, and matches a 3x3 window to a glyph.
// Included by img.cpp. Each stb implementation may be compiled only once per program, so stb_image and stb_image_resize2
// live in img.cpp and this file keeps only the two headers it uses.
#pragma once
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_truetype.h"
#include <iostream>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

const int GRID = 3;  
const int CELLS = GRID * GRID; 

float TONE_WEIGHT = 3.0f / 5;
float SHAPE_WEIGHT = 1 - TONE_WEIGHT;

enum DISTANCE { EUCLIDEAN, L1 };

bool loadFont(const char* fontPath, std::vector<unsigned char>& ttf, stbtt_fontinfo& font) {
    std::ifstream f(fontPath, std::ios::binary);
    ttf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (ttf.empty() || !stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        std::cerr << "cannot load " << fontPath << "\n";
        return false;
    }
    return true;
}

void renderGlyph(const stbtt_fontinfo& font, int c, float PX, std::vector<unsigned char>& canvas, int& cellW, int& cellH) {
    float scale = stbtt_ScaleForPixelHeight(&font, PX);
    int ascent, descent, lineGap, adv, lsb;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    stbtt_GetCodepointHMetrics(&font, c, &adv, &lsb);
    float baselineF = ascent * scale;                  
    int baseline = (int)std::floor(baselineF);
    float shiftY = baselineF - baseline;       
    cellH = (int)std::lround((ascent - descent) * scale);
    cellW = cellH;
    float scaleX = (float)cellW / adv;            

    canvas.assign((size_t)cellW * cellH, 0);
    int gw, gh, xo, yo;
    unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&font, scaleX, scale, 0.0f, shiftY, c, &gw, &gh, &xo, &yo);
    if (bmp) {
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++) {
                int cx = x + xo, cy = y + yo + baseline;   
                if (cx >= 0 && cx < cellW && cy >= 0 && cy < cellH)
                    canvas[(size_t)cy * cellW + cx] = bmp[y * gw + x];
            }
        stbtt_FreeBitmap(bmp, nullptr);
    }
}

// for debugging
void gridInfoPrint(const int* v) {
    static const char* names[CELLS] = {"TL", "TM", "TR", "ML", "MM", "MR", "BL", "BM", "BR"};
    for (int i = 0; i < CELLS; i++) std::cout << names[i] << "=" << v[i] << " ";
    std::cout << "MEAN=" << v[CELLS] << "\n";
}

// get 3x3 mean
void getGridMeans(const std::vector<unsigned char>& canvas, int cellW, int cellH, int* q) {
    for (int i = 0; i < CELLS; i++) {
        int x0 = (i % GRID) * cellW / GRID, x1 = (i % GRID + 1) * cellW / GRID;
        int y0 = (i / GRID) * cellH / GRID, y1 = (i / GRID + 1) * cellH / GRID;
        long sum = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                sum += canvas[(size_t)y * cellW + x];
        int n = (x1 - x0) * (y1 - y0);
        q[i] = (int)((sum + n / 2) / n);                   // rounded mean
    }
}

std::unordered_map<char, int*> glyphMap(const char* fontPath, float PX) {
    std::unordered_map<char, int*> glyphs;
    std::vector<unsigned char> ttf;
    stbtt_fontinfo font;
    if (!loadFont(fontPath, ttf, font)) return glyphs;

    // shortcut: pointers point into this static table, valid for the whole program; do not delete them
    static int table[95][CELLS + 1];
    std::vector<unsigned char> canvas;
    int cellW, cellH;
    for (int c = 32; c <= 126; c++) {
        int* v = table[c - 32];
        renderGlyph(font, c, PX, canvas, cellW, cellH);
        getGridMeans(canvas, cellW, cellH, v);
        int sum = 0;
        for (int i = 0; i < CELLS; i++) sum += v[i];
        v[CELLS] = (sum + CELLS / 2) / CELLS;              // rounded mean of the block means
        glyphs[(char)c] = v;
    }
    return glyphs;
}

bool writeGlyphPng(int c, const int* v) {
    unsigned char px[CELLS];
    for (int i = 0; i < CELLS; i++) px[i] = (unsigned char)v[i];
    std::string out = "data/glyph_" + std::to_string(c) + ".png";
    if (!stbi_write_png(out.c_str(), GRID, GRID, 1, px, GRID)) {
        std::cerr << "write failed\n";
        return false;
    }
    std::cout << out << " (" << GRID << "x" << GRID << ")\n";
    return true;
}

void meanRemoved(const float* v, int n, float& mean, float* shape) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    mean = sum / n;
    for (int i = 0; i < n; i++) shape[i] = v[i] - mean;
}

float euclidean(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(sum);
}

float l1(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += std::fabs(a[i] - b[i]);
    return sum;
}

float distance(const float* a, const float* b, int n, DISTANCE mode) {
    return mode == EUCLIDEAN ? euclidean(a, b, n) / std::sqrt((float)n) : l1(a, b, n) / n;
}

static float shapeTable[95][CELLS];

void glyphSets(const std::unordered_map<char, int*>& glyphs, std::unordered_map<char, float>& tones, std::unordered_map<char, float*>& shapes) {
    float maxMean = 0, maxBlock = 0;
    for (auto& g : glyphs) {
        float sum = 0;
        for (int i = 0; i < CELLS; i++) {
            sum += g.second[i];
            if (g.second[i] > maxBlock) maxBlock = (float)g.second[i];
        }
        if (sum / CELLS > maxMean) maxMean = sum / CELLS;
    }
    for (auto& g : glyphs) {
        float v[CELLS], mean;
        float* shape = shapeTable[g.first - 32];
        for (int i = 0; i < CELLS; i++) v[i] = (float)g.second[i];
        meanRemoved(v, CELLS, mean, shape);
        for (int i = 0; i < CELLS; i++) shape[i] *= 255 / maxBlock;
        tones[g.first] = mean * 255 / maxMean;
        shapes[g.first] = shape;
    }
}

// code, tone, CELLS shape values.
bool writeGlyphCache(const char* cachePath, const char* fontPath, float PX, const std::unordered_map<char, float>& tones, const std::unordered_map<char, float*>& shapes) {
    std::ofstream f(cachePath);
    f << fontPath << "\n" << PX << " " << GRID << "\n" << std::setprecision(9);   // 9 digits: floats read back exactly
    for (auto& t : tones) {
        f << (int)t.first << " " << t.second;
        for (int i = 0; i < CELLS; i++) f << " " << shapes.at(t.first)[i];
        f << "\n";
    }
    return (bool)f;
}

// read from cache
bool readGlyphCache(const char* cachePath, const char* fontPath, float PX, std::unordered_map<char, float>& tones, std::unordered_map<char, float*>& shapes) {
    std::ifstream f(cachePath);
    std::string font;
    float px;
    int grid;
    if (!std::getline(f, font) || font != fontPath || !(f >> px >> grid) || px != PX || grid != GRID) return false;
    for (int n = 0; n < 95; n++) {
        int c;
        float tone;
        if (!(f >> c >> tone) || c < 32 || c > 126) return false;
        float* shape = shapeTable[c - 32];
        for (int i = 0; i < CELLS; i++)
            if (!(f >> shape[i])) return false;
        tones[(char)c] = tone;
        shapes[(char)c] = shape;
    }
    return tones.size() == 95;
}

bool loadGlyphSets(const char* fontPath, float PX, const char* cachePath, std::unordered_map<char, float>& tones, std::unordered_map<char, float*>& shapes) {
    if (readGlyphCache(cachePath, fontPath, PX, tones, shapes)) return true;
    std::unordered_map<char, int*> glyphs = glyphMap(fontPath, PX);
    if (glyphs.empty()) return false;
    glyphSets(glyphs, tones, shapes);
    if (!writeGlyphCache(cachePath, fontPath, PX, tones, shapes)) std::cerr << "cannot write " << cachePath << "\n";
    return true;
}

// a terminal cell's height / width for this font: line height over the advance of 'M'.
// Line height as renderers take it: the typo metrics if the font asks for them, else Windows' win metrics on Windows
// and hhea elsewhere. stbtt__find_table and ttUSHORT are stb internals, usable here because stb is compiled in this file.
float fontCellAspect(const char* fontPath) {
    std::vector<unsigned char> ttf;
    stbtt_fontinfo font;
    if (!loadFont(fontPath, ttf, font)) return 0;
    int adv, lsb, ascent, descent, gap;
    stbtt_GetCodepointHMetrics(&font, 'M', &adv, &lsb);
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);   // hhea
    stbtt_uint32 os2 = stbtt__find_table(font.data, font.fontstart, "OS/2");
    if (os2 && (ttUSHORT(font.data + os2 + 62) & 0x80)) stbtt_GetFontVMetricsOS2(&font, &ascent, &descent, &gap);   // USE_TYPO_METRICS
#ifdef _WIN32
    else if (os2) { ascent = ttUSHORT(font.data + os2 + 74); descent = -ttUSHORT(font.data + os2 + 76); gap = 0; }   // usWinAscent / usWinDescent
#endif
    return adv > 0 ? (float)(ascent - descent + gap) / adv : 0;
}

// score = TONE_WEIGHT * tone distance + SHAPE_WEIGHT * shape distance
char matchGlyph(const float* window, const std::unordered_map<char, float>& tones, const std::unordered_map<char, float*>& shapes, DISTANCE mode) {
    float tone, shape[CELLS];
    meanRemoved(window, CELLS, tone, shape);
    char best = ' ';
    float bestScore = 1e30f;
    for (auto& t : tones) {
        float score = TONE_WEIGHT * distance(&tone, &t.second, 1, mode) + SHAPE_WEIGHT * distance(shape, shapes.at(t.first), CELLS, mode);
        if (score < bestScore) { bestScore = score; best = t.first; }
    }
    return best;
}
