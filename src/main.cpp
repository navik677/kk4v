#include <SDL2/SDL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/ctrl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include "vita_klog.h"

// The actual vitasdk newlib heap-size override (sceLibcHeapSize, tried
// first, is for Sony's official SceLibc and vita-elf-create silently
// accepts it as a no-op here). Default is 128MiB, which this
// content-heavy VN's cumulative XP3 texture/asset loading can exhaust,
// causing "Cannot allocate memory for Bitmap" on an ordinary ~1.9MB
// allocation later on.
//
// 320MiB crashed on launch, before this app's own boot log even got
// written -- this SELF isn't built with the UNSAFE attribute, and a
// "safe" homebrew app's actual grantable memory budget on real
// hardware is well under the Vita's full 512MB (much of it is
// reserved for the system/other budgets); the initial
// sceKernelAllocMemBlock-based heap reservation itself apparently
// fails outright above some ceiling well below that, rather than
// degrading to smaller successful mallocs at runtime. Keep this
// increase modest and comfortably under that ceiling.
//
// 160MiB launches fine but still isn't enough once previously-skipped
// KAG @bg crossfades actually execute (a content-patch fix elsewhere
// made a background image load that used to silently no-op): still
// the same "Cannot allocate memory for Bitmap" on an ordinary ~1.9MB
// allocation, just later into a real playthrough instead of at boot.
//
// Tried 224MiB next; that failed WORSE and EARLIER than either 160 or
// 320 -- an "Internal error" instantiating the built-in
// KAGWaveSoundBuffer class during the very first BGM/audio init at
// boot, before any scenario script even runs. This wasn't a case of
// "bigger heap = bigger ceiling before failure": something about this
// specific value's memory layout starves a native (non-newlib-heap)
// allocation that 160MiB and presumably other values don't. Back to
// 160MiB (the only value confirmed stable across this whole session)
// until the late-game bitmap OOM is addressed some other way --
// reducing what's actually resident at that point, not by guessing at
// more heap-size values one hardware round-trip at a time.
extern "C" unsigned int _newlib_heap_size_user = 160 * 1024 * 1024;

static bool g_LogOk = false;

static void InitFileLogging() {
    sceIoMkdir("ux0:data/kirikiroid2", 0777);
    // Truncate any previous run's log.
    SceUID fd = sceIoOpen("ux0:data/kirikiroid2/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    g_LogOk = fd >= 0;
    if (fd >= 0) sceIoClose(fd);
    KK4V_Log("[KK4V] Boot: log started");
}

static void OnTerminate() {
    KK4V_Log("[KK4V] std::terminate() called (uncaught exception or abort)");
    abort();
}

#include <ft2build.h>
#include FT_FREETYPE_H

#include "environ/Application.h"
#include "XP3Archive.h"
#include "environ/vkdefine.h"
#include "WindowIntf.h"
#include "tvpinputdefs.h"

#include <pthread.h>

extern "C" void* const __attribute__((used)) _force_pthread_cancel = (void*)&pthread_cancel;

extern bool TVPTerminated;
extern tTJSNI_Window* TVPGetActiveWindow();

// Shared with vita_backend.cpp
extern SDL_Window* s_SDLWindow;
extern SDL_Renderer* s_SDLRenderer;

#define SCREEN_W 960
#define SCREEN_H 544

// ============================================================================
// Colors
// ============================================================================
struct Color { uint8_t r, g, b, a; };
static const Color COL_BG          = {26,  26,  46,  255};
static const Color COL_HEADER_BG   = {45,  27,  78,  255};
static const Color COL_SELECTED_BG = {61,  43,  94,  255};
static const Color COL_ITEM_BG     = {37,  37,  64,  255};
static const Color COL_ACCENT      = {255, 107, 157, 255};
static const Color COL_SEPARATOR   = {51,  51,  85,  255};
static const Color COL_TITLE       = {232, 180, 248, 255};
static const Color COL_SELECTED_TX = {255, 215, 0,   255};
static const Color COL_NORMAL_TX   = {204, 204, 221, 255};
static const Color COL_HINT_TX     = {136, 136, 153, 255};
static const Color COL_TAG_TJS     = {136, 255, 136, 255};
static const Color COL_TAG_XP3     = {136, 187, 255, 255};

// ============================================================================
// FreeType text -> SDL_Texture renderer (fast: render to surface, upload once)
// ============================================================================
class SDLFontRenderer {
    FT_Library ftLib;
    FT_Face ftFace;
    bool valid;
public:
    SDLFontRenderer() : ftLib(nullptr), ftFace(nullptr), valid(false) {}

    bool init(const char* fontPath) {
        if (FT_Init_FreeType(&ftLib)) return false;
        if (FT_New_Face(ftLib, fontPath, 0, &ftFace)) {
            FT_Done_FreeType(ftLib);
            ftLib = nullptr;
            return false;
        }
        valid = true;
        return true;
    }

    ~SDLFontRenderer() {
        if (ftFace) FT_Done_Face(ftFace);
        if (ftLib) FT_Done_FreeType(ftLib);
    }

    // Render text to a new RGBA SDL_Surface (caller must SDL_FreeSurface)
    SDL_Surface* renderToSurface(int size, const Color& color, const char* text) {
        if (!valid || !text || !*text) return nullptr;
        FT_Set_Pixel_Sizes(ftFace, 0, size);

        // First pass: measure total width and max ascender/descender
        int totalW = 0;
        int maxAscender = 0, maxDescender = 0;
        {
            const char* p = text;
            while (*p) {
                unsigned long ch;
                int bytes;
                decodeUTF8(p, ch, bytes);
                p += bytes;
                if (FT_Load_Char(ftFace, ch, FT_LOAD_RENDER)) continue;
                FT_GlyphSlot g = ftFace->glyph;
                totalW += g->advance.x >> 6;
                if (g->bitmap_top > maxAscender) maxAscender = g->bitmap_top;
                int desc = (int)g->bitmap.rows - g->bitmap_top;
                if (desc > maxDescender) maxDescender = desc;
            }
        }
        if (totalW <= 0) return nullptr;

        int surfH = maxAscender + maxDescender + 2;
        if (surfH < size) surfH = size;

        SDL_Surface* surf = SDL_CreateRGBSurface(0, totalW, surfH, 32,
            0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
        if (!surf) return nullptr;
        SDL_FillRect(surf, nullptr, 0); // transparent

        // Second pass: blit glyphs
        int penX = 0;
        const char* p = text;
        uint8_t* pixels = (uint8_t*)surf->pixels;
        int pitch = surf->pitch;

        while (*p) {
            unsigned long ch;
            int bytes;
            decodeUTF8(p, ch, bytes);
            p += bytes;
            if (FT_Load_Char(ftFace, ch, FT_LOAD_RENDER)) continue;

            FT_GlyphSlot g = ftFace->glyph;
            FT_Bitmap& bmp = g->bitmap;
            int drawX = penX + g->bitmap_left;
            int drawY = maxAscender - g->bitmap_top;

            for (unsigned int row = 0; row < bmp.rows; row++) {
                int dy = drawY + (int)row;
                if (dy < 0 || dy >= surfH) continue;
                for (unsigned int col = 0; col < bmp.width; col++) {
                    int dx = drawX + (int)col;
                    if (dx < 0 || dx >= totalW) continue;
                    uint8_t alpha = bmp.buffer[row * bmp.pitch + col];
                    if (alpha > 0) {
                        uint8_t* px = pixels + dy * pitch + dx * 4;
                        px[0] = color.r;
                        px[1] = color.g;
                        px[2] = color.b;
                        px[3] = (uint8_t)((alpha * color.a) / 255);
                    }
                }
            }
            penX += g->advance.x >> 6;
        }
        return surf;
    }

    // Draw text at position using a temporary texture (fast)
    void drawText(SDL_Renderer* renderer, int x, int y, int size,
                  const Color& color, const char* text) {
        SDL_Surface* surf = renderToSurface(size, color, text);
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_Rect dst = {x, y - size, surf->w, surf->h};
            SDL_RenderCopy(renderer, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
        SDL_FreeSurface(surf);
    }

    int textWidth(int size, const char* text) {
        if (!valid || !text || !*text) return 0;
        FT_Set_Pixel_Sizes(ftFace, 0, size);
        int width = 0;
        const char* p = text;
        while (*p) {
            unsigned long ch;
            int bytes;
            decodeUTF8(p, ch, bytes);
            p += bytes;
            if (FT_Load_Char(ftFace, ch, FT_LOAD_RENDER)) continue;
            width += ftFace->glyph->advance.x >> 6;
        }
        return width;
    }

private:
    static void decodeUTF8(const char* p, unsigned long& ch, int& bytes) {
        ch = (unsigned char)*p;
        bytes = 1;
        if (ch < 0x80) return;
        if (ch < 0xC0) return; // continuation
        if (ch < 0xE0 && p[1]) {
            ch = ((ch & 0x1F) << 6) | (p[1] & 0x3F); bytes = 2;
        } else if (ch < 0xF0 && p[1] && p[2]) {
            ch = ((ch & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); bytes = 3;
        } else if (p[1] && p[2] && p[3]) {
            ch = ((ch & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); bytes = 4;
        }
    }
};

// ============================================================================
// Game entry
// ============================================================================
struct GameEntry {
    std::string name;
    std::string path;
    bool hasStartupTjs;
    bool hasXp3;
};

// ============================================================================
// Scan for novel folders
// ============================================================================
static std::vector<GameEntry> scanForGames() {
    std::vector<GameEntry> games;
    const char* basePath = "ux0:data/kirikiroid2";

    DIR* dir = opendir(basePath);
    if (!dir) return games;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        std::string sub = std::string(basePath) + "/" + ent->d_name;
        struct stat st;
        if (stat(sub.c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        DIR* subdir = opendir(sub.c_str());
        if (!subdir) continue;

        GameEntry entry;
        entry.name = ent->d_name;
        entry.path = "ux0:/data/kirikiroid2/" + std::string(ent->d_name) + "/";
        entry.hasStartupTjs = false;
        entry.hasXp3 = false;

        struct dirent* subent;
        while ((subent = readdir(subdir)) != nullptr) {
            if (subent->d_name[0] == '.') continue;
            std::string fname = subent->d_name;
            std::string lower = fname;
            for (auto &c : lower) c = tolower((unsigned char)c);

            if (lower == "startup.tjs") {
                entry.hasStartupTjs = true;
            } else {
                size_t dot = lower.find_last_of('.');
                if (dot != std::string::npos) {
                    std::string ext = lower.substr(dot);
                    if (ext == ".xp3" || ext == ".kxp") entry.hasXp3 = true;
                }
            }
        }
        closedir(subdir);
        if (entry.hasStartupTjs || entry.hasXp3) games.push_back(entry);
    }
    closedir(dir);

    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
        return a.name < b.name;
    });
    return games;
}

// ============================================================================
// Draw helpers
// ============================================================================
static void drawRect(SDL_Renderer* r, int x, int y, int w, int h, const Color& c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

// ============================================================================
// Draw menu frame
// ============================================================================
static void drawMenu(SDL_Renderer* renderer, SDLFontRenderer& font,
                     const std::vector<GameEntry>& games, int selected, int scrollOffset) {
    SDL_SetRenderDrawColor(renderer, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(renderer);

    // Header
    drawRect(renderer, 0, 0, SCREEN_W, 64, COL_HEADER_BG);
    drawRect(renderer, 0, 64, SCREEN_W, 2, COL_ACCENT);
    font.drawText(renderer, 30, 46, 26, COL_TITLE, "KK4V - KiriKiri 4 Vita");
    font.drawText(renderer, SCREEN_W - 150, 46, 16, COL_HINT_TX, "KK4V v0.1");
    font.drawText(renderer, SCREEN_W - 150, 26, 12,
                  g_LogOk ? COL_TAG_TJS : COL_ACCENT,
                  g_LogOk ? "log: ok" : "log: FAILED");
    if (games.empty()) {
        int centerY = SCREEN_H / 2;
        const char* msg1 = "No visual novels found!";
        int w1 = font.textWidth(22, msg1);
        font.drawText(renderer, (SCREEN_W - w1) / 2, centerY, 22, COL_ACCENT, msg1);

        const char* msg2 = "Place game folder into:";
        int w2 = font.textWidth(18, msg2);
        font.drawText(renderer, (SCREEN_W - w2) / 2, centerY + 34, 18, COL_NORMAL_TX, msg2);

        const char* msg3 = "ux0:/data/kirikiroid2/<novel>/";
        int w3 = font.textWidth(16, msg3);
        font.drawText(renderer, (SCREEN_W - w3) / 2, centerY + 62, 16, COL_SELECTED_TX, msg3);
    } else {
        int startY = 80;
        int itemHeight = 56;
        int maxVisible = (SCREEN_H - startY - 50) / itemHeight;

        for (int i = 0; i < maxVisible && (i + scrollOffset) < (int)games.size(); i++) {
            int idx = i + scrollOffset;
            int y = startY + i * itemHeight;
            bool isSel = (idx == selected);

            drawRect(renderer, 20, y, SCREEN_W - 40, itemHeight - 4,
                     isSel ? COL_SELECTED_BG : COL_ITEM_BG);
            if (isSel) drawRect(renderer, 20, y, 4, itemHeight - 4, COL_ACCENT);

            Color tc = isSel ? COL_SELECTED_TX : COL_NORMAL_TX;
            font.drawText(renderer, 40, y + 34, isSel ? 22 : 20, tc, games[idx].name.c_str());

            const char* tag = games[idx].hasStartupTjs ? "[TJS]" : "[XP3]";
            Color tagC = games[idx].hasStartupTjs ? COL_TAG_TJS : COL_TAG_XP3;
            int tw = font.textWidth(14, tag);
            font.drawText(renderer, SCREEN_W - 60 - tw, y + 32, 14, tagC, tag);
        }

        if (scrollOffset > 0)
            font.drawText(renderer, SCREEN_W / 2 - 5, startY - 2, 16, COL_HINT_TX, "^");
        if (scrollOffset + maxVisible < (int)games.size())
            font.drawText(renderer, SCREEN_W / 2 - 5, startY + maxVisible * itemHeight + 8, 16, COL_HINT_TX, "v");
    }

    // Footer
    drawRect(renderer, 0, SCREEN_H - 36, SCREEN_W, 36, COL_HEADER_BG);
    drawRect(renderer, 0, SCREEN_H - 38, SCREEN_W, 2, COL_SEPARATOR);
    if (!games.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d / %d", selected + 1, (int)games.size());
        font.drawText(renderer, 30, SCREEN_H - 10, 14, COL_HINT_TX, buf);
        font.drawText(renderer, SCREEN_W - 350, SCREEN_H - 10, 14, COL_HINT_TX,
                     "D-Pad: Navigate   X: Select   O: Exit");
    } else {
        font.drawText(renderer, SCREEN_W - 150, SCREEN_H - 10, 14, COL_HINT_TX, "O: Exit");
    }

    SDL_RenderPresent(renderer);
}

// ============================================================================
// "FateCrypt" XP3 content decryption
//
// Some Type-Moon (and Type-Moon-licensed) titles ship XP3 archives whose
// file contents are obfuscated with a trivial per-byte XOR (0x36), plus two
// single-byte fixups at fixed absolute offsets within each file. This was
// verified by hand against Fate/stay night [Realta Nua]'s own
// data.xp3>system/Initialize.tjs: XORing with 0x36 (and fixing up the two
// special offsets) decodes it to clean, readable UTF-16LE TJS source.
//
// The real Windows engine gets this via a native "cxdec.tpm" plugin (whose
// loading this Vita port stubs out — see TVPLoadPlugin). This mirrors just
// the byte transform through the engine's own official extraction-filter
// hook (TVPSetXP3ArchiveExtractionFilter), the same hook such plugins use.
// It's enabled only when the game folder ships a cxdec.tpm, so it can't
// affect games that don't need it.
// ============================================================================
static void TVP_tTVPXP3ArchiveExtractionFilter_CONVENTION
FateCryptXP3Filter(tTVPXP3ExtractionFilterInfo *info, tTJSVariant *ctx) {
    tjs_uint8 *buf = (tjs_uint8*)info->Buffer;
    tjs_uint64 base = info->Offset;
    for (tjs_uint i = 0; i < info->BufferSize; i++) {
        tjs_uint64 pos = base + i;
        tjs_uint8 v = buf[i] ^ 0x36;
        if (pos == 0x13) v ^= 1;
        else if (pos == 0x2ea29) v ^= 3;
        buf[i] = v;
    }
}

static void EnableFateCryptIfPresent(const std::string &gamePath) {
    std::string tpmPath = gamePath + "cxdec.tpm";
    SceIoStat st;
    if (sceIoGetstat(tpmPath.c_str(), &st) >= 0) {
        KK4V_Log("[KK4V] Found cxdec.tpm -> enabling FateCrypt XP3 content filter");
        TVPSetXP3ArchiveExtractionFilter(FateCryptXP3Filter);
    }
}

// ============================================================================
// Game selector menu — always shown
// ============================================================================
static std::string showGameMenu(SDL_Window* window, SDL_Renderer* renderer) {
    SDLFontRenderer font;
    if (!font.init("app0:default.ttf")) {
        // Font failed — if there's exactly one game, pick it blindly
        std::vector<GameEntry> games = scanForGames();
        if (games.size() >= 1) return games[0].path;
        return "";
    }

    std::vector<GameEntry> games = scanForGames();

    int selected = 0;
    int scrollOffset = 0;
    int maxVisible = (SCREEN_H - 80 - 50) / 56;

    SceCtrlData pad, oldPad;
    memset(&pad, 0, sizeof(pad));
    memset(&oldPad, 0, sizeof(oldPad));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    std::string result;
    bool running = true;

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~oldPad.buttons;

        if (!games.empty()) {
            if (pressed & SCE_CTRL_UP) {
                if (--selected < 0) selected = (int)games.size() - 1;
            }
            if (pressed & SCE_CTRL_DOWN) {
                if (++selected >= (int)games.size()) selected = 0;
            }
            if (selected < scrollOffset) scrollOffset = selected;
            else if (selected >= scrollOffset + maxVisible) scrollOffset = selected - maxVisible + 1;

            if (pressed & SCE_CTRL_CROSS) {
                result = games[selected].path;
                KK4V_Log("[KK4V] Selected");
                // Immediate visual feedback so it's obvious the press registered,
                // even if file logging or the engine load hangs afterwards.
                SDL_SetRenderDrawColor(renderer, COL_BG.r, COL_BG.g, COL_BG.b, 255);
                SDL_RenderClear(renderer);
                char loadMsg[300];
                snprintf(loadMsg, sizeof(loadMsg), "Loading: %s...", games[selected].name.c_str());
                int lw = font.textWidth(22, loadMsg);
                font.drawText(renderer, (SCREEN_W - lw) / 2, SCREEN_H / 2, 22, COL_TITLE, loadMsg);
                SDL_RenderPresent(renderer);
                running = false;
            }
        }
        if (pressed & SCE_CTRL_CIRCLE) running = false;

        oldPad = pad;
        drawMenu(renderer, font, games, selected, scrollOffset);
        SDL_Delay(16);
    }
    return result;
}

// ============================================================================
// Encoding config
// ============================================================================
extern void TVPSetDefaultReadEncoding(const ttstr& encoding);

static std::string readEncodingConfig(const std::string& gamePath) {
    std::string encoding;
    std::string p1 = gamePath + "encoding.txt";
    size_t pos = p1.find("ux0:/");
    if (pos != std::string::npos) p1.replace(pos, 5, "ux0:");
    FILE* f = fopen(p1.c_str(), "r");
    if (f) { char b[64]={0}; if(fgets(b,63,f)){char*n=strpbrk(b,"\r\n");if(n)*n=0;if(*b)encoding=b;} fclose(f); }
    if (encoding.empty()) {
        f = fopen("ux0:data/kirikiroid2/encoding.txt", "r");
        if (f) { char b[64]={0}; if(fgets(b,63,f)){char*n=strpbrk(b,"\r\n");if(n)*n=0;if(*b)encoding=b;} fclose(f); }
    }
    return encoding;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char *argv[]) {

    InitFileLogging();
    std::set_terminate(OnTerminate);

    // Check game.txt for direct launch bypass
    std::string gamePath;
    std::string forcedEncoding;
    bool directLaunch = false;

    FILE* f = fopen("ux0:data/kirikiroid2/game.txt", "r");
    if (f) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf) - 1, f)) {
            char* nl = strpbrk(buf, "\r\n");
            if (nl) *nl = '\0';
            if (strlen(buf) > 0) {
                gamePath = "ux0:/data/kirikiroid2/" + std::string(buf) + "/";
                directLaunch = true;
            }
        }
        if (fgets(buf, sizeof(buf) - 1, f)) {
            char* nl = strpbrk(buf, "\r\n");
            if (nl) *nl = '\0';
            if (strlen(buf) > 0) forcedEncoding = buf;
        }
        fclose(f);
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        sceKernelExitProcess(0);
        return -1;
    }

    // Create the one and only SDL window+renderer (shared with engine)
    s_SDLWindow = SDL_CreateWindow("KK4V",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (s_SDLWindow) {
        s_SDLRenderer = SDL_CreateRenderer(s_SDLWindow, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    }
    if (!s_SDLWindow || !s_SDLRenderer) {
        SDL_Quit();
        sceKernelExitProcess(0);
        return -1;
    }
    SDL_SetRenderDrawBlendMode(s_SDLRenderer, SDL_BLENDMODE_BLEND);

    // Show menu (always, unless game.txt bypass)
    if (!directLaunch) {
        gamePath = showGameMenu(s_SDLWindow, s_SDLRenderer);
        if (gamePath.empty()) {
            SDL_DestroyRenderer(s_SDLRenderer); s_SDLRenderer = nullptr;
            SDL_DestroyWindow(s_SDLWindow); s_SDLWindow = nullptr;
            SDL_Quit();
            sceKernelExitProcess(0);
            return 0;
        }
    }

    // Clear screen before engine starts
    SDL_SetRenderDrawColor(s_SDLRenderer, 0, 0, 0, 255);
    SDL_RenderClear(s_SDLRenderer);
    SDL_RenderPresent(s_SDLRenderer);

    // Encoding
    if (forcedEncoding.empty()) forcedEncoding = readEncodingConfig(gamePath);

    // Create application
    Application = new tTVPApplication();

    if (!forcedEncoding.empty()) {
        TVPSetDefaultReadEncoding(ttstr(forcedEncoding.c_str()));
    }

    EnableFateCryptIfPresent(gamePath);
    ttstr gamePathW(gamePath.c_str());

    try {
        Application->StartApplication(gamePathW);
    } catch (const std::exception &e) {
        KK4V_Log("[KK4V] StartApplication() threw std::exception");
        KK4V_Log(e.what());
        sceKernelExitProcess(0);
        return -1;
    } catch (...) {
        KK4V_Log("[KK4V] StartApplication() threw unknown exception");
        sceKernelExitProcess(0);
        return -1;
    }
    KK4V_Log("[KK4V] StartApplication() returned");

    SDL_Joystick* joystick = nullptr;
    if (SDL_NumJoysticks() > 0) joystick = SDL_JoystickOpen(0);

    // Native SceCtrl button -> (VK for OnKeyDown/Up, TJS char for OnKeyPress or 0)
    // polling, instead of SDL's joystick abstraction: the SDL button-index
    // mapping for Vita is unverified/inconsistent, while these SCE_CTRL_*
    // bitmasks are the real, documented hardware buttons (same approach the
    // game-selection menu already uses reliably). Every transition is
    // logged so the actual physical button can be confirmed from a device.
    struct ButtonMap { SceCtrlButtons mask; const char *name; tjs_uint16 vk; tjs_char ch; };
    static const ButtonMap kButtonMap[] = {
        { SCE_CTRL_CROSS,    "CROSS",    VK_RETURN,  TJS_W('\r') }, // confirm
        { SCE_CTRL_CIRCLE,   "CIRCLE",   VK_ESCAPE,  (tjs_char)0x1B }, // cancel / back
        { SCE_CTRL_SQUARE,   "SQUARE",   VK_CONTROL, 0 },            // KAG: hold-to-skip
        { SCE_CTRL_TRIANGLE, "TRIANGLE", VK_MENU,    0 },            // system menu
        { SCE_CTRL_UP,       "UP",       VK_UP,      0 },
        { SCE_CTRL_DOWN,     "DOWN",     VK_DOWN,    0 },
        { SCE_CTRL_LEFT,     "LEFT",     VK_LEFT,    0 },
        { SCE_CTRL_RIGHT,    "RIGHT",    VK_RIGHT,   0 },
        { SCE_CTRL_L1,       "L1",       VK_PRIOR,   0 },            // backlog / prev slot
        { SCE_CTRL_R1,       "R1",       VK_NEXT,    0 },            // next slot
        { SCE_CTRL_SELECT,   "SELECT",   VK_TAB,     0 },            // hide text window
        { SCE_CTRL_START,    "START",    VK_ESCAPE,  (tjs_char)0x1B }, // pause / system menu
    };
    SceCtrlData pad, oldPad;
    memset(&pad, 0, sizeof(pad));
    memset(&oldPad, 0, sizeof(oldPad));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    while (!TVPTerminated) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { TVPTerminated = true; break; }
            tTJSNI_Window* win = TVPGetActiveWindow();
            if (win) {
                if (ev.type == SDL_MOUSEBUTTONDOWN)
                    win->OnMouseDown(ev.button.x, ev.button.y, mbLeft, 0);
                else if (ev.type == SDL_MOUSEBUTTONUP) {
                    win->OnMouseUp(ev.button.x, ev.button.y, mbLeft, 0);
                    win->OnClick(ev.button.x, ev.button.y);
                } else if (ev.type == SDL_MOUSEMOTION)
                    win->OnMouseMove(ev.motion.x, ev.motion.y, 0);
            }
        }

        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~oldPad.buttons;
        unsigned int released = oldPad.buttons & ~pad.buttons;
        if (pressed || released) {
            tTJSNI_Window* win = TVPGetActiveWindow();
            for (const auto &bm : kButtonMap) {
                if (pressed & bm.mask) {
                    if (win) {
                        win->OnKeyDown(bm.vk, 0);
                        if (bm.ch) win->OnKeyPress(bm.ch);
                    }
                }
                if (released & bm.mask) {
                    if (win) win->OnKeyUp(bm.vk, 0);
                }
            }
        }
        oldPad = pad;
        try {
            Application->Run();
        } catch (const std::exception &e) {
            KK4V_Log("[KK4V] Application->Run() threw std::exception");
            KK4V_Log(e.what());
            break;
        } catch (...) {
            KK4V_Log("[KK4V] Application->Run() threw unknown exception");
            break;
        }
        SDL_Delay(1);
    }
    if (joystick) SDL_JoystickClose(joystick);

    delete Application;
    SDL_Quit();
    sceKernelExitProcess(0);
    return 0;
}
