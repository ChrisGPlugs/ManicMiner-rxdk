// platform_rxdk.cpp - RXDK-SDL3 / original Xbox implementation of platform.h
//
// Video: SDL creates the full-screen window and owns the console's Direct3D 8
// device (published as a window property). Each frame the 320x240 Spectrum
// picture (screen plus border) is copied into a linear texture and drawn as
// a 4:3 quad. Where the picture is a whole multiple of 240 lines (480 = 2x,
// 720p = 3x) it is drawn with point sampling, so the pixels stay sharp;
// otherwise it is filtered. This replaces the nxdk port's direct writes to
// the framebuffer.
//
// If the device is not there (it always is on the console), SDL's renderer
// is used instead.
//
// SPDX-License-Identifier: MIT

#include <xtl.h>

#include "platform.h"

#include <stdarg.h>
#include <string.h>

namespace {

SDL_Window*   g_window = NULL;
char          g_modeName[96] = "none";
int           g_width = 0, g_height = 0;
int           g_srcW = 0, g_srcH = 0;
SDL_FRect     g_picture = { 0, 0, 0, 0 };

D3DDevice*    g_device = NULL;
D3DTexture*   g_textures[2] = { NULL, NULL };
int           g_next = 0;

SDL_Renderer* g_renderer = NULL;
SDL_Texture*  g_sdlTexture = NULL;

struct QuadVertex
{
    float x, y, z, rhw;
    float u, v;
};

const SDL_DisplayMode* findMode(SDL_DisplayMode** modes, int count, int w, int h)
{
    for (int i = 0; i < count; i++)
        if (modes[i]->w == w && modes[i]->h == h)
            return modes[i];
    return NULL;
}

// 720p first (the picture is exactly 3x there), then standard definition
// (480 = 2x; 576 on 50 Hz PAL), then 1080i. The console only lists the
// modes its AV cable and dashboard settings allow.
bool pickMode(SDL_DisplayMode* out)
{
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count);
    if (!modes) return false;
    static const int order[][2] = { { 1280, 720 }, { 640, 480 }, { 640, 576 }, { 1920, 1080 } };
    const SDL_DisplayMode* m = NULL;
    for (unsigned i = 0; !m && i < SDL_arraysize(order); i++)
        m = findMode(modes, count, order[i][0], order[i][1]);
    if (m) *out = *m;
    SDL_free(modes);
    return m != NULL;
}

// 4:3 picture on this mode. HD modes are 16:9, and so is SD when the
// dashboard is set to widescreen; plain SD is a 4:3 screen whatever its
// pixel count, so the picture fills it.
void computePicture()
{
    const bool wide = g_height >= 720 || (XGetVideoFlags() & XC_VIDEO_FLAGS_WIDESCREEN);
    float w = (float)g_width;
    if (wide) w = (float)(g_width * 3 / 4);
    g_picture.w = w;
    g_picture.h = (float)g_height;
    g_picture.x = (float)(int)((g_width - w) / 2);
    g_picture.y = 0;
}

bool integerScale()
{
    return g_srcH && g_height % g_srcH == 0 && (int)g_picture.w == g_srcW * (g_height / g_srcH);
}

bool initD3D()
{
    g_device = (D3DDevice*)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                                                  SDL_PROP_WINDOW_XBOXOG_D3DDEVICE_POINTER, NULL);
    if (!g_device) return false;

    for (int i = 0; i < 2; i++)
    {
        if (FAILED(g_device->CreateTexture(g_srcW, g_srcH, 1, 0, D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT,
                                           &g_textures[i])))
        {
            plat_log("MM: CreateTexture failed, using SDL's renderer\n");
            for (int j = 0; j < i; j++) { g_textures[j]->Release(); g_textures[j] = NULL; }
            g_device = NULL;
            return false;
        }
    }

    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    const DWORD filter = integerScale() ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    g_device->SetTextureStageState(0, D3DTSS_MAGFILTER, filter);
    g_device->SetTextureStageState(0, D3DTSS_MINFILTER, filter);
    g_device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
    return true;
}

bool initRenderer()
{
    g_renderer = SDL_CreateRenderer(g_window, NULL);
    if (!g_renderer) return false;
    SDL_SetRenderVSync(g_renderer, 1);
    g_sdlTexture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     g_srcW, g_srcH);
    if (!g_sdlTexture) return false;
    SDL_SetTextureScaleMode(g_sdlTexture, integerScale() ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    return true;
}

void presentD3D(const uint32_t* pixels)
{
    // Two textures in turn: the one drawn last frame may still be in use by
    // the GPU, and locking it would wait.
    D3DTexture* tex = g_textures[g_next];
    g_next ^= 1;

    D3DLOCKED_RECT locked;
    if (SUCCEEDED(tex->LockRect(0, &locked, NULL, 0)))
    {
        Uint8* dst = (Uint8*)locked.pBits;
        for (int y = 0; y < g_srcH; y++)
            memcpy(dst + (size_t)y * locked.Pitch, pixels + (size_t)y * g_srcW, (size_t)g_srcW * 4);
        tex->UnlockRect(0);
    }

    // Linear textures are addressed in texels, not 0..1.
    const float x0 = g_picture.x - 0.5f, y0 = g_picture.y - 0.5f;
    const float x1 = x0 + g_picture.w,   y1 = y0 + g_picture.h;
    const float u1 = (float)g_srcW, v1 = (float)g_srcH;
    const QuadVertex quad[4] = {
        { x0, y0, 0.0f, 1.0f, 0.0f, 0.0f },
        { x1, y0, 0.0f, 1.0f, u1,   0.0f },
        { x0, y1, 0.0f, 1.0f, 0.0f, v1   },
        { x1, y1, 0.0f, 1.0f, u1,   v1   },
    };

    g_device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    g_device->BeginScene();
    g_device->SetTexture(0, tex);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
    g_device->SetTexture(0, NULL);
    g_device->EndScene();
    g_device->Present(NULL, NULL, NULL, NULL);
}

void presentRenderer(const uint32_t* pixels)
{
    SDL_UpdateTexture(g_sdlTexture, NULL, pixels, g_srcW * 4);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_sdlTexture, NULL, &g_picture);
    SDL_RenderPresent(g_renderer);
}

} // namespace

extern "C" {

int plat_video_init(int src_w, int src_h)
{
    g_srcW = src_w;
    g_srcH = src_h;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        plat_log("MM: SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_DisplayMode mode;
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!pickMode(&mode))
    {
        if (!desktop) return 0;
        mode = *desktop;
    }

    g_window = SDL_CreateWindow("Manic Miner", mode.w, mode.h, SDL_WINDOW_FULLSCREEN);
    if (!g_window)
    {
        plat_log("MM: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!SDL_SetWindowFullscreenMode(g_window, &mode))
        plat_log("MM: SDL_SetWindowFullscreenMode %dx%d failed: %s\n", mode.w, mode.h, SDL_GetError());
    SDL_SyncWindow(g_window);
    SDL_HideCursor();

    const SDL_DisplayMode* current = SDL_GetWindowFullscreenMode(g_window);
    if (!current) current = desktop;
    g_width = current ? current->w : mode.w;
    g_height = current ? current->h : mode.h;
    computePicture();

    const bool d3d = initD3D();
    if (!d3d && !initRenderer())
    {
        plat_log("MM: no way to present: %s\n", SDL_GetError());
        return 0;
    }
    SDL_snprintf(g_modeName, sizeof(g_modeName), "%dx%d (picture %dx%d, %s, %s)", g_width, g_height,
                 (int)g_picture.w, (int)g_picture.h, integerScale() ? "sharp" : "filtered",
                 d3d ? "Direct3D" : "SDL renderer");
    plat_log("MM: video %s\n", g_modeName);
    return 1;
}

void plat_present(const uint32_t* pixels)
{
    if (g_device) presentD3D(pixels);
    else if (g_renderer) presentRenderer(pixels);
}

const char* plat_video_mode_name(void)
{
    return g_modeName;
}

uint64_t plat_now_us(void)
{
    return SDL_GetTicksNS() / 1000;
}

void plat_log(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    OutputDebugStringA(line);
}

const char* plat_data_dir(void)
{
    return "D:\\";
}

const char* plat_save_dir(void)
{
    // U:\ is this title's save folder, E:\UDATA\4D4D0058\ (from the XBE's
    // title ID): the same folder the nxdk build wrote to by its full path.
    static const char* dir = NULL;
    if (!dir)
    {
        const char* d = SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES);
        dir = d ? d : "U:\\";
    }
    return dir;
}

int plat_make_dir(const char* path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int plat_delete_file(const char* path)
{
    return DeleteFileA(path) ? 1 : 0;
}

int plat_rename_file(const char* from, const char* to)
{
    return MoveFileA(from, to) ? 1 : 0;
}

void plat_commit_file(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
}

void plat_list_dir(const char* dir, void (*fn)(const char* path, void* user), void* user)
{
    char pattern[MAX_PATH];
    SDL_snprintf(pattern, sizeof(pattern), "%s*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        SDL_snprintf(path, sizeof(path), "%s%s", dir, fd.cFileName);
        fn(path, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void plat_check_igr(SDL_Gamepad* pad)
{
    if (!pad) return;
    // Triggers are 0..32767 in SDL3: held past half travel.
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) < 16384 ||
        SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) < 16384)
        return;
    // Start or Black, so it works on both the Duke and the Controller S.
    // RXDK-SDL3 maps Black to the right shoulder button.
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
        (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START) ||
         SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)))
    {
        // High scores are flushed when written. A full reboot
        // (HalRebootRoutine = 1), not the quick reboot (2).
        plat_log("MM: IGR\n");
        HalReturnToFirmware(HalRebootRoutine);
    }
}

void plat_exit(void)
{
    // SDL_Quit stops DirectSound and releases the Direct3D device; then
    // back to the dashboard.
    plat_log("MM: exit\n");
    for (int i = 0; i < 2; i++)
        if (g_textures[i]) { g_textures[i]->Release(); g_textures[i] = NULL; }
    g_device = NULL;
    SDL_Quit();
    XLaunchNewImage(NULL, NULL);
    for (;;) Sleep(1000);
}

} // extern "C"
