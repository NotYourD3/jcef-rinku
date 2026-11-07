// Shared-texture bridge for JCEF-Rinku (Windows / D3D11 <-> OpenGL).
//
// CEF's contract (see include/cef_render_handler.h, OnAcceleratedPaint):
//
//   "The handle's resource cannot be cached and cannot be accessed outside of
//    this callback. It should be reopened each time this callback is executed
//    and the contents should be copied to a texture owned by the client
//    application. The contents of |info| will be released back to the pool
//    after this callback returns."
//
//   "Handle for the shared texture. The shared texture is instantiated
//    without a keyed mutex."  (include/internal/cef_types_win.h)
//
// Therefore the ONLY valid pipeline is:
//
//   CEF UI thread, inside OnAcceleratedPaint:
//       reopen the handle -> GPU-side CopyResource into OUR staging texture
//       -> Flush -> release CEF's texture (handle is now dead)
//
//   GL render thread, once per presented frame:
//       unlock previous WGL lock (D3D may write again)
//       -> GPU-side CopyResource(staging -> owned D3D texture)
//       -> wglDXLockObjectsNV -> GL renders from the registered GL texture
//
// Both copies are VRAM->VRAM DMA copies. No BGRA pixels ever cross the CPU,
// so a 4K repaint costs ~0.1 ms of GPU time instead of ~33 MB of memcpy plus
// a blocking glTexSubImage2D upload.
//
// WGL_NV_DX_interop signatures used below are taken verbatim from
// https://registry.khronos.org/OpenGL/extensions/NV/WGL_NV_DX_interop.txt

#include <jni.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "opengl32.lib")

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#define WGL_ACCESS_READ_ONLY_NV 0x0000

#define LOG(msg, ...) do { fprintf(stderr, "[CEF-SHARED] " msg "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define LOG_EVERY(n, msg, ...) do { static int _c = 0; if (((_c++) % (n)) == 0) LOG(msg, ##__VA_ARGS__); } while(0)

// ---------------------------------------------------------------------------
// WGL_NV_DX_interop entry points. NOTE: wglDXRegisterObjectNV returns HANDLE
// and takes the D3D *resource* as its 2nd argument; wglDX{Un,}LockObjectsNV
// take an array of those HANDLEs (NOT GLuint texture names).
// ---------------------------------------------------------------------------
typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENVPROC)(void* dxDevice);
typedef BOOL(WINAPI* PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void* dxResource,
    GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI* PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI* PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);
typedef BOOL(WINAPI* PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);

static PFNWGLDXOPENDEVICENVPROC pfnOpenDevice = nullptr;
static PFNWGLDXCLOSEDEVICENVPROC pfnCloseDevice = nullptr;
static PFNWGLDXREGISTEROBJECTNVPROC pfnRegisterObject = nullptr;
static PFNWGLDXUNREGISTEROBJECTNVPROC pfnUnregisterObject = nullptr;
static PFNWGLDXLOCKOBJECTSNVPROC pfnLockObjects = nullptr;
static PFNWGLDXUNLOCKOBJECTSNVPROC pfnUnlockObjects = nullptr;

// ---------------------------------------------------------------------------
static ID3D11Device* g_device = nullptr;
static ID3D11Device1* g_device1 = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D12Device* g_d3d12Device = nullptr;
static ID3D11On12Device* g_d3d11On12 = nullptr;

static HANDLE g_wglDevice = nullptr;      // from wglDXOpenDeviceNV
static HANDLE g_registered = nullptr;     // from wglDXRegisterObjectNV
static GLuint g_glTex = 0;                // GL texture backed by g_ownedTex
static bool g_glLocked = false;

static ID3D11Texture2D* g_stagingTex = nullptr; // written on CEF UI thread
static ID3D11Texture2D* g_ownedTex = nullptr;   // registered with WGL on GL thread
static UINT g_texW = 0, g_texH = 0;                 // CEF texture (may be padded)
static UINT g_viewW = 0, g_viewH = 0;               // JS viewport, used for the GL texture
static DXGI_FORMAT g_texFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
static volatile bool g_texturesDirty = true;

static CRITICAL_SECTION g_cs;
static bool g_csInit = false;

static volatile LONG g_framesCopied = 0;
static volatile LONG g_framesBound = 0;
static volatile LONG g_openFailures = 0;
static volatile LONG g_bindFailures = 0;
static char g_lastError[256] = "none";
static char g_adapterName[128] = "unknown";

template<typename T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void EnsureCriticalSection() {
  if (!g_csInit) {
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
  }
}

static void SetLastErrorText(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(g_lastError, sizeof(g_lastError), _TRUNCATE, fmt, ap);
  va_end(ap);
}

static const char* HResultText(HRESULT hr, char* buf, size_t len) {
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, (DWORD)hr, 0, buf, (DWORD)len, nullptr);
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = 0;
  return buf;
}

// wglGetProcAddress signals "unsupported" with 1, 2, 3 or -1 rather than NULL.
static bool IsValidWglProc(FARPROC p) {
  intptr_t v = reinterpret_cast<intptr_t>(p);
  return v != 0 && v != 1 && v != 2 && v != 3 && v != static_cast<intptr_t>(-1);
}

// ---------------------------------------------------------------------------
static bool CreateD3D11On12Bridge() {
  if (g_d3d11On12) return true;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d12Device)))) {
    return false;
  }
  ID3D11Device* bridge = nullptr;
  ID3D11DeviceContext* bridgeCtx = nullptr;
  HRESULT hr = D3D11On12CreateDevice(g_d3d12Device, 0, nullptr, 0, nullptr, 0, 0,
      &bridge, &bridgeCtx, nullptr);
  SafeRelease(bridgeCtx);
  if (FAILED(hr) || !bridge) { SafeRelease(g_d3d12Device); return false; }
  hr = bridge->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&g_d3d11On12));
  SafeRelease(bridge);
  if (FAILED(hr)) { SafeRelease(g_d3d12Device); return false; }
  LOG("D3D11On12 bridge created");
  return true;
}

static bool EnsureD3DDevice() {
  if (g_device) return true;
  EnsureCriticalSection();

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_9_1;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
      nullptr, 0, D3D11_SDK_VERSION, &g_device, &fl, &g_context);
  if (FAILED(hr) || !g_device) {
    char b[256];
    LOG("HARDWARE device failed: %s", HResultText(hr, b, sizeof(b)));
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &g_device, &fl, &g_context);
  }
  if (FAILED(hr) || !g_device) {
    SetLastErrorText("D3D11CreateDevice failed hr=0x%08X", hr);
    return false;
  }

  IDXGIDevice* dxgiDev = nullptr;
  if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev) {
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
      DXGI_ADAPTER_DESC desc;
      if (SUCCEEDED(adapter->GetDesc(&desc))) {
        WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, g_adapterName,
            sizeof(g_adapterName), nullptr, nullptr);
        LOG("D3D11 adapter='%s' vendor=0x%04X device=0x%04X featureLevel=0x%04X",
            g_adapterName, desc.VendorId, desc.DeviceId, fl);
      }
      adapter->Release();
    }
    dxgiDev->Release();
  }
  g_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_device1);
  LOG("ID3D11Device1 available: %d", g_device1 != nullptr);
  return true;
}

// Reopen CEF's shared handle. CEF 151 documents BOTH D3D11 (OpenSharedResource1)
// and D3D12 (OpenSharedHandle) handles, so try all known variants.
static HRESULT OpenCefTexture(HANDLE handle, ID3D11Texture2D** out) {
  *out = nullptr;
  HRESULT hr = E_FAIL;

  if (g_device1) {
    hr = g_device1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(out));
    if (SUCCEEDED(hr) && *out) return hr;
  }
  if (g_device) {
    hr = g_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(out));
    if (SUCCEEDED(hr) && *out) return hr;
  }

  // D3D12 shared handle -> wrap into D3D11 so we can CopyResource to our texture.
  if (CreateD3D11On12Bridge() && g_d3d12Device && g_d3d11On12) {
    ID3D12Resource* d3d12Tex = nullptr;
    hr = g_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(&d3d12Tex));
    if (SUCCEEDED(hr) && d3d12Tex) {
      D3D11_RESOURCE_FLAGS rf = {};
      rf.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D* wrapped = nullptr;
      hr = g_d3d11On12->CreateWrappedResource(d3d12Tex, &rf,
          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&wrapped));
      d3d12Tex->Release();
      if (SUCCEEDED(hr) && wrapped) {
        *out = wrapped;
        return S_OK;
      }
    }
  }
  return FAILED(hr) ? hr : E_FAIL;
}

static ID3D11Texture2D* CreatePlainTexture(UINT w, UINT h, DXGI_FORMAT fmt, bool shared) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.SampleDesc.Quality = 0;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.CPUAccessFlags = 0;
  d.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED : 0;
  ID3D11Texture2D* tex = nullptr;
  HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &tex);
  if (FAILED(hr)) {
    char b[256];
    LOG("CreateTexture2D(%ux%u fmt=%d shared=%d) failed: %s", w, h, (int)fmt,
        (int)shared, HResultText(hr, b, sizeof(b)));
    return nullptr;
  }
  return tex;
}

// Called on the CEF UI thread. Recreate staging only; WGL objects are owned by
// the GL thread and are rebuilt lazily by BindFrame().
static bool EnsureStaging(UINT w, UINT h, DXGI_FORMAT fmt) {
  if (g_stagingTex && g_texW == w && g_texH == h && g_texFormat == fmt) return true;
  SafeRelease(g_stagingTex);
  g_stagingTex = CreatePlainTexture(w, h, fmt, false);
  if (!g_stagingTex) return false;
  g_texW = w;
  g_texH = h;
  g_texFormat = fmt;
  g_texturesDirty = true;
  LOG("Staging texture rebuilt: %ux%u fmt=%d", w, h, (int)fmt);
  return true;
}

// CEF allocates the shared texture at viewport * device_scale_factor, which is
// NOT the logical viewport size that the Java layer passes to lockSharedTexture().
// VeilUI calls setDeviceScaleFactor(2), so the shared texture is 2x the view
// rect on each axis. Sizing the GL texture to the logical viewport and copying
// only that sub-rect shows the top-left quarter stretched over the whole quad
// (cropped UI, oversized text). Derive the real content size from the shared
// texture descriptor instead, clamping away any driver-side row padding.
static void GetContentSize(UINT& outW, UINT& outH) {
  outW = g_texW;
  outH = g_texH;
  if (g_viewW == 0 || g_viewH == 0 || g_texW == 0 || g_texH == 0) return;
  UINT sx = (g_texW + g_viewW / 2) / g_viewW;
  UINT sy = (g_texH + g_viewH / 2) / g_viewH;
  if (sx < 1) sx = 1;
  if (sy < 1) sy = 1;
  UINT cw = g_viewW * sx;
  UINT ch = g_viewH * sy;
  if (cw > 0 && cw <= g_texW) outW = cw;
  if (ch > 0 && ch <= g_texH) outH = ch;
}

static bool InitWglInterop() {
  if (g_wglDevice) return true;
  if (!EnsureD3DDevice()) return false;
  if (!wglGetCurrentDC()) {
    SetLastErrorText("no current GL context (must be called on the render thread)");
    LOG_EVERY(120, "InitWglInterop: %s", g_lastError);
    return false;
  }

  FARPROC pOpen = wglGetProcAddress("wglDXOpenDeviceNV");
  FARPROC pClose = wglGetProcAddress("wglDXCloseDeviceNV");
  FARPROC pRegister = wglGetProcAddress("wglDXRegisterObjectNV");
  FARPROC pUnregister = wglGetProcAddress("wglDXUnregisterObjectNV");
  FARPROC pLock = wglGetProcAddress("wglDXLockObjectsNV");
  FARPROC pUnlock = wglGetProcAddress("wglDXUnlockObjectsNV");

  if (!IsValidWglProc(pOpen) || !IsValidWglProc(pRegister)
      || !IsValidWglProc(pUnregister) || !IsValidWglProc(pLock)
      || !IsValidWglProc(pUnlock)) {
    SetLastErrorText("WGL_NV_DX_interop not exposed by this GL driver");
    LOG("InitWglInterop: %s (open=%p register=%p lock=%p)", g_lastError,
        pOpen, pRegister, pLock);
    return false;
  }
  pfnOpenDevice = (PFNWGLDXOPENDEVICENVPROC)pOpen;
  pfnCloseDevice = (PFNWGLDXCLOSEDEVICENVPROC)pClose;
  pfnRegisterObject = (PFNWGLDXREGISTEROBJECTNVPROC)pRegister;
  pfnUnregisterObject = (PFNWGLDXUNREGISTEROBJECTNVPROC)pUnregister;
  pfnLockObjects = (PFNWGLDXLOCKOBJECTSNVPROC)pLock;
  pfnUnlockObjects = (PFNWGLDXUNLOCKOBJECTSNVPROC)pUnlock;

  g_wglDevice = pfnOpenDevice(static_cast<void*>(g_device));
  if (!g_wglDevice) {
    SetLastErrorText("wglDXOpenDeviceNV failed GetLastError=%lu (D3D/GL adapter mismatch?)",
        GetLastError());
    LOG("InitWglInterop: %s adapter='%s'", g_lastError, g_adapterName);
    return false;
  }
  LOG("InitWglInterop: SUCCESS wglDevice=%p adapter='%s'", g_wglDevice, g_adapterName);
  return true;
}

static void TearDownRegistration() {
  if (g_glLocked && g_wglDevice && g_registered && pfnUnlockObjects) {
    pfnUnlockObjects(g_wglDevice, 1, &g_registered);
    g_glLocked = false;
  }
  if (g_wglDevice && g_registered && pfnUnregisterObject) {
    pfnUnregisterObject(g_wglDevice, g_registered);
  }
  g_registered = nullptr;
  if (g_glTex) { glDeleteTextures(1, &g_glTex); g_glTex = 0; }
  SafeRelease(g_ownedTex);
}

static bool RegisterOwnedTexture() {
  TearDownRegistration();
  if (!g_stagingTex) return false;

  // The GL texture must cover the FULL CEF frame (viewport * device scale
  // factor). Consumers such as VeilUI sample it with UV 0..1, so a texture
  // sized to the logical viewport would crop the frame to its top-left corner.
  UINT ow = 0, oh = 0;
  GetContentSize(ow, oh);
  if (ow == 0 || oh == 0) return false;

  // WGL_NV_DX_interop can only register a D3D resource created as SHARED. Using a
  // non-shared texture here makes wglDXRegisterObjectNV fail (or register an
  // object whose contents never reach GL, which shows up as a solid white frame).
  g_ownedTex = CreatePlainTexture(ow, oh, g_texFormat, true);
  if (!g_ownedTex) g_ownedTex = CreatePlainTexture(ow, oh, g_texFormat, false);
  if (!g_ownedTex) {
    SetLastErrorText("could not create owned D3D11 texture");
    return false;
  }

  // A freshly created USAGE_DEFAULT texture has undefined contents, and drivers
  // commonly expose it as opaque white once registered with WGL. Clear it to
  // transparent black so any sampling before CEF's first real frame is
  // invisible instead of producing a white flash.
  if (g_context) {
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_device->CreateRenderTargetView(g_ownedTex, nullptr, &rtv)) && rtv) {
      const FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      g_context->ClearRenderTargetView(rtv, clearColor);
      g_context->Flush();
      rtv->Release();
    }
  }

  glGenTextures(1, &g_glTex);
  glBindTexture(GL_TEXTURE_2D, g_glTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  // Define storage first: NV interop requires a real GL object of matching type.
  // CEF may hand us BGRA or RGBA; the GL storage must describe the same channel
  // order or the interop copy will produce garbage (often a blank white frame).
  GLenum glPixelFormat = (g_texFormat == DXGI_FORMAT_B8G8R8A8_UNORM
      || g_texFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? GL_BGRA_EXT : GL_RGBA;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ow, oh, 0,
               glPixelFormat, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);

  g_registered = pfnRegisterObject(g_wglDevice, g_ownedTex, g_glTex,
      GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
  if (!g_registered) {
    char b[256];
    DWORD gle = GetLastError();
    SetLastErrorText("wglDXRegisterObjectNV failed GetLastError=%lu (%s)", gle,
        HResultText(HRESULT_FROM_WIN32(gle), b, sizeof(b)));
    LOG("RegisterOwnedTexture: %s", g_lastError);
    glDeleteTextures(1, &g_glTex);
    g_glTex = 0;
    SafeRelease(g_ownedTex);
    return false;
  }
  const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  LOG("Registered CEF frame texture: glTex=%u gl=%ux%u cef=%ux%u view=%ux%u fmt=%d object=%p "
      "glRenderer='%s' glVendor='%s' d3dAdapter='%s'",
      g_glTex, ow, oh, g_texW, g_texH, g_viewW, g_viewH, (int)g_texFormat, g_registered,
      glRenderer ? glRenderer : "?", glVendor ? glVendor : "?", g_adapterName);
  g_texturesDirty = false;
  return true;
}

// ---------------------------------------------------------------------------
// JNI
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL
Java_org_cef_misc_CefSharedTexture_isSupported(JNIEnv*, jclass) {
  return EnsureD3DDevice() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_cef_misc_CefSharedTexture_initWglInterop(JNIEnv*, jclass) {
  return InitWglInterop() ? JNI_TRUE : JNI_FALSE;
}

/**
 * MUST be called synchronously from inside OnAcceleratedPaint, while CEF's
 * handle is still valid. Reopens it and performs a VRAM->VRAM copy into our
 * staging texture. No pixels touch the CPU.
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_org_cef_misc_CefSharedTexture_copySharedTextureFrame(JNIEnv*, jclass,
    jlong sharedTextureHandle, jint width, jint height) {
  (void)width;
  (void)height;
  if (sharedTextureHandle == 0 || !EnsureD3DDevice()) return JNI_FALSE;

  HANDLE handle = reinterpret_cast<HANDLE>(sharedTextureHandle);
  ID3D11Texture2D* cefTex = nullptr;
  HRESULT hr = OpenCefTexture(handle, &cefTex);
  if (FAILED(hr) || !cefTex) {
    char b[256];
    InterlockedIncrement(&g_openFailures);
    SetLastErrorText("OpenSharedResource failed hr=0x%08X (%s)", hr,
        HResultText(hr, b, sizeof(b)));
    LOG_EVERY(120, "copySharedTextureFrame: %s handle=%p", g_lastError, handle);
    return JNI_FALSE;
  }

  D3D11_TEXTURE2D_DESC desc;
  cefTex->GetDesc(&desc);
  if (desc.Width == 0 || desc.Height == 0) {
    cefTex->Release();
    return JNI_FALSE;
  }

  EnterCriticalSection(&g_cs);
  bool ok = EnsureStaging(desc.Width, desc.Height, desc.Format);
  if (ok && g_context) {
    g_context->CopyResource(g_stagingTex, cefTex);
    g_context->Flush();
  }
  LeaveCriticalSection(&g_cs);
  cefTex->Release();

  if (!ok) return JNI_FALSE;
  InterlockedIncrement(&g_framesCopied);
  LOG_EVERY(240, "copySharedTextureFrame: frame=%ld %ux%u fmt=%d",
      (long)g_framesCopied, desc.Width, desc.Height, (int)desc.Format);
  return JNI_TRUE;
}

/**
 * Called on the GL render thread. Releases the previous frame's WGL lock,
 * copies staging -> owned (still VRAM only), then locks the interop object so
 * GL may sample it for the rest of this frame. Returns the GL texture id.
 *
 * The |sharedTextureHandle| argument is kept for API compatibility and is
 * intentionally ignored: the handle is already invalid at this point.
 */
extern "C" JNIEXPORT jint JNICALL
Java_org_cef_misc_CefSharedTexture_lockSharedTexture(JNIEnv*, jclass,
    jlong sharedTextureHandle, jint width, jint height) {
  (void)sharedTextureHandle;
  if (!InitWglInterop()) return 0;
  if (!g_stagingTex) return 0;

  if (width > 0 && height > 0
      && (g_viewW != (UINT)width || g_viewH != (UINT)height)) {
    g_viewW = (UINT)width;
    g_viewH = (UINT)height;
    g_texturesDirty = true;
  }

  if (g_glLocked && g_wglDevice && g_registered && pfnUnlockObjects) {
    pfnUnlockObjects(g_wglDevice, 1, &g_registered);
    g_glLocked = false;
  }

  EnterCriticalSection(&g_cs);
  if (g_texturesDirty || !g_registered || !g_ownedTex) {
    // Stays under the D3D lock so the CEF thread cannot swap the staging
    // texture while we read its dimensions and (re)register with WGL.
    if (!RegisterOwnedTexture()) {
      LeaveCriticalSection(&g_cs);
      InterlockedIncrement(&g_bindFailures);
      return 0;
    }
  }
  bool copied = false;
  if (g_ownedTex && g_stagingTex && g_context) {
    // Copy the FULL CEF frame (viewport * device scale factor), not just the
    // logical viewport. UV 0..1 consumers must see the whole page.
    UINT cw = 0, ch = 0;
    GetContentSize(cw, ch);
    D3D11_BOX box = {0, 0, 0, cw, ch, 1};
    g_context->CopySubresourceRegion(g_ownedTex, 0, 0, 0, 0, g_stagingTex, 0, &box);
    g_context->Flush();
    copied = true;
  }
  LeaveCriticalSection(&g_cs);

  if (!copied) return 0;
  if (!pfnLockObjects(g_wglDevice, 1, &g_registered)) {
    char b[256];
    DWORD gle = GetLastError();
    SetLastErrorText("wglDXLockObjectsNV failed GetLastError=%lu (%s)", gle,
        HResultText(HRESULT_FROM_WIN32(gle), b, sizeof(b)));
    LOG_EVERY(120, "lockSharedTexture: %s", g_lastError);
    InterlockedIncrement(&g_bindFailures);
    return 0;
  }
  g_glLocked = true;
  InterlockedIncrement(&g_framesBound);
  LOG_EVERY(240, "lockSharedTexture: frame=%ld glTex=%u", (long)g_framesBound, g_glTex);
  return static_cast<jint>(g_glTex);
}

extern "C" JNIEXPORT void JNICALL
Java_org_cef_misc_CefSharedTexture_unlockSharedTexture(JNIEnv*, jclass) {
  if (g_glLocked && g_wglDevice && g_registered && pfnUnlockObjects) {
    pfnUnlockObjects(g_wglDevice, 1, &g_registered);
    g_glLocked = false;
  }
}

/**
 * Fallback only. Must also be called inside OnAcceleratedPaint (the Java layer
 * now does that), because the handle dies when the callback returns.
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_org_cef_misc_CefSharedTexture_readPixels(JNIEnv* env, jclass,
    jlong sharedTextureHandle, jint width, jint height, jobject buffer) {
  if (!EnsureD3DDevice() || sharedTextureHandle == 0 || !buffer) return JNI_FALSE;
  HANDLE handle = reinterpret_cast<HANDLE>(sharedTextureHandle);
  ID3D11Texture2D* shared = nullptr;
  if (FAILED(OpenCefTexture(handle, &shared)) || !shared) return JNI_FALSE;

  D3D11_TEXTURE2D_DESC desc;
  shared->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC sd = desc;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.BindFlags = 0;
  sd.MiscFlags = 0;

  void* dst = env->GetDirectBufferAddress(buffer);
  if (!dst) { shared->Release(); return JNI_FALSE; }

  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  EnterCriticalSection(&g_cs);
  HRESULT hr = g_device->CreateTexture2D(&sd, nullptr, &staging);
  if (SUCCEEDED(hr) && staging && g_context) {
    g_context->CopyResource(staging, shared);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
      UINT w = desc.Width < (UINT)width ? desc.Width : (UINT)width;
      UINT h = desc.Height < (UINT)height ? desc.Height : (UINT)height;
      UINT rowBytes = w * 4;
      const uint8_t* src = (const uint8_t*)mapped.pData;
      uint8_t* out = (uint8_t*)dst;
      for (UINT y = 0; y < h; y++) memcpy(out + y * rowBytes, src + y * mapped.RowPitch, rowBytes);
      g_context->Unmap(staging, 0);
      ok = true;
    }
  }
  LeaveCriticalSection(&g_cs);
  SafeRelease(staging);
  shared->Release();
  return ok ? JNI_TRUE : JNI_FALSE;
}

/** Actual pixel size of the GL texture backing the frame: high 32 bits = width. */
extern "C" JNIEXPORT jlong JNICALL
Java_org_cef_misc_CefSharedTexture_getFrameSize(JNIEnv*, jclass) {
  UINT w = 0, h = 0;
  GetContentSize(w, h);
  return (static_cast<jlong>(w) << 32) | static_cast<jlong>(h);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_cef_misc_CefSharedTexture_getDiagnostics(JNIEnv* env, jclass) {
  char buf[2048];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE,
      "adapter=%s | d3d11=%p d3d11_1=%p | wglDevice=%p registered=%p glTex=%u | "
      "texture=%ux%u fmt=%d | view=%ux%u | framesCopied=%ld framesBound=%ld | "
      "openFailures=%ld bindFailures=%ld | locked=%d | lastError=%s",
      g_adapterName, g_device, g_device1, g_wglDevice, g_registered, g_glTex,
      g_texW, g_texH, (int)g_texFormat, g_viewW, g_viewH,
      (long)g_framesCopied, (long)g_framesBound,
      (long)g_openFailures, (long)g_bindFailures,
      g_glLocked ? 1 : 0, g_lastError);
  return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT void JNICALL
Java_org_cef_misc_CefSharedTexture_cleanup(JNIEnv*, jclass) {
  EnsureCriticalSection();
  if (g_wglDevice) TearDownRegistration();
  if (g_wglDevice && pfnCloseDevice) pfnCloseDevice(g_wglDevice);
  g_wglDevice = nullptr;
  EnterCriticalSection(&g_cs);
  SafeRelease(g_stagingTex);
  LeaveCriticalSection(&g_cs);
  SafeRelease(g_context);
  SafeRelease(g_device1);
  SafeRelease(g_device);
  SafeRelease(g_d3d11On12);
  SafeRelease(g_d3d12Device);
  g_texW = g_texH = 0;
  g_texturesDirty = true;
  LOG("cleanup done");
}
