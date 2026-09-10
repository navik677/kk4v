#include "tjsCommHead.h"
#include "TVPScreen.h"
#include "Application.h"
#include "StorageIntf.h"
#include "ConfigManager/LocaleConfigManager.h"
#include <SDL2/SDL.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <string>
#include <vector>

// -----------------------------------------
// Screen & Display
// -----------------------------------------
int tTVPScreen::GetWidth() { return 960; }
int tTVPScreen::GetHeight() { return 544; }
int tTVPScreen::GetDesktopLeft() { return 0; }
int tTVPScreen::GetDesktopTop() { return 0; }
int tTVPScreen::GetDesktopWidth() { return 960; }
int tTVPScreen::GetDesktopHeight() { return 544; }

// -----------------------------------------
// Timing & OS
// -----------------------------------------
tjs_uint32 TVPGetRoughTickCount32() {
    return SDL_GetTicks();
}

ttstr TVPGetPlatformName() {
    return ttstr(TJS_W("PSVita"));
}
ttstr TVPGetOSName() {
    return ttstr(TJS_W("VitaOS"));
}

// -----------------------------------------
// File System
// -----------------------------------------
#include <psp2/io/stat.h>
#include <utime.h>

bool TVPCreateFolders(const ttstr& folder) {
    if (folder.IsEmpty()) return true;
    std::string path = folder.AsNarrowStdString();
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path.c_str());
    size_t len = strlen(tmp);
    if (len == 0) return true;
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            if (strchr(tmp, ':') != p - 1) { // Don't try to mkdir "ux0:"
                sceIoMkdir(tmp, 0777);
            }
            *p = '/';
        }
    }
    sceIoMkdir(tmp, 0777);
    return true;
}

void TVP_utime(const char *name, time_t modtime) {
    struct utimbuf utb;
    utb.actime = modtime;
    utb.modtime = modtime;
    utime(name, &utb);
}

// -----------------------------------------
// UI & Dialogs Stubs
// -----------------------------------------
int TVPShowSimpleInputBox(ttstr& result, const ttstr& caption, const ttstr& prompt, const std::vector<ttstr>& history) {
    return 0;
}

std::string TVPShowFileSelector(const std::string &title, const std::string &initfilename, std::string initdir, bool issave) {
    return "";
}

#include "vita_klog.h"

extern "C" int TVPShowSimpleMessageBox(const char * text, const char * caption, unsigned int nButton, const char **btnText) {
    char logMsg[1024];
    snprintf(logMsg, sizeof(logMsg), "[KK4V] MessageBox [%s]: %s", caption ? caption : "Msg", text ? text : "");
    KK4V_Log(logMsg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, caption ? caption : "Msg", text ? text : "", nullptr);
    return 0;
}

// Renamed from av_dirname to avoid colliding with the real libavutil
// av_dirname() now that ffmpeg is linked in (see vita_movie.cpp).
extern "C" char *TVPLocalDirname(char *path) {
    char *p = strrchr(path, '/');
    if (!p) return (char *)".";
    *p = 0;
    return path;
}

// -----------------------------------------
// Logging
// -----------------------------------------
void TVPConsoleLog(const ttstr &mes, bool important) {
    std::string s = mes.AsNarrowStdString();
    char logMsg[600];
    snprintf(logMsg, sizeof(logMsg), "[TVP] %s", s.c_str());
    KK4V_Log(logMsg);
}

namespace TJS {
    void TVPConsoleLog(const tjs_char* msg) {
        std::string s = ttstr(msg).AsNarrowStdString();
        char logMsg[600];
        snprintf(logMsg, sizeof(logMsg), "[TVP] %s", s.c_str());
        KK4V_Log(logMsg);
    }
    void TVPConsoleLog(const char* msg, ...) {
        // Stub for vararg TVPConsoleLog
    }
}

// -----------------------------------------
// LocaleConfigManager
// -----------------------------------------
// We provide a stub since we removed ConfigManager from compilation
LocaleConfigManager* LocaleConfigManager::GetInstance() {
    static LocaleConfigManager instance;
    return &instance;
}

LocaleConfigManager::LocaleConfigManager() {
}

const std::string& LocaleConfigManager::GetText(const std::string &tid) {
    static std::string fallback = tid;
    fallback = tid;
    return fallback;
}

#include "Platform.h"
#include "ConfigManager/IndividualConfigManager.h"
#include "StorageIntf.h" // For tTVPArchive

typedef struct _GUID {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[ 8 ];
} GUID;
typedef GUID IID;

extern "C" const IID IID_IUnknown = {0,0,0,{0, 0xc0, 0, 0, 0, 0x46}};
extern "C" const IID IID_ISequentialStream = {0x0c733a30,0x2a1c,0x11ce,{0xad,0xe5,0x00,0xaa,0x00,0x44,0x77,0x3d}};
extern "C" const IID IID_IStream = {0x0000000c,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

void TVPLoadInternalPlugin(const ttstr& name) {}
void TVPLoadInternalPlugins() {}
std::vector<ttstr> TVPRegisteredPlugins;

static bool vita_stat_single(const char *path, tTVP_stat &s) {
    struct stat st;
    if (stat(path, &st) == 0) {
        s.st_mode = (uint16_t)st.st_mode;
        s.st_size = (uint64_t)st.st_size;
        s.st_atime = (uint64_t)st.st_atime;
        s.st_mtime = (uint64_t)st.st_mtime;
        s.st_ctime = (uint64_t)st.st_ctime;
        return true;
    }

    SceIoStat sce_st;
    if (sceIoGetstat(path, &sce_st) >= 0) {
        uint16_t mode = 0;
        if (SCE_S_ISDIR(sce_st.st_mode)) {
            mode |= S_IFDIR;
        } else if (SCE_S_ISREG(sce_st.st_mode)) {
            mode |= S_IFREG;
        }
        mode |= (sce_st.st_mode & 0777);
        s.st_mode = mode;
        s.st_size = (uint64_t)sce_st.st_size;
        s.st_atime = 0;
        s.st_mtime = 0;
        s.st_ctime = 0;
        return true;
    }
    return false;
}

bool TVP_stat(const char *name, tTVP_stat &s) {
    if (!name || !*name) return false;

    // Handle file://./, file://, and ./ prefixes
    if (strncmp(name, "file://./", 9) == 0) name += 9;
    else if (strncmp(name, "file://", 7) == 0) name += 7;
    else if (strncmp(name, "./", 2) == 0) name += 2;

    std::string cleanName = name;
    // Strip trailing archive delimiter '>' or slashes if present (e.g. for directory or archive checks)
    while (cleanName.length() > 1 && 
           (cleanName.back() == '>' || cleanName.back() == '/' || cleanName.back() == '\\')) {
        // Keep root device slash, e.g., don't strip '/' from "ux0:/" or "/"
        if (cleanName.back() == '/' && cleanName.length() >= 2 && cleanName[cleanName.length() - 2] == ':') {
            break;
        }
        cleanName.pop_back();
    }

    if (vita_stat_single(cleanName.c_str(), s)) {
        return true;
    }

    // Try without slash after colon (e.g., "ux0:file" vs "ux0:/file")
    const char* colon = strchr(cleanName.c_str(), ':');
    if (colon && colon[1] == '/') {
        std::string alt(cleanName.c_str(), colon - cleanName.c_str() + 1);
        alt += (colon + 2);
        if (vita_stat_single(alt.c_str(), s)) {
            return true;
        }
        return false;
    }
    return false;
}

bool TVP_stat(const tjs_char *name, tTVP_stat &s) {
    ttstr str(name);
    std::string narrow = str.AsNarrowStdString();
    return TVP_stat(narrow.c_str(), s);
}

std::string TVPGetApplicationHomeDirectory() {
    return "ux0:/data/kirikiroid2/";
}

bool TVPWriteDataToFile(const ttstr &filepath, const void *data, unsigned int len) {
    std::string path = filepath.AsNarrowStdString();
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    return written == len;
}

tTVPArchive* TVPOpenZIPArchive(const ttstr & name, tTJSBinaryStream *st, bool normalizeFileName) { return nullptr; }
tTVPArchive* TVPOpen7ZArchive(const ttstr & name, tTJSBinaryStream *st, bool normalizeFileName) { return nullptr; }
tTVPArchive* TVPOpenTARArchive(const ttstr & name, tTJSBinaryStream *st, bool normalizeFileName) { return nullptr; }

void TVPGetMemoryInfo(TVPMemoryInfo &m) {
    m.MemTotal = 512 * 1024;
    m.MemFree = 256 * 1024;
    m.SwapTotal = 0;
    m.SwapFree = 0;
    m.VirtualTotal = 512 * 1024;
    m.VirtualUsed = 256 * 1024;
}

IndividualConfigManager* IndividualConfigManager::GetInstance() {
    static IndividualConfigManager instance;
    return &instance;
}
std::string IndividualConfigManager::GetFilePath() { return ""; }
template<> std::string IndividualConfigManager::GetValue<std::string>(const std::string &name, const std::string& defVal) { return defVal; }
template<> int IndividualConfigManager::GetValue<int>(const std::string &name, const int& defVal) { return defVal; }
template<> bool IndividualConfigManager::GetValue<bool>(const std::string &name, const bool& defVal) { return defVal; }
template<> float IndividualConfigManager::GetValue<float>(const std::string &name, const float& defVal) { return defVal; }

void TVPDetectCPU() {}
extern "C" void TVPGL_ASM_Init() {}
void TVPExitApplication(int code) {
    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "[KK4V] TVPExitApplication(%d) called", code);
    KK4V_Log(logMsg);
    SDL_Quit();
    exit(code);
}

bool TVPGetJoyPadAsyncState(unsigned int key, bool getcurrent) { return false; }
bool TVPGetKeyMouseAsyncState(unsigned int key, bool getcurrent) { return false; }

int TVPShowSimpleMessageBox(const ttstr & text, const ttstr & caption, const std::vector<ttstr> & buttons) {
    std::string cap = caption.AsNarrowStdString();
    std::string msg = text.AsNarrowStdString();
    char logMsg[1024];
    snprintf(logMsg, sizeof(logMsg), "[KK4V] MessageBox [%s]: %s", cap.c_str(), msg.c_str());
    KK4V_Log(logMsg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, cap.c_str(), msg.c_str(), nullptr);
    return 0;
}

class tTJSNI_MenuItem;
int TVPShowPopMenu(tTJSNI_MenuItem* menu) { return 0; }

class tTJSNI_VideoOverlay;
class IStream;
class iTVPVideoOverlay;
// GetVideoOverlayObject() is implemented in vita_movie.cpp (ffmpeg-based).
void GetVideoLayerObject(tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname, const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out) {}
void GetMixingVideoOverlayObject(tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname, const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out) {}
void GetMFVideoOverlayObject(tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname, const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out) {}
namespace cocos2d { class Node; }
#include "WindowIntf.h"
#include "environ/win32/TVPWindow.h"
#include "RenderManager.h"

SDL_Window* s_SDLWindow = nullptr;
SDL_Renderer* s_SDLRenderer = nullptr;
static SDL_Texture* s_SDLTexture = nullptr;
static int s_TexWidth = 0;
static int s_TexHeight = 0;

class CVitaWindowLayer : public iWindowLayer {
public:
    tTJSNI_Window* m_Window;
    int m_Width;
    int m_Height;
    bool m_Visible;
    std::string m_Caption;

    CVitaWindowLayer(tTJSNI_Window* w) : m_Window(w), m_Width(960), m_Height(544), m_Visible(true) {}
    virtual ~CVitaWindowLayer() {}

    virtual void SetPaintBoxSize(tjs_int w, tjs_int h) override {
        m_Width = w;
        m_Height = h;
    }
    virtual bool GetFormEnabled() override { return true; }
    virtual void SetDefaultMouseCursor() override {}
    virtual void GetCursorPos(tjs_int &x, tjs_int &y) override { x = 0; y = 0; }
    virtual void SetCursorPos(tjs_int x, tjs_int y) override {}
    virtual void SetHintText(const ttstr &text) override {}
    virtual void SetAttentionPoint(tjs_int left, tjs_int top, const struct tTVPFont * font) override {}
    virtual void ZoomRectangle(tjs_int & left, tjs_int & top, tjs_int & right, tjs_int & bottom) override {}
    virtual void BringToFront() override {}
    virtual void ShowWindowAsModal() override {}
    virtual bool GetVisible() override { return m_Visible; }
    virtual void SetVisible(bool bVisible) override { m_Visible = bVisible; }
    virtual const char *GetCaption() override { return m_Caption.c_str(); }
    virtual void SetCaption(const std::string &cap) override { m_Caption = cap; }
    virtual void SetWidth(tjs_int w) override { m_Width = w; }
    virtual void SetHeight(tjs_int h) override { m_Height = h; }
    virtual void SetSize(tjs_int w, tjs_int h) override { m_Width = w; m_Height = h; }
    virtual void GetSize(tjs_int &w, tjs_int &h) override { w = m_Width; h = m_Height; }
    virtual tjs_int GetWidth() const override { return m_Width; }
    virtual tjs_int GetHeight() const override { return m_Height; }
    virtual void GetWinSize(tjs_int &w, tjs_int &h) override { w = m_Width; h = m_Height; }
    virtual void SetZoom(tjs_int numer, tjs_int denom) override {}

    virtual void UpdateDrawBuffer(iTVPTexture2D *tex) override {
        if (!tex || !s_SDLRenderer) return;
        tjs_uint w = tex->GetWidth();
        tjs_uint h = tex->GetHeight();
        if (w == 0 || h == 0) return;

        if (!s_SDLTexture || s_TexWidth != (int)w || s_TexHeight != (int)h) {
            if (s_SDLTexture) SDL_DestroyTexture(s_SDLTexture);
            s_SDLTexture = SDL_CreateTexture(s_SDLRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            // The engine's composited window buffer carries real RGB but an
            // alpha channel that is often 0 (it's meaningful only for the
            // engine's own internal layer compositing, not for display).
            // Force this texture to ignore alpha entirely so the Vita's SDL2
            // renderer backend can't treat those pixels as transparent.
            if (s_SDLTexture) SDL_SetTextureBlendMode(s_SDLTexture, SDL_BLENDMODE_NONE);
            s_TexWidth = (int)w;
            s_TexHeight = (int)h;
        }

        const void* pixels = tex->GetPixelData();
        if (pixels && s_SDLTexture) {
            SDL_UpdateTexture(s_SDLTexture, nullptr, pixels, tex->GetPitch());
            SDL_RenderClear(s_SDLRenderer);
            SDL_RenderCopy(s_SDLRenderer, s_SDLTexture, nullptr, nullptr);
            SDL_RenderPresent(s_SDLRenderer);
        }
    }

    virtual void InvalidateClose() override {}
    virtual bool GetWindowActive() override { return true; }
    virtual void Close() override {}
    virtual void OnCloseQueryCalled(bool b) override {}
    virtual void InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) override {}
    virtual void OnKeyUp(tjs_uint16 vk, int shift) override {}
    virtual void OnKeyPress(tjs_uint16 vk, int repeat, bool prevkeystate, bool convertkey) override {}
    virtual tTVPImeMode GetDefaultImeMode() const override { return imDisable; }
    virtual void SetImeMode(tTVPImeMode mode) override {}
    virtual void ResetImeMode() override {}
    virtual void UpdateWindow(tTVPUpdateType type) override {
        if (s_SDLRenderer) SDL_RenderPresent(s_SDLRenderer);
    }
    virtual void SetVisibleFromScript(bool b) override { m_Visible = b; }
    virtual void SetUseMouseKey(bool b) override {}
    virtual bool GetUseMouseKey() const override { return false; }
    virtual void ResetMouseVelocity() override {}
    virtual void ResetTouchVelocity(tjs_int id) override {}
    virtual bool GetMouseVelocity(float& x, float& y, float& speed) const override { return false; }
    virtual void TickBeat() override {}
    virtual cocos2d::Node *GetPrimaryArea() override { return nullptr; }
};

static CVitaWindowLayer* s_CurrentWindowLayer = nullptr;

iWindowLayer *TVPCreateAndAddWindow(tTJSNI_Window *w) {
    if (!s_SDLWindow) {
        s_SDLWindow = SDL_CreateWindow("KK4V", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 544, SDL_WINDOW_SHOWN);
        s_SDLRenderer = SDL_CreateRenderer(s_SDLWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    }
    s_CurrentWindowLayer = new CVitaWindowLayer(w);
    return s_CurrentWindowLayer;
}

void TVPRemoveWindowLayer(iWindowLayer *lay) {
    if (lay == s_CurrentWindowLayer) s_CurrentWindowLayer = nullptr;
    delete static_cast<CVitaWindowLayer*>(lay);
}

tTJSNI_Window* TVPGetActiveWindow() {
    return s_CurrentWindowLayer ? s_CurrentWindowLayer->m_Window : nullptr;
}

std::string TVPGetPackageVersionString() { return "1.0.0"; }
void TVPOpenPatchLibUrl() {}
void TVPCauseAtInstallExtensionClass(TJS::iTJSDispatch2*) {}

class iTVPBaseBitmap;
bool TVPAcceptSaveAsJXR(void* formatdata, const ttstr & type, class iTJSDispatch2** dic ) { return false; }
void TVPSaveAsJXR(void* formatdata, tTJSBinaryStream* dst, const iTVPBaseBitmap* image, const ttstr & mode, iTJSDispatch2* meta) {}

void TVPRelinquishCPU() {
    SDL_Delay(1);
}

#include "FFWaveDecoder.h"
tTVPWaveDecoder * FFWaveDecoderCreator::Create(const ttstr & storagename, const ttstr & extension) { return nullptr; }

#include "GraphicsLoaderIntf.h"
void TVPLoadBPG(void* formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback metainfopushcallback,
	tTJSBinaryStream *src, tjs_int keyidx, tTVPGraphicLoadMode mode) {}
void TVPLoadHeaderBPG(void* formatdata, tTJSBinaryStream *src, iTJSDispatch2** dic) {}

void TVPLoadJXR(void* formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback metainfopushcallback,
	tTJSBinaryStream *src, tjs_int keyidx,  tTVPGraphicLoadMode mode) {}
void TVPLoadHeaderJXR(void* formatdata, tTJSBinaryStream *src, iTJSDispatch2** dic) {}

void TVPLoadPVRv3(void* formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback metainfopushcallback,
	tTJSBinaryStream *src, tjs_int keyidx, tTVPGraphicLoadMode mode) {}
void TVPLoadHeaderPVRv3(void* formatdata, tTJSBinaryStream *src, iTJSDispatch2** dic) {}

class iTVPTexture2D;
iTVPTexture2D* TVPLoadPVRv3(tTJSBinaryStream *s, const std::function<void(const ttstr&, const tTJSVariant&)> &cb) { return nullptr; }

