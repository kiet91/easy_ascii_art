#include "init.cpp"
#include "img.cpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

const float PX = 63;                          // glyph render size
const char* CACHE = "data/glyph_cache.txt";
const int THREAD_MIN_COLS = 400;              // threads only for consoles wider than this

struct CONFIG {
    DISTANCE distance = EUCLIDEAN;
    bool dither = true;
    bool fit = true;
    COLORMODE color = COLOR_SEPARATE;
    bool ansi = true;              // colour output: ANSI escape codes, otherwise SetConsoleTextAttribute
    float toneWeight = 3.0f / 5;  
    float cellAspect = 0;          // 0: from the font
    std::string output = "data/out.txt";
};

bool validNumber(const std::string& value, float lo, float hi, float& out) {
    char* end;
    float v = std::strtof(value.c_str(), &end);
    if (value.empty() || *end || !(v >= lo && v <= hi)) return false;   // also rejects nan
    out = v;
    return true;
}

// Reads key=value lines (blank lines and lines starting with # are skipped). Keys not in the file keep their default.
bool readConfig(const char* path, CONFIG& cfg) {
    std::ifstream f(path);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return false; }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        std::string key = line.substr(0, eq), value = eq == std::string::npos ? "" : line.substr(eq + 1);
        if (key == "distance" && (value == "euclidean" || value == "l1")) cfg.distance = value == "l1" ? L1 : EUCLIDEAN;
        else if (key == "dither" && (value == "1" || value == "0")) cfg.dither = value == "1";
        else if (key == "fit" && (value == "1" || value == "0")) cfg.fit = value == "1";
        else if (key == "color" && (value == "off" || value == "separate" || value == "joint"))
            cfg.color = value == "off" ? COLOR_OFF : value == "joint" ? COLOR_JOINT : COLOR_SEPARATE;
        else if (key == "color_output" && (value == "ansi" || value == "api")) cfg.ansi = value == "ansi";
        else if (key == "tone_weight" && validNumber(value, 0, 1, cfg.toneWeight)) {}       // validNumber stores it
        else if (key == "cell_aspect" && validNumber(value, 0.1f, 10, cfg.cellAspect)) {}
        else if (key == "output" && !value.empty()) cfg.output = value;
        else { std::cerr << path << ": bad line: " << line << "\n"; return false; }
    }
    return true;
}

// lists the images in data/ and returns the one picked by number ("" if there are none or input ends)
std::string chooseImage() {
    std::vector<std::string> images = listFiles("data", {".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif"});
    if (images.empty()) { std::cerr << "no images in data/\n"; return ""; }
    for (size_t i = 0; i < images.size(); i++) std::cout << i + 1 << ". " << images[i] << "\n";
    std::cout << "Image number: ";
    std::string line;
    while (std::getline(std::cin, line)) {
        char* end;
        long n = std::strtol(line.c_str(), &end, 10);
        if (!line.empty() && !*end && n >= 1 && n <= (long)images.size()) return images[n - 1];
        std::cout << "1 to " << images.size() << ": ";
    }
    return "";
}

int main() {
    if (!init()) return 1;
    CONFIG cfg;
    if (!readConfig("config.txt", cfg)) return 1;
    TONE_WEIGHT = cfg.toneWeight;
    SHAPE_WEIGHT = 1 - cfg.toneWeight;

    std::unordered_map<char, float> tones;
    std::unordered_map<char, float*> shapes;
    if (!loadGlyphSets(FONT.c_str(), PX, CACHE, tones, shapes)) return 1;
    if (cfg.cellAspect == 0) cfg.cellAspect = fontCellAspect(FONT.c_str());

    std::string image = chooseImage();
    if (image.empty()) return 1;
    std::cout << "Resize the console, then press Enter...";
    std::string enter;
    std::getline(std::cin, enter);

    std::pair<int,int> console = getConsoleSize();
    int w, h;
    unsigned char* img = loadForConsole(image.c_str(), console.first, console.second, cfg.fit, cfg.cellAspect, cfg.color == COLOR_OFF ? 1 : 3, w, h);
    if (!img) return 1;
    int threads = console.first > THREAD_MIN_COLS ? maxThreads() : 1;
    std::vector<int> colors;
    std::string art = cfg.color == COLOR_OFF ? imageToAscii(img, w, h, tones, shapes, cfg.distance, cfg.dither, threads)
                                             : imageToAscii(img, w, h, tones, shapes, cfg.distance, cfg.dither, cfg.color, colors, threads);
    free(img);

    clearScreen();
    if (cfg.color == COLOR_OFF) std::cout << art << std::flush;
    else printColored(art, colors, cfg.ansi);

    std::ofstream out(cfg.output);
    out << art << "\n";
    if (!out) { std::cerr << "\ncannot write " << cfg.output << "\n"; return 1; }
    return 0;
}
