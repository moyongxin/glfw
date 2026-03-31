//========================================================================
// GLFW 3.5 Win32 DXGI fallback context
//------------------------------------------------------------------------
// Copyright (c) 2026 moyongxin
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#define COBJMACROS
#include "internal.h"

#if defined(_GLFW_WIN32)

#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <stddef.h>

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif

#ifndef GLsizei
typedef int GLsizei;
#endif

#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

#ifndef DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 2048
#endif

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200
#endif

typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice,
                                                     void *dxObject,
                                                     GLuint name, GLenum type,
                                                     GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice,
                                                     HANDLE hObject);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count,
                                                HANDLE *hObjects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count,
                                                  HANDLE *hObjects);

typedef void(WINAPI *PFNGLGENTEXTURESPROC)(GLsizei n, GLuint *textures);
typedef void(WINAPI *PFNGLDELETETEXTURESPROC)(GLsizei n,
                                              const GLuint *textures);
typedef void(WINAPI *PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);

static PFNWGLDXOPENDEVICENVPROC _wglDXOpenDeviceNV;
static PFNWGLDXCLOSEDEVICENVPROC _wglDXCloseDeviceNV;
static PFNWGLDXREGISTEROBJECTNVPROC _wglDXRegisterObjectNV;
static PFNWGLDXUNREGISTEROBJECTNVPROC _wglDXUnregisterObjectNV;
static PFNWGLDXLOCKOBJECTSNVPROC _wglDXLockObjectsNV;
static PFNWGLDXUNLOCKOBJECTSNVPROC _wglDXUnlockObjectsNV;

static GLFWbool createInteropSurface(_GLFWwindow *window, int width,
                                     int height);

static DXGI_FORMAT chooseSwapchainFormat(const _GLFWfbconfig *fbconfig) {
    if (fbconfig->floatbuffer || fbconfig->redBits >= 16 ||
        fbconfig->greenBits >= 16 || fbconfig->blueBits >= 16) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    if (fbconfig->redBits >= 10 || fbconfig->greenBits >= 10 ||
        fbconfig->blueBits >= 10) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

static void assignWindowColorStateFromFormat(_GLFWwindow *window,
                                             DXGI_FORMAT format) {
    window->win32.dxgiSwapchainFormat = (uint32_t)format;

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) { // scRGB + 16-bit float
        window->bitsPerSample = 16;
        window->win32.dxgiColorPrimaries = 1;
        // Start with sRGB transfer until scRGB colorspace is successfully set.
        window->win32.dxgiColorTransfer = 5;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) { // PQ + 10-bit UNORM
        window->bitsPerSample = 10;
        window->win32.dxgiColorPrimaries = 9;
        window->win32.dxgiColorTransfer = 16;
    } else { // sRGB + 8-bit UNORM
        window->bitsPerSample = 8;
        window->win32.dxgiColorPrimaries = 1;
        window->win32.dxgiColorTransfer = 10;
    }
}

static void configureSwapchainColorSpace(_GLFWwindow *window,
                                         IDXGISwapChain *swapchain) {
    IDXGISwapChain3 *swapchain3 = NULL;
    DXGI_COLOR_SPACE_TYPE requested;
    UINT support = 0;
    HRESULT hr;

    if (!swapchain)
        return;

    // Drive DXGI colorspace from the exposed transfer/primaries state.
    if (window->win32.dxgiColorPrimaries == 1 &&
        window->win32.dxgiColorTransfer == 5) {
        requested = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; // linear scRGB
    } else if (window->win32.dxgiColorPrimaries == 9 &&
               window->win32.dxgiColorTransfer == 16) {
        requested =
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; // HDR10 (PQ + BT.2020)
    } else {
        requested = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // sRGB
    }

    hr = IDXGISwapChain_QueryInterface(swapchain, &IID_IDXGISwapChain3,
                                       (void **)&swapchain3);
    if (FAILED(hr) || !swapchain3)
        return;

    hr =
        IDXGISwapChain3_CheckColorSpaceSupport(swapchain3, requested, &support);
    if (SUCCEEDED(hr) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        hr = IDXGISwapChain3_SetColorSpace1(swapchain3, requested);
        if (FAILED(hr) &&
            requested == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
            _glfwInputError(
                GLFW_PLATFORM_ERROR,
                "Win32: Failed to set DXGI scRGB colorspace (0x%08lX)",
                (unsigned long)hr);
            window->win32.dxgiColorPrimaries = 1;
            window->win32.dxgiColorTransfer = 10;
        } else if (FAILED(hr) &&
                   requested == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            _glfwInputError(
                GLFW_PLATFORM_ERROR,
                "Win32: Failed to set DXGI HDR10 colorspace (0x%08lX)",
                (unsigned long)hr);
            window->win32.dxgiColorPrimaries = 1;
            window->win32.dxgiColorTransfer = 10;
        } else if (SUCCEEDED(hr) &&
                   requested == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
            window->win32.dxgiColorPrimaries = 1;
            window->win32.dxgiColorTransfer = 5;
        } else if (SUCCEEDED(hr) &&
                   requested == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            window->win32.dxgiColorPrimaries = 9;
            window->win32.dxgiColorTransfer = 16;
        }
    } else if (requested == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Win32: DXGI swapchain does not support scRGB colorspace");
        window->win32.dxgiColorPrimaries = 1;
        window->win32.dxgiColorTransfer = 10;
    } else if (requested == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Win32: DXGI swapchain does not support HDR10 colorspace");
        window->win32.dxgiColorPrimaries = 1;
        window->win32.dxgiColorTransfer = 10;
    }

    IDXGISwapChain3_Release(swapchain3);
}

static void makeContextCurrentDXGIWGL(_GLFWwindow *window) {
    if (window) {
        if (wglMakeCurrent(window->win32.dxgiWglDC, window->win32.dxgiWglRC))
            _glfwPlatformSetTls(&_glfw.contextSlot, window);
        else {
            _glfwInputErrorWin32(
                GLFW_PLATFORM_ERROR,
                "WGL: Failed to make DXGI helper context current");
            _glfwPlatformSetTls(&_glfw.contextSlot, NULL);
        }
    } else {
        if (!wglMakeCurrent(NULL, NULL)) {
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR,
                                 "WGL: Failed to clear current context");
        }

        _glfwPlatformSetTls(&_glfw.contextSlot, NULL);
    }
}

static void swapBuffersDXGIWGL(_GLFWwindow *window) {
    SwapBuffers(window->win32.dxgiWglDC);
}

static void swapIntervalDXGIWGL(int interval) {
    _GLFWwindow *window = _glfwPlatformGetTls(&_glfw.contextSlot);
    if (!window)
        return;

    window->win32.dxgiSwapInterval = interval;

    if (_glfw.wgl.EXT_swap_control)
        wglSwapIntervalEXT(interval);
}

static int extensionSupportedDXGIWGL(const char *extension) {
    const char *extensions = NULL;

    if (_glfw.wgl.GetExtensionsStringARB)
        extensions = wglGetExtensionsStringARB(wglGetCurrentDC());
    else if (_glfw.wgl.GetExtensionsStringEXT)
        extensions = wglGetExtensionsStringEXT();

    if (!extensions)
        return GLFW_FALSE;

    return _glfwStringInExtensionString(extension, extensions);
}

static GLFWglproc getProcAddressDXGIWGL(const char *procname) {
    const GLFWglproc proc = (GLFWglproc)wglGetProcAddress(procname);
    if (proc)
        return proc;

    return (GLFWglproc)_glfwPlatformGetModuleSymbol(_glfw.wgl.instance,
                                                    procname);
}

static void destroyContextDXGIWGL(_GLFWwindow *window) {
    if (window->win32.dxgiUsesHelperContext) {
        if (window->win32.dxgiWglRC) {
            wglDeleteContext(window->win32.dxgiWglRC);
            window->win32.dxgiWglRC = NULL;
        }

        if (window->win32.dxgiWglDC) {
            ReleaseDC(_glfw.win32.helperWindowHandle, window->win32.dxgiWglDC);
            window->win32.dxgiWglDC = NULL;
        }

        window->win32.dxgiUsesHelperContext = GLFW_FALSE;
    }
}

static GLFWbool createHelperWGLContextForDXGI(_GLFWwindow *window) {
    PIXELFORMATDESCRIPTOR pfd;
    HDC dc;
    HGLRC rc;

    dc = GetDC(_glfw.win32.helperWindowHandle);
    if (!dc) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR,
                             "WGL: Failed to retrieve DC for helper window");
        return GLFW_FALSE;
    }

    if (GetPixelFormat(dc) == 0) {
        ZeroMemory(&pfd, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags =
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;

        if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) {
            ReleaseDC(_glfw.win32.helperWindowHandle, dc);
            _glfwInputErrorWin32(
                GLFW_PLATFORM_ERROR,
                "WGL: Failed to set pixel format for helper context");
            return GLFW_FALSE;
        }
    }

    rc = wglCreateContext(dc);
    if (!rc) {
        ReleaseDC(_glfw.win32.helperWindowHandle, dc);
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR,
                             "WGL: Failed to create helper context");
        return GLFW_FALSE;
    }

    window->win32.dxgiWglDC = dc;
    window->win32.dxgiWglRC = rc;
    window->win32.dxgiUsesHelperContext = GLFW_TRUE;

    window->context.source = GLFW_NATIVE_CONTEXT_API;
    window->context.client = GLFW_OPENGL_API;
    window->context.makeCurrent = makeContextCurrentDXGIWGL;
    window->context.swapBuffers = swapBuffersDXGIWGL;
    window->context.swapInterval = swapIntervalDXGIWGL;
    window->context.extensionSupported = extensionSupportedDXGIWGL;
    window->context.getProcAddress = getProcAddressDXGIWGL;
    window->context.destroy = destroyContextDXGIWGL;
    window->context.wgl.dc = dc;
    window->context.wgl.handle = rc;
    window->context.wgl.interval = 1;

    return GLFW_TRUE;
}

static GLFWbool loadInteropProcs(_GLFWwindow *window) {
    if (_wglDXOpenDeviceNV)
        return GLFW_TRUE;

    _wglDXOpenDeviceNV =
        (PFNWGLDXOPENDEVICENVPROC)window->context.getProcAddress(
            "wglDXOpenDeviceNV");
    _wglDXCloseDeviceNV =
        (PFNWGLDXCLOSEDEVICENVPROC)window->context.getProcAddress(
            "wglDXCloseDeviceNV");
    _wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)window->context.getProcAddress(
            "wglDXRegisterObjectNV");
    _wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)window->context.getProcAddress(
            "wglDXUnregisterObjectNV");
    _wglDXLockObjectsNV =
        (PFNWGLDXLOCKOBJECTSNVPROC)window->context.getProcAddress(
            "wglDXLockObjectsNV");
    _wglDXUnlockObjectsNV =
        (PFNWGLDXUNLOCKOBJECTSNVPROC)window->context.getProcAddress(
            "wglDXUnlockObjectsNV");

    if (!_wglDXOpenDeviceNV || !_wglDXCloseDeviceNV ||
        !_wglDXRegisterObjectNV || !_wglDXUnregisterObjectNV ||
        !_wglDXLockObjectsNV || !_wglDXUnlockObjectsNV) {
        _glfwInputError(
            GLFW_API_UNAVAILABLE,
            "Win32: WGL_NV_DX_interop2 entry points are unavailable");
        return GLFW_FALSE;
    }

    return GLFW_TRUE;
}

static void releaseInteropObject(_GLFWwindow *window) {
    HANDLE object = (HANDLE)window->win32.dxgiInteropObject;
    HANDLE device = (HANDLE)window->win32.dxgiInteropDevice;

    if (device && object) {
        _wglDXUnlockObjectsNV(device, 1, &object);
        _wglDXUnregisterObjectNV(device, object);
    }

    window->win32.dxgiInteropObject = NULL;
}

static void releaseGLTexture(_GLFWwindow *window) {
    if (window->win32.dxgiSwapchainImageTexture) {
        PFNGLDELETETEXTURESPROC DeleteTextures =
            (PFNGLDELETETEXTURESPROC)window->context.getProcAddress(
                "glDeleteTextures");
        if (DeleteTextures) {
            GLuint texture = window->win32.dxgiSwapchainImageTexture;
            DeleteTextures(1, &texture);
        }

        window->win32.dxgiSwapchainImageTexture = 0;
    }
}

static void releaseD3DObjects(_GLFWwindow *window) {
    if (window->win32.dxgiInteropTexture) {
        ID3D11Texture2D_Release(
            (ID3D11Texture2D *)window->win32.dxgiInteropTexture);
        window->win32.dxgiInteropTexture = NULL;
    }

    if (window->win32.dxgiBackBuffer) {
        ID3D11Texture2D_Release(
            (ID3D11Texture2D *)window->win32.dxgiBackBuffer);
        window->win32.dxgiBackBuffer = NULL;
    }
}

static void releaseDeviceChain(_GLFWwindow *window) {
    if (window->win32.dxgiSwapchain) {
        IDXGISwapChain_Release((IDXGISwapChain *)window->win32.dxgiSwapchain);
        window->win32.dxgiSwapchain = NULL;
    }

    if (window->win32.dxgiDeviceContext) {
        ID3D11DeviceContext_Release(
            (ID3D11DeviceContext *)window->win32.dxgiDeviceContext);
        window->win32.dxgiDeviceContext = NULL;
    }

    if (window->win32.dxgiDevice) {
        ID3D11Device_Release((ID3D11Device *)window->win32.dxgiDevice);
        window->win32.dxgiDevice = NULL;
    }
}

static void releaseInteropDevice(_GLFWwindow *window) {
    if (window->win32.dxgiInteropDevice) {
        _wglDXCloseDeviceNV((HANDLE)window->win32.dxgiInteropDevice);
        window->win32.dxgiInteropDevice = NULL;
    }
}

static void makeWindowContextCurrent(_GLFWwindow *window,
                                     _GLFWwindow **previous) {
    *previous = _glfwPlatformGetTls(&_glfw.contextSlot);

    if (*previous && *previous != window)
        (*previous)->context.makeCurrent(NULL);

    window->context.makeCurrent(window);
}

static void restorePreviousContext(_GLFWwindow *previous, _GLFWwindow *window) {
    if (previous == window)
        return;

    window->context.makeCurrent(NULL);

    if (previous)
        previous->context.makeCurrent(previous);
}

static void disableDXGIFallbackWin32(_GLFWwindow *window, const char *operation,
                                     HRESULT hr) {
    _glfwInputError(GLFW_PLATFORM_ERROR,
                    "Win32: DXGI %s failed (0x%08lX), disabling fallback",
                    operation, (unsigned long)hr);
    _glfwDestroyDXGIFallbackWin32(window);
}

static GLFWbool isDXGIDeviceLostError(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
           hr == DXGI_ERROR_DEVICE_HUNG;
}

static GLFWbool recreateDXGIFallbackResources(_GLFWwindow *window) {
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgiDevice = NULL;
    D3D_FEATURE_LEVEL featureLevel;
    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_FORMAT format;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    RECT rect;
    HRESULT hr;
    int swapInterval = window->win32.dxgiSwapInterval;

    releaseInteropObject(window);
    releaseGLTexture(window);
    releaseD3DObjects(window);
    releaseInteropDevice(window);
    releaseDeviceChain(window);

    window->win32.dxgiInteropActive = GLFW_FALSE;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                           D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to recreate D3D11 device (0x%08lX)",
                        (unsigned long)hr);
        return GLFW_FALSE;
    }

    (void)featureLevel;

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice,
                                     (void **)&dxgiDevice);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to query IDXGIDevice during recovery "
                        "(0x%08lX)",
                        (unsigned long)hr);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return GLFW_FALSE;
    }

    hr = IDXGIDevice_GetAdapter(dxgiDevice, &adapter);
    IDXGIDevice_Release(dxgiDevice);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to get DXGI adapter during recovery "
                        "(0x%08lX)",
                        (unsigned long)hr);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return GLFW_FALSE;
    }

    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
    IDXGIAdapter_Release(adapter);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to get DXGI factory during recovery "
                        "(0x%08lX)",
                        (unsigned long)hr);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return GLFW_FALSE;
    }

    format = (DXGI_FORMAT)window->win32.dxgiSwapchainFormat;
    if (format == DXGI_FORMAT_UNKNOWN)
        format = DXGI_FORMAT_R8G8B8A8_UNORM;

    ZeroMemory(&desc, sizeof(desc));
    desc.BufferCount = 2;
    desc.BufferDesc.Format = format;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window->win32.handle;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc,
                                      &swapchain);
    if (FAILED(hr) && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc,
                                          &swapchain);
    }

    IDXGIFactory_Release(factory);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to recreate DXGI swapchain (0x%08lX)",
                        (unsigned long)hr);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return GLFW_FALSE;
    }

    window->win32.dxgiInteropDevice = _wglDXOpenDeviceNV(device);
    if (!window->win32.dxgiInteropDevice) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to reopen WGL DX interop device");
        IDXGISwapChain_Release(swapchain);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return GLFW_FALSE;
    }

    window->win32.dxgiDevice = device;
    window->win32.dxgiDeviceContext = context;
    window->win32.dxgiSwapchain = swapchain;
    window->win32.dxgiAllowTearing =
        (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    window->win32.dxgiSwapInterval = swapInterval;

    assignWindowColorStateFromFormat(window, format);
    configureSwapchainColorSpace(window, swapchain);

    GetClientRect(window->win32.handle, &rect);
    if (!createInteropSurface(
            window, rect.right - rect.left > 0 ? rect.right - rect.left : 1,
            rect.bottom - rect.top > 0 ? rect.bottom - rect.top : 1)) {
        _glfwDestroyDXGIFallbackWin32(window);
        return GLFW_FALSE;
    }

    window->win32.dxgiInteropActive = GLFW_TRUE;
    return GLFW_TRUE;
}

static GLFWbool handleDXGIDeviceLoss(_GLFWwindow *window, const char *operation,
                                     HRESULT hr) {
    ID3D11Device *device;
    HRESULT reason;

    if (isDXGIDeviceLostError(hr)) {
        if (recreateDXGIFallbackResources(window)) {
            _glfwInputError(GLFW_PLATFORM_ERROR,
                            "Win32: DXGI %s failed (0x%08lX), "
                            "recreated fallback resources",
                            operation, (unsigned long)hr);
            return GLFW_TRUE;
        }

        disableDXGIFallbackWin32(window, operation, hr);
        return GLFW_TRUE;
    }

    device = (ID3D11Device *)window->win32.dxgiDevice;
    if (!device)
        return GLFW_FALSE;

    reason = ID3D11Device_GetDeviceRemovedReason(device);
    if (FAILED(reason)) {
        if (recreateDXGIFallbackResources(window)) {
            _glfwInputError(GLFW_PLATFORM_ERROR,
                            "Win32: DXGI %s failed (0x%08lX), "
                            "recreated fallback after removed reason "
                            "(0x%08lX)",
                            operation, (unsigned long)hr,
                            (unsigned long)reason);
            return GLFW_TRUE;
        }

        disableDXGIFallbackWin32(window, operation, reason);
        return GLFW_TRUE;
    }

    return GLFW_FALSE;
}

static GLFWbool getCurrentSwapchainBackBuffer(_GLFWwindow *window,
                                              ID3D11Texture2D **backBuffer) {
    if (!backBuffer || !window->win32.dxgiBackBuffer)
        return GLFW_FALSE;

    *backBuffer = (ID3D11Texture2D *)window->win32.dxgiBackBuffer;

    return GLFW_TRUE;
}

static GLFWbool cacheSwapchainBackBuffers(_GLFWwindow *window) {
    IDXGISwapChain *swapchain = (IDXGISwapChain *)window->win32.dxgiSwapchain;
    ID3D11Texture2D *buffer = NULL;
    HRESULT hr;

    if (!swapchain)
        return GLFW_FALSE;

    // We use FLIP_DISCARD swapchains, just get the first back buffer
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
                                  (void **)&buffer);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to get DXGI back buffer 0 (0x%08lX)",
                        (unsigned long)hr);
        return GLFW_FALSE;
    }

    window->win32.dxgiBackBuffer = buffer;
    return GLFW_TRUE;
}

static GLFWbool createInteropSurface(_GLFWwindow *window, int width,
                                     int height) {
    ID3D11Device *device = (ID3D11Device *)window->win32.dxgiDevice;
    ID3D11Texture2D *shared = NULL;
    IDXGIResource *sharedResource = NULL;
    D3D11_TEXTURE2D_DESC desc;
    HANDLE interopObject = NULL;
    HANDLE interopDevice = (HANDLE)window->win32.dxgiInteropDevice;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLBINDTEXTUREPROC BindTexture;
    _GLFWwindow *previous;
    HRESULT hr;
    HANDLE sharedHandle = NULL;
    GLuint texture = 0;

    releaseInteropObject(window);
    releaseGLTexture(window);
    releaseD3DObjects(window);

    if (!cacheSwapchainBackBuffers(window))
        return GLFW_FALSE;

    ZeroMemory(&desc, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (DXGI_FORMAT)window->win32.dxgiSwapchainFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &shared);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to create DXGI interop texture");
        return GLFW_FALSE;
    }

    hr = ID3D11Texture2D_QueryInterface(shared, &IID_IDXGIResource,
                                        (void **)&sharedResource);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to query shared texture handle");
        ID3D11Texture2D_Release(shared);
        return GLFW_FALSE;
    }

    hr = IDXGIResource_GetSharedHandle(sharedResource, &sharedHandle);
    IDXGIResource_Release(sharedResource);
    if (FAILED(hr)) {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to get shared texture handle");
        ID3D11Texture2D_Release(shared);
        return GLFW_FALSE;
    }

    makeWindowContextCurrent(window, &previous);

    GenTextures =
        (PFNGLGENTEXTURESPROC)window->context.getProcAddress("glGenTextures");
    BindTexture =
        (PFNGLBINDTEXTUREPROC)window->context.getProcAddress("glBindTexture");

    if (!GenTextures || !BindTexture) {
        restorePreviousContext(previous, window);
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Win32: Failed to retrieve required OpenGL texture entry points");
        ID3D11Texture2D_Release(shared);
        return GLFW_FALSE;
    }

    GenTextures(1, &texture);
    BindTexture(GL_TEXTURE_2D, texture);

    interopObject =
        _wglDXRegisterObjectNV(interopDevice, shared, texture, GL_TEXTURE_2D,
                               WGL_ACCESS_READ_WRITE_NV);
    if (!interopObject) {
        GLuint temp = texture;
        PFNGLDELETETEXTURESPROC DeleteTextures =
            (PFNGLDELETETEXTURESPROC)window->context.getProcAddress(
                "glDeleteTextures");
        if (DeleteTextures)
            DeleteTextures(1, &temp);

        restorePreviousContext(previous, window);
        _glfwInputError(
            GLFW_API_UNAVAILABLE,
            "Win32: Failed to register DXGI texture for WGL interop");
        ID3D11Texture2D_Release(shared);
        return GLFW_FALSE;
    }

    if (!_wglDXLockObjectsNV(interopDevice, 1, &interopObject)) {
        _wglDXUnregisterObjectNV(interopDevice, interopObject);

        {
            GLuint temp = texture;
            PFNGLDELETETEXTURESPROC DeleteTextures =
                (PFNGLDELETETEXTURESPROC)window->context.getProcAddress(
                    "glDeleteTextures");
            if (DeleteTextures)
                DeleteTextures(1, &temp);
        }

        restorePreviousContext(previous, window);
        _glfwInputError(GLFW_API_UNAVAILABLE,
                        "Win32: Failed to lock WGL DX interop object");
        ID3D11Texture2D_Release(shared);
        return GLFW_FALSE;
    }

    restorePreviousContext(previous, window);

    window->win32.dxgiInteropTexture = shared;
    window->win32.dxgiInteropObject = interopObject;
    window->win32.dxgiSwapchainImageTexture = texture;
    window->win32.dxgiSwapchainImageHandle = (uint64_t)(uintptr_t)sharedHandle;

    return GLFW_TRUE;
}

GLFWbool _glfwCreateDXGIFallbackWin32(_GLFWwindow *window,
                                      const _GLFWctxconfig *ctxconfig,
                                      const _GLFWfbconfig *fbconfig) {
    DXGI_FORMAT swapchainFormat;
    _GLFWwindow *previous;

    window->win32.dxgiInteropActive = GLFW_FALSE;
    window->win32.dxgiSwapchainImageTexture = 0;
    window->win32.dxgiSwapchainImageHandle = 0;
    window->win32.dxgiAllowTearing = GLFW_FALSE;
    window->win32.dxgiUsesHelperContext = GLFW_FALSE;
    window->win32.dxgiWglDC = NULL;
    window->win32.dxgiWglRC = NULL;
    window->win32.dxgiBackBuffer = NULL;
    window->win32.dxgiSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    window->win32.dxgiColorPrimaries = 1;
    window->win32.dxgiColorTransfer = 10;

    swapchainFormat = chooseSwapchainFormat(fbconfig);
    assignWindowColorStateFromFormat(window, swapchainFormat);

    if (ctxconfig->source != GLFW_NATIVE_CONTEXT_API ||
        ctxconfig->client != GLFW_OPENGL_API) {
        _glfwInputError(
            GLFW_API_UNAVAILABLE,
            "Win32: DXGI fallback requires a native OpenGL context");
        return GLFW_FALSE;
    }

    if (window->context.source != GLFW_NATIVE_CONTEXT_API ||
        window->context.client == GLFW_NO_API ||
        !window->context.getProcAddress || !window->context.swapBuffers) {
        if (!createHelperWGLContextForDXGI(window))
            return GLFW_FALSE;
    }

    makeWindowContextCurrent(window, &previous);

    if (!loadInteropProcs(window)) {
        restorePreviousContext(previous, window);
        destroyContextDXGIWGL(window);
        return GLFW_FALSE;
    }

    if (!window->context.extensionSupported ||
        !window->context.extensionSupported("WGL_NV_DX_interop2")) {
        restorePreviousContext(previous, window);
        _glfwInputError(GLFW_API_UNAVAILABLE,
                        "Win32: WGL_NV_DX_interop2 is not available");
        destroyContextDXGIWGL(window);
        return GLFW_FALSE;
    }

    restorePreviousContext(previous, window);

    if (!recreateDXGIFallbackResources(window)) {
        destroyContextDXGIWGL(window);
        return GLFW_FALSE;
    }

    return GLFW_TRUE;
}

void _glfwDestroyDXGIFallbackWin32(_GLFWwindow *window) {
    releaseInteropObject(window);
    releaseGLTexture(window);
    releaseD3DObjects(window);
    releaseInteropDevice(window);
    releaseDeviceChain(window);

    window->win32.dxgiInteropActive = GLFW_FALSE;
    window->win32.dxgiSwapchainImageHandle = 0;
    window->win32.dxgiSwapchainImageTexture = 0;
    window->win32.dxgiSwapInterval = 1;
    window->win32.dxgiAllowTearing = GLFW_FALSE;
    window->win32.dxgiSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    window->win32.dxgiColorPrimaries = 1;
    window->win32.dxgiColorTransfer = 10;

    destroyContextDXGIWGL(window);
}

void _glfwResizeDXGIFallbackWin32(_GLFWwindow *window, int width, int height) {
    RECT rect;
    HRESULT hr;

    if (!window->win32.dxgiInteropActive || !window->win32.dxgiSwapchain)
        return;

    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    releaseInteropObject(window);
    releaseGLTexture(window);
    releaseD3DObjects(window);

    hr = IDXGISwapChain_ResizeBuffers(
        (IDXGISwapChain *)window->win32.dxgiSwapchain, 0, (UINT)width,
        (UINT)height, DXGI_FORMAT_UNKNOWN,
        window->win32.dxgiAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                                       : 0);

    if (FAILED(hr)) {
        if (handleDXGIDeviceLoss(window, "resize buffers", hr))
            return;

        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to resize DXGI swapchain buffers "
                        "(0x%08lX), attempting to restore interop surface",
                        (unsigned long)hr);

        GetClientRect(window->win32.handle, &rect);
        if (!createInteropSurface(
                window, rect.right - rect.left > 0 ? rect.right - rect.left : 1,
                rect.bottom - rect.top > 0 ? rect.bottom - rect.top : 1)) {
            if (handleDXGIDeviceLoss(window, "resize recovery", hr))
                return;

            disableDXGIFallbackWin32(window, "resize recovery", hr);
        }

        return;
    }

    if (!createInteropSurface(window, width, height)) {
        if (handleDXGIDeviceLoss(window, "interop surface recreation", E_FAIL))
            return;

        disableDXGIFallbackWin32(window, "interop surface recreation", E_FAIL);
        return;
    }

    configureSwapchainColorSpace(window,
                                 (IDXGISwapChain *)window->win32.dxgiSwapchain);
}

void _glfwSwapBuffersDXGIFallbackWin32(_GLFWwindow *window) {
    ID3D11DeviceContext *context;
    ID3D11Texture2D *backBuffer;
    ID3D11Texture2D *sharedTexture;
    IDXGISwapChain *swapchain;
    HANDLE interopDevice;
    HANDLE interopObject;
    UINT presentFlags = 0;
    int interval;
    HRESULT hr;

    if (!window->win32.dxgiInteropActive)
        return;

    context = (ID3D11DeviceContext *)window->win32.dxgiDeviceContext;
    backBuffer = NULL;
    sharedTexture = (ID3D11Texture2D *)window->win32.dxgiInteropTexture;
    swapchain = (IDXGISwapChain *)window->win32.dxgiSwapchain;
    interopDevice = (HANDLE)window->win32.dxgiInteropDevice;
    interopObject = (HANDLE)window->win32.dxgiInteropObject;

    if (!context || !sharedTexture || !swapchain || !interopDevice ||
        !interopObject) {
        return;
    }

    if (!_wglDXUnlockObjectsNV(interopDevice, 1, &interopObject)) {
        disableDXGIFallbackWin32(window, "unlock interop object", E_FAIL);
        return;
    }

    if (!getCurrentSwapchainBackBuffer(window, &backBuffer)) {
        disableDXGIFallbackWin32(window, "acquire back buffer", E_FAIL);
        return;
    }

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)backBuffer,
                                     (ID3D11Resource *)sharedTexture);

    interval = window->win32.dxgiSwapInterval;
    if (interval < 0)
        interval = 0;

    if (interval == 0 && window->win32.dxgiAllowTearing)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    hr = IDXGISwapChain_Present(swapchain, (UINT)interval, presentFlags);
    if (hr == DXGI_STATUS_OCCLUDED) {
        // Window is occluded; keep fallback active and try again later.
    } else if (FAILED(hr)) {
        if (handleDXGIDeviceLoss(window, "present", hr)) {
            return;
        }

        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Win32: Failed to present DXGI swapchain (0x%08lX)",
                        (unsigned long)hr);
    }

    if (!_wglDXLockObjectsNV(interopDevice, 1, &interopObject)) {
        disableDXGIFallbackWin32(window, "lock interop object", E_FAIL);
        return;
    }
}

uint32_t _glfwGetWindowSwapchainImageTextureWin32(_GLFWwindow *window) {
    return window->win32.dxgiSwapchainImageTexture;
}

uint64_t _glfwGetWindowSwapchainImageHandleWin32(_GLFWwindow *window) {
    return window->win32.dxgiSwapchainImageHandle;
}

#endif // _GLFW_WIN32
