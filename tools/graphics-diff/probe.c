/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Application-side graphics probe for the milestone 13 DLLs.
 * The same calls run on Windows and VerOS. Tokens are the status or the
 * pixels the caller can read back. No adapter identity and no D3DKMT.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define SURFACE_EDGE 64
#define PIXELS (SURFACE_EDGE * SURFACE_EDGE * 4)

/* Public D3D11 and DXGI numbers. The probe does not include those headers. */
#define DRIVER_HARDWARE 1
#define D3D11_SDK 7
#define FEATURE_11_0 0xb000
#define FORMAT_RGBA8 28
#define USAGE_DEFAULT 0
#define USAGE_STAGING 3
#define BIND_RENDER_TARGET 0x20
#define CPU_READ 0x20000
#define MAP_READ 1

#define GL_TRIANGLES 0x0004
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

typedef struct
{
    unsigned int Width;
    unsigned int Height;
    unsigned int MipLevels;
    unsigned int ArraySize;
    unsigned int Format;
    unsigned int SampleCount;
    unsigned int SampleQuality;
    unsigned int Usage;
    unsigned int BindFlags;
    unsigned int CpuAccess;
    unsigned int MiscFlags;
} TEXDESC;

typedef struct
{
    void *Data;
    unsigned int RowPitch;
    unsigned int DepthPitch;
} MAPPED;

typedef struct
{
    HDC Dc;
    HBITMAP Dib;
    HGDIOBJ Old;
    void *Bits;
} SURFACE;

typedef HRESULT (__stdcall *PFN_D3D11_CREATE)(void *Adapter, int Driver, HMODULE Software,
    unsigned int Flags, const int *Levels, unsigned int LevelCount, unsigned int Sdk,
    void **Device, int *Level, void **Context);
typedef HRESULT (__stdcall *PFN_CREATE_TEX)(void *This, const TEXDESC *Desc, const void *Init,
    void **Texture);
typedef HRESULT (__stdcall *PFN_CREATE_RTV)(void *This, void *Resource, const void *Desc,
    void **View);
typedef void (__stdcall *PFN_CLEAR_RTV)(void *This, void *View, const float *Color);
typedef void (__stdcall *PFN_COPY)(void *This, void *Dst, void *Src);
typedef HRESULT (__stdcall *PFN_MAP)(void *This, void *Resource, unsigned int Sub,
    unsigned int Kind, unsigned int Flags, MAPPED *Mapped);
typedef void (__stdcall *PFN_UNMAP)(void *This, void *Resource, unsigned int Sub);
typedef ULONG (__stdcall *PFN_RELEASE)(void *This);
typedef BOOL (__stdcall *PFN_ALPHABLEND)(HDC, int, int, int, int, HDC, int, int, int, int,
    BLENDFUNCTION);
typedef void (__stdcall *PFN_GLCLEARCOLOR)(float, float, float, float);
typedef void (__stdcall *PFN_GLCLEAR)(unsigned int);
typedef void (__stdcall *PFN_GLVIEWPORT)(int, int, int, int);
typedef void (__stdcall *PFN_GLMATRIX)(unsigned int);
typedef void (__stdcall *PFN_GLLOAD)(void);
typedef void (__stdcall *PFN_GLORTHO)(double, double, double, double, double, double);
typedef void (__stdcall *PFN_GLCOLOR)(float, float, float);
typedef void (__stdcall *PFN_GLBEGIN)(unsigned int);
typedef void (__stdcall *PFN_GLEND)(void);
typedef void (__stdcall *PFN_GLVERTEX)(float, float);
typedef void (__stdcall *PFN_GLFLUSH)(void);
typedef void (__stdcall *PFN_GLREAD)(int, int, int, int, unsigned int, unsigned int, void *);
typedef HGLRC (__stdcall *PFN_WGLCREATE)(HDC);
typedef BOOL (__stdcall *PFN_WGLMAKE)(HDC, HGLRC);
typedef BOOL (__stdcall *PFN_WGLDELETE)(HGLRC);

static HANDLE g_com = INVALID_HANDLE_VALUE;
static HANDLE g_com2 = INVALID_HANDLE_VALUE;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static jmp_buf g_jump;
static volatile int g_guard;
static void *g_guard_handle;
static char g_token[48];

static void
emit(const char *name, const char *status)
{
    char line[160];
    int n;
    DWORD wrote;

    n = _snprintf(line, sizeof(line), "DIFF %s %s\r\n", name, status);
    if (n < 0)
    {
        return;
    }
    if (g_com != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_com, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_com);
    }
    if (g_com2 != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_com2, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_com2);
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_file, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_file);
    }
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    OutputDebugStringA(line);
}

static LONG WINAPI
on_fault(EXCEPTION_POINTERS *info)
{
    (void)info;
    if (g_guard)
    {
        longjmp(g_jump, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int
guard(void (*fn)(void))
{
    if (setjmp(g_jump) != 0)
    {
        g_guard = 0;
        return 0;
    }
    g_guard = 1;
    fn();
    g_guard = 0;
    return 1;
}

static void
set_fail(void)
{
    _snprintf(g_token, sizeof(g_token), "FAIL");
}

static void
set_missing(void)
{
    _snprintf(g_token, sizeof(g_token), "MISSING");
}

static void
set_status(unsigned long value)
{
    _snprintf(g_token, sizeof(g_token), "0x%08lX", value);
}

static unsigned long
fnv1a(const unsigned char *data, size_t length)
{
    unsigned long hash = 2166136261ul;
    size_t i;

    for (i = 0; i < length; i++)
    {
        hash ^= data[i];
        hash *= 16777619ul;
    }
    return hash;
}

static void
set_pixels(const void *bits, size_t length)
{
    set_status(fnv1a((const unsigned char *)bits, length));
}

static void
run_case(const char *name, void (*fn)(void))
{
    set_fail();
    emit(name, "STARTED");
    if (!guard(fn))
    {
        emit(name, "CRASH");
        return;
    }
    emit(name, g_token);
}

static int
open_surface(SURFACE *surface, int width, int height)
{
    BITMAPINFO info;

    memset(surface, 0, sizeof(*surface));
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    surface->Dc = CreateCompatibleDC(NULL);
    if (surface->Dc == NULL)
    {
        return 0;
    }
    surface->Dib = CreateDIBSection(surface->Dc, &info, DIB_RGB_COLORS, &surface->Bits, NULL, 0);
    if (surface->Dib == NULL || surface->Bits == NULL)
    {
        DeleteDC(surface->Dc);
        surface->Dc = NULL;
        return 0;
    }
    surface->Old = SelectObject(surface->Dc, surface->Dib);
    memset(surface->Bits, 0, (size_t)width * (size_t)height * 4u);
    return 1;
}

static void
close_surface(SURFACE *surface)
{
    if (surface->Dc != NULL && surface->Old != NULL)
    {
        SelectObject(surface->Dc, surface->Old);
    }
    if (surface->Dib != NULL)
    {
        DeleteObject(surface->Dib);
    }
    if (surface->Dc != NULL)
    {
        DeleteDC(surface->Dc);
    }
    memset(surface, 0, sizeof(*surface));
}

static void
fill_rect(HDC dc, int width, int height, COLORREF color)
{
    HBRUSH brush;
    RECT rect;

    brush = CreateSolidBrush(color);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void
case_module(const wchar_t *path)
{
    HMODULE module;

    module = LoadLibraryW(path);
    if (module == NULL)
    {
        set_missing();
        return;
    }
    FreeLibrary(module);
    set_status(0);
}

static void
case_user32(void)
{
    case_module(L"user32.dll");
}

static void
case_gdi32(void)
{
    case_module(L"gdi32.dll");
}

static void
case_dxgi(void)
{
    case_module(L"dxgi.dll");
}

static void
case_d3d11(void)
{
    case_module(L"d3d11.dll");
}

static void
case_opengl(void)
{
    case_module(L"opengl32.dll");
}

static void
case_bits(void)
{
    HDC screen;
    int bits;

    screen = GetDC(NULL);
    if (screen == NULL)
    {
        set_fail();
        return;
    }
    bits = GetDeviceCaps(screen, BITSPIXEL);
    ReleaseDC(NULL, screen);
    set_status((unsigned long)bits);
}

static void
case_memory_dc(void)
{
    HDC dc;

    dc = CreateCompatibleDC(NULL);
    if (dc == NULL)
    {
        set_fail();
        return;
    }
    DeleteDC(dc);
    set_status(0);
}

static void
case_dib(void)
{
    SURFACE surface;

    if (!open_surface(&surface, SURFACE_EDGE, SURFACE_EDGE))
    {
        set_fail();
        return;
    }
    close_surface(&surface);
    set_status(0);
}

static void
case_fill(void)
{
    SURFACE surface;

    if (!open_surface(&surface, SURFACE_EDGE, SURFACE_EDGE))
    {
        set_fail();
        return;
    }
    fill_rect(surface.Dc, SURFACE_EDGE, SURFACE_EDGE, RGB(255, 0, 0));
    GdiFlush();
    set_pixels(surface.Bits, PIXELS);
    close_surface(&surface);
}

static void
case_rect(void)
{
    SURFACE surface;
    HBRUSH brush;
    HPEN pen;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;

    if (!open_surface(&surface, SURFACE_EDGE, SURFACE_EDGE))
    {
        set_fail();
        return;
    }
    brush = CreateSolidBrush(RGB(255, 0, 0));
    pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
    old_brush = SelectObject(surface.Dc, brush);
    old_pen = SelectObject(surface.Dc, pen);
    Rectangle(surface.Dc, 4, 4, 40, 30);
    SelectObject(surface.Dc, old_brush);
    SelectObject(surface.Dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    GdiFlush();
    set_pixels(surface.Bits, PIXELS);
    close_surface(&surface);
}

static void
case_line(void)
{
    SURFACE surface;
    HPEN pen;
    HGDIOBJ old;

    if (!open_surface(&surface, SURFACE_EDGE, SURFACE_EDGE))
    {
        set_fail();
        return;
    }
    pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
    old = SelectObject(surface.Dc, pen);
    MoveToEx(surface.Dc, 0, 0, NULL);
    LineTo(surface.Dc, SURFACE_EDGE - 1, SURFACE_EDGE - 1);
    SelectObject(surface.Dc, old);
    DeleteObject(pen);
    GdiFlush();
    set_pixels(surface.Bits, PIXELS);
    close_surface(&surface);
}

static void
case_blit(void)
{
    SURFACE source;
    SURFACE dest;

    memset(&source, 0, sizeof(source));
    memset(&dest, 0, sizeof(dest));
    if (!open_surface(&source, SURFACE_EDGE, SURFACE_EDGE) ||
        !open_surface(&dest, SURFACE_EDGE, SURFACE_EDGE))
    {
        close_surface(&source);
        close_surface(&dest);
        set_fail();
        return;
    }
    fill_rect(source.Dc, SURFACE_EDGE, SURFACE_EDGE, RGB(255, 0, 0));
    BitBlt(dest.Dc, 0, 0, SURFACE_EDGE, SURFACE_EDGE, source.Dc, 0, 0, SRCCOPY);
    GdiFlush();
    set_pixels(dest.Bits, PIXELS);
    close_surface(&source);
    close_surface(&dest);
}

static void
case_stretch(void)
{
    SURFACE source;
    SURFACE dest;

    memset(&source, 0, sizeof(source));
    memset(&dest, 0, sizeof(dest));
    if (!open_surface(&source, 32, 32) || !open_surface(&dest, SURFACE_EDGE, SURFACE_EDGE))
    {
        close_surface(&source);
        close_surface(&dest);
        set_fail();
        return;
    }
    fill_rect(source.Dc, 32, 32, RGB(255, 0, 0));
    SetStretchBltMode(dest.Dc, COLORONCOLOR);
    StretchBlt(dest.Dc, 0, 0, SURFACE_EDGE, SURFACE_EDGE, source.Dc, 0, 0, 32, 32, SRCCOPY);
    GdiFlush();
    set_pixels(dest.Bits, PIXELS);
    close_surface(&source);
    close_surface(&dest);
}

static void
case_alpha(void)
{
    HMODULE image;
    PFN_ALPHABLEND blend;
    SURFACE source;
    SURFACE dest;
    BLENDFUNCTION fn;

    memset(&source, 0, sizeof(source));
    memset(&dest, 0, sizeof(dest));

    image = LoadLibraryW(L"msimg32.dll");
    if (image == NULL)
    {
        set_missing();
        return;
    }
    blend = (PFN_ALPHABLEND)(void *)GetProcAddress(image, "AlphaBlend");
    if (blend == NULL)
    {
        FreeLibrary(image);
        set_missing();
        return;
    }
    if (!open_surface(&source, SURFACE_EDGE, SURFACE_EDGE) ||
        !open_surface(&dest, SURFACE_EDGE, SURFACE_EDGE))
    {
        close_surface(&source);
        close_surface(&dest);
        FreeLibrary(image);
        set_fail();
        return;
    }
    fill_rect(source.Dc, SURFACE_EDGE, SURFACE_EDGE, RGB(255, 0, 0));
    memset(&fn, 0, sizeof(fn));
    fn.BlendOp = AC_SRC_OVER;
    fn.SourceConstantAlpha = 128;
    if (!blend(dest.Dc, 0, 0, SURFACE_EDGE, SURFACE_EDGE, source.Dc, 0, 0, SURFACE_EDGE,
            SURFACE_EDGE, fn))
    {
        close_surface(&source);
        close_surface(&dest);
        FreeLibrary(image);
        set_fail();
        return;
    }
    GdiFlush();
    set_pixels(dest.Bits, PIXELS);
    close_surface(&source);
    close_surface(&dest);
    FreeLibrary(image);
}

static void
case_pixel(void)
{
    SURFACE surface;
    COLORREF color;

    if (!open_surface(&surface, SURFACE_EDGE, SURFACE_EDGE))
    {
        set_fail();
        return;
    }
    if (SetPixel(surface.Dc, 10, 10, RGB(0, 255, 0)) == (COLORREF)-1)
    {
        close_surface(&surface);
        set_fail();
        return;
    }
    color = GetPixel(surface.Dc, 10, 10);
    GdiFlush();
    if (color != RGB(0, 255, 0))
    {
        close_surface(&surface);
        set_fail();
        return;
    }
    set_pixels(surface.Bits, PIXELS);
    close_surface(&surface);
}

static void
com_release(void *object)
{
    void **table;

    if (object == NULL)
    {
        return;
    }
    table = *(void ***)object;
    ((PFN_RELEASE)table[2])(object);
}

static void
fill_tex(TEXDESC *desc, unsigned int usage, unsigned int bind, unsigned int cpu)
{
    memset(desc, 0, sizeof(*desc));
    desc->Width = SURFACE_EDGE;
    desc->Height = SURFACE_EDGE;
    desc->MipLevels = 1;
    desc->ArraySize = 1;
    desc->Format = FORMAT_RGBA8;
    desc->SampleCount = 1;
    desc->Usage = usage;
    desc->BindFlags = bind;
    desc->CpuAccess = cpu;
}

static int
create_device(void **device, void **context, int *level, HMODULE *image)
{
    PFN_D3D11_CREATE create;
    int feature = FEATURE_11_0;
    HRESULT hr;

    *device = NULL;
    *context = NULL;
    *level = 0;
    *image = LoadLibraryW(L"d3d11.dll");
    if (*image == NULL)
    {
        set_missing();
        return 0;
    }
    create = (PFN_D3D11_CREATE)(void *)GetProcAddress(*image, "D3D11CreateDevice");
    if (create == NULL)
    {
        FreeLibrary(*image);
        *image = NULL;
        set_missing();
        return 0;
    }
    hr = create(NULL, DRIVER_HARDWARE, NULL, 0, &feature, 1, D3D11_SDK, device, level, context);
    _snprintf(g_token, sizeof(g_token), "0x%08lX:0x%08lX",
        (unsigned long)hr, (unsigned long)*level);
    if (hr != 0 || *device == NULL || *context == NULL)
    {
        com_release(*context);
        com_release(*device);
        *context = NULL;
        *device = NULL;
        FreeLibrary(*image);
        *image = NULL;
        return 0;
    }
    return 1;
}

static void
case_device(void)
{
    void *device;
    void *context;
    HMODULE image;
    int level;

    if (!create_device(&device, &context, &level, &image))
    {
        return;
    }
    com_release(context);
    com_release(device);
    FreeLibrary(image);
}

static void
case_clear(void)
{
    void *device = NULL;
    void *context = NULL;
    void *color_tex = NULL;
    void *staging = NULL;
    void *view = NULL;
    void **device_vt;
    void **context_vt;
    HMODULE image = NULL;
    TEXDESC desc;
    MAPPED mapped;
    int level;
    unsigned char pixels[PIXELS];
    unsigned int y;
    HRESULT hr;
    float color[4];

    if (!create_device(&device, &context, &level, &image))
    {
        if (strcmp(g_token, "MISSING") != 0)
        {
            set_fail();
        }
        goto cleanup;
    }
    device_vt = *(void ***)device;
    context_vt = *(void ***)context;
    fill_tex(&desc, USAGE_DEFAULT, BIND_RENDER_TARGET, 0);
    hr = ((PFN_CREATE_TEX)device_vt[5])(device, &desc, NULL, &color_tex);
    if (hr != 0 || color_tex == NULL)
    {
        set_fail();
        goto cleanup;
    }
    hr = ((PFN_CREATE_RTV)device_vt[9])(device, color_tex, NULL, &view);
    if (hr != 0 || view == NULL)
    {
        set_fail();
        goto cleanup;
    }
    color[0] = 1.0f;
    color[1] = 0.0f;
    color[2] = 0.0f;
    color[3] = 1.0f;
    ((PFN_CLEAR_RTV)context_vt[50])(context, view, color);
    fill_tex(&desc, USAGE_STAGING, 0, CPU_READ);
    hr = ((PFN_CREATE_TEX)device_vt[5])(device, &desc, NULL, &staging);
    if (hr != 0 || staging == NULL)
    {
        set_fail();
        goto cleanup;
    }
    ((PFN_COPY)context_vt[47])(context, staging, color_tex);
    memset(&mapped, 0, sizeof(mapped));
    hr = ((PFN_MAP)context_vt[14])(context, staging, 0, MAP_READ, 0, &mapped);
    if (hr != 0 || mapped.Data == NULL || mapped.RowPitch < SURFACE_EDGE * 4u)
    {
        set_fail();
        goto cleanup;
    }
    for (y = 0; y < SURFACE_EDGE; y++)
    {
        memcpy(pixels + (y * SURFACE_EDGE * 4u),
            (unsigned char *)mapped.Data + (y * mapped.RowPitch),
            SURFACE_EDGE * 4u);
    }
    ((PFN_UNMAP)context_vt[15])(context, staging, 0);
    set_pixels(pixels, PIXELS);

cleanup:
    com_release(view);
    com_release(staging);
    com_release(color_tex);
    com_release(context);
    com_release(device);
    if (image != NULL)
    {
        FreeLibrary(image);
    }
}

static void *
gl_proc(HMODULE image, const char *name)
{
    return (void *)GetProcAddress(image, name);
}

static void
case_gl_draw(void)
{
    HMODULE image;
    WNDCLASSW wc;
    HWND hwnd = NULL;
    HDC dc = NULL;
    HGLRC rc = NULL;
    PIXELFORMATDESCRIPTOR pfd;
    int format;
    unsigned char pixels[PIXELS];
    PFN_WGLCREATE create_rc;
    PFN_WGLMAKE make_rc;
    PFN_WGLDELETE delete_rc;
    PFN_GLCLEARCOLOR clear_color;
    PFN_GLCLEAR clear_bits;
    PFN_GLVIEWPORT viewport;
    PFN_GLMATRIX matrix_mode;
    PFN_GLLOAD load_identity;
    PFN_GLORTHO ortho;
    PFN_GLCOLOR color3;
    PFN_GLBEGIN begin;
    PFN_GLEND end;
    PFN_GLVERTEX vertex2;
    PFN_GLFLUSH flush;
    PFN_GLREAD read_pixels;

    image = LoadLibraryW(L"opengl32.dll");
    if (image == NULL)
    {
        set_missing();
        return;
    }
    create_rc = (PFN_WGLCREATE)gl_proc(image, "wglCreateContext");
    make_rc = (PFN_WGLMAKE)gl_proc(image, "wglMakeCurrent");
    delete_rc = (PFN_WGLDELETE)gl_proc(image, "wglDeleteContext");
    clear_color = (PFN_GLCLEARCOLOR)gl_proc(image, "glClearColor");
    clear_bits = (PFN_GLCLEAR)gl_proc(image, "glClear");
    viewport = (PFN_GLVIEWPORT)gl_proc(image, "glViewport");
    matrix_mode = (PFN_GLMATRIX)gl_proc(image, "glMatrixMode");
    load_identity = (PFN_GLLOAD)gl_proc(image, "glLoadIdentity");
    ortho = (PFN_GLORTHO)gl_proc(image, "glOrtho");
    color3 = (PFN_GLCOLOR)gl_proc(image, "glColor3f");
    begin = (PFN_GLBEGIN)gl_proc(image, "glBegin");
    end = (PFN_GLEND)gl_proc(image, "glEnd");
    vertex2 = (PFN_GLVERTEX)gl_proc(image, "glVertex2f");
    flush = (PFN_GLFLUSH)gl_proc(image, "glFlush");
    read_pixels = (PFN_GLREAD)gl_proc(image, "glReadPixels");
    if (create_rc == NULL || make_rc == NULL || delete_rc == NULL || clear_color == NULL ||
        clear_bits == NULL || viewport == NULL || matrix_mode == NULL || load_identity == NULL ||
        ortho == NULL || color3 == NULL || begin == NULL || end == NULL || vertex2 == NULL ||
        flush == NULL || read_pixels == NULL)
    {
        FreeLibrary(image);
        set_missing();
        return;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"GfxProbe";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, L"GfxProbe", L"GfxProbe", WS_POPUP, 0, 0, SURFACE_EDGE, SURFACE_EDGE,
        NULL, NULL, wc.hInstance, NULL);
    if (hwnd == NULL)
    {
        set_fail();
        goto cleanup;
    }
    dc = GetDC(hwnd);
    if (dc == NULL)
    {
        set_fail();
        goto cleanup;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &pfd);
    if (format == 0 || !SetPixelFormat(dc, format, &pfd))
    {
        set_fail();
        goto cleanup;
    }
    rc = create_rc(dc);
    if (rc == NULL || !make_rc(dc, rc))
    {
        set_fail();
        goto cleanup;
    }
    viewport(0, 0, SURFACE_EDGE, SURFACE_EDGE);
    clear_color(0.0f, 0.0f, 1.0f, 1.0f);
    clear_bits(GL_COLOR_BUFFER_BIT);
    matrix_mode(GL_PROJECTION);
    load_identity();
    ortho(0.0, (double)SURFACE_EDGE, 0.0, (double)SURFACE_EDGE, -1.0, 1.0);
    matrix_mode(GL_MODELVIEW);
    load_identity();
    color3(1.0f, 0.0f, 0.0f);
    begin(GL_TRIANGLES);
    vertex2(0.0f, 0.0f);
    vertex2((float)SURFACE_EDGE, 0.0f);
    vertex2(0.0f, (float)SURFACE_EDGE);
    end();
    flush();
    memset(pixels, 0, sizeof(pixels));
    read_pixels(0, 0, SURFACE_EDGE, SURFACE_EDGE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    set_pixels(pixels, PIXELS);

cleanup:
    if (make_rc != NULL)
    {
        make_rc(NULL, NULL);
    }
    if (rc != NULL && delete_rc != NULL)
    {
        delete_rc(rc);
    }
    if (dc != NULL && hwnd != NULL)
    {
        ReleaseDC(hwnd, dc);
    }
    if (hwnd != NULL)
    {
        DestroyWindow(hwnd);
    }
    FreeLibrary(image);
}

static HANDLE
open_com(const char *path)
{
    HANDLE handle;
    DCB dcb;

    handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return handle;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb))
    {
        BuildCommDCBA("baud=115200 parity=N data=8 stop=1", &dcb);
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(handle, &dcb);
    return handle;
}

static void
open_sink(void)
{
    char marker[16];
    char path[20];
    int letter;

    for (letter = 'A'; letter <= 'Z'; letter++)
    {
        _snprintf(marker, sizeof(marker), "%c:\\SINK.TXT", letter);
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        _snprintf(path, sizeof(path), "%c:\\result.log", letter);
        g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        return;
    }
}

int
main(void)
{
    g_guard_handle = AddVectoredExceptionHandler(1, on_fault);
    g_com = open_com("\\\\.\\COM1");
    g_com2 = open_com("\\\\.\\COM2");
    open_sink();

    run_case("Module.User32", case_user32);
    run_case("Module.Gdi32", case_gdi32);
    run_case("Module.Dxgi", case_dxgi);
    run_case("Module.D3d11", case_d3d11);
    run_case("Module.Opengl32", case_opengl);
    run_case("Gdi.BitsPixel", case_bits);
    run_case("Gdi.MemoryDc", case_memory_dc);
    run_case("Gdi.Dib32", case_dib);
    run_case("Gdi.Fill", case_fill);
    run_case("Gdi.Rect", case_rect);
    run_case("Gdi.Line", case_line);
    run_case("Gdi.Blit", case_blit);
    run_case("Gdi.Stretch", case_stretch);
    run_case("Gdi.Alpha", case_alpha);
    run_case("Gdi.Pixel", case_pixel);
    run_case("D3d11.CreateDevice", case_device);
    run_case("D3d11.Clear", case_clear);
    run_case("Opengl.Draw", case_gl_draw);
    emit("END", "OK");

    if (g_com != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_com);
    }
    if (g_com2 != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_com2);
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
    }
    if (g_guard_handle != NULL)
    {
        RemoveVectoredExceptionHandler(g_guard_handle);
    }
    return 0;
}
