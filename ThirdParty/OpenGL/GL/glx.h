/**
 * @file glx.h
 * @brief Minimal GLX type definitions — fallback for when libgl-dev is not installed.
 *
 * Provides GLX types, function declarations, and constants needed by SDL2 and the
 * SparkEngine OpenGL RHI backend. The actual GLX functions are resolved at runtime
 * from libGL.so.1 (which is always present when Mesa is installed, even without the
 * -dev package). This header just satisfies the compiler.
 *
 * Requires X11 development headers (libx11-dev), which are typically always present
 * on Linux systems with a display server.
 */
#ifndef __glx_h_
#define __glx_h_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>

/* GLX types */
typedef struct __GLXcontextRec* GLXContext;
typedef XID GLXDrawable;
typedef XID GLXPixmap;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
typedef XID GLXFBConfigID;
typedef struct __GLXFBConfigRec* GLXFBConfig;

#ifdef __cplusplus
extern "C" {
#endif

/* GLX 1.0/1.1 */
extern GLXContext glXCreateContext(Display*, XVisualInfo*, GLXContext, int);
extern void glXDestroyContext(Display*, GLXContext);
extern int glXMakeCurrent(Display*, GLXDrawable, GLXContext);
extern void glXSwapBuffers(Display*, GLXDrawable);
extern GLXContext glXGetCurrentContext(void);
extern GLXDrawable glXGetCurrentDrawable(void);
extern int glXQueryExtension(Display*, int*, int*);
extern int glXQueryVersion(Display*, int*, int*);
extern int glXIsDirect(Display*, GLXContext);
extern const char* glXQueryExtensionsString(Display*, int);
extern const char* glXGetClientString(Display*, int);
extern const char* glXQueryServerString(Display*, int, int);
extern XVisualInfo* glXChooseVisual(Display*, int, int*);
extern GLXPixmap glXCreateGLXPixmap(Display*, XVisualInfo*, Pixmap);
extern void glXDestroyGLXPixmap(Display*, GLXPixmap);
extern void glXWaitGL(void);
extern void glXWaitX(void);

/* GLX 1.3 */
extern GLXFBConfig* glXChooseFBConfig(Display*, int, const int*, int*);
extern GLXFBConfig* glXGetFBConfigs(Display*, int, int*);
extern int glXGetFBConfigAttrib(Display*, GLXFBConfig, int, int*);
extern XVisualInfo* glXGetVisualFromFBConfig(Display*, GLXFBConfig);
extern GLXWindow glXCreateWindow(Display*, GLXFBConfig, Window, const int*);
extern void glXDestroyWindow(Display*, GLXWindow);
extern GLXPbuffer glXCreatePbuffer(Display*, GLXFBConfig, const int*);
extern void glXDestroyPbuffer(Display*, GLXPbuffer);
extern GLXContext glXCreateNewContext(Display*, GLXFBConfig, int, GLXContext, int);
extern int glXMakeContextCurrent(Display*, GLXDrawable, GLXDrawable, GLXContext);

#ifdef __cplusplus
}
#endif

/* GLX_ARB_create_context function pointer type */
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC)(Display*, GLXFBConfig, GLXContext, int, const int*);

/* GLX 1.0 attribute tokens */
#define GLX_USE_GL             1
#define GLX_BUFFER_SIZE        2
#define GLX_LEVEL              3
#define GLX_RGBA               4
#define GLX_DOUBLEBUFFER       5
#define GLX_STEREO             6
#define GLX_AUX_BUFFERS        7
#define GLX_RED_SIZE           8
#define GLX_GREEN_SIZE         9
#define GLX_BLUE_SIZE          10
#define GLX_ALPHA_SIZE         11
#define GLX_DEPTH_SIZE         12
#define GLX_STENCIL_SIZE       13
#define GLX_ACCUM_RED_SIZE     14
#define GLX_ACCUM_GREEN_SIZE   15
#define GLX_ACCUM_BLUE_SIZE    16
#define GLX_ACCUM_ALPHA_SIZE   17

/* GLX 1.3 / multisampling */
#define GLX_SAMPLE_BUFFERS     100000
#define GLX_SAMPLES            100001

/* GLX 1.3 FBConfig tokens */
#define GLX_RENDER_TYPE        0x8011
#define GLX_RGBA_BIT           0x00000001
#define GLX_RGBA_TYPE          0x8014
#define GLX_DRAWABLE_TYPE      0x8010
#define GLX_WINDOW_BIT         0x00000001
#define GLX_PBUFFER_BIT        0x00000004
#define GLX_X_RENDERABLE       0x8012
#define GLX_X_VISUAL_TYPE      0x22
#define GLX_TRUE_COLOR         0x8002
#define GLX_VISUAL_ID          0x800B
#define GLX_NONE               0x8000

/* GLX Pbuffer tokens */
#define GLX_PBUFFER_WIDTH      0x8041
#define GLX_PBUFFER_HEIGHT     0x8040
#define GLX_MAX_PBUFFER_WIDTH  0x8016
#define GLX_MAX_PBUFFER_HEIGHT 0x8017
#define GLX_MAX_PBUFFER_PIXELS 0x8018
#define GLX_LARGEST_PBUFFER    0x801C
#define GLX_PRESERVED_CONTENTS 0x801B

/* GLX_ARB_create_context */
#define GLX_CONTEXT_MAJOR_VERSION_ARB          0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB          0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB           0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB       0x00000001
#define GLX_CONTEXT_FLAGS_ARB                  0x2094
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002

/* GLX_ARB_framebuffer_sRGB */
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB       0x20B2

/* GLX_ARB_create_context_robustness */
#define GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB              0x00000004
#define GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB    0x8256
#define GLX_NO_RESET_NOTIFICATION_ARB                  0x8261
#define GLX_LOSE_CONTEXT_ON_RESET_ARB                  0x8252

/* GLX_EXT_create_context_es2_profile */
#define GLX_CONTEXT_ES2_PROFILE_BIT_EXT        0x00000004

/* GLX_NV_float_buffer */
#define GLX_FLOAT_COMPONENTS_NV                0x20B0

/* GLX_EXT_swap_control */
#define GLX_SWAP_INTERVAL_EXT                  0x20F1
#define GLX_MAX_SWAP_INTERVAL_EXT              0x20F2

/* GLX_EXT_swap_control_tear */
#define GLX_LATE_SWAPS_TEAR_EXT                0x20F3

#endif /* __glx_h_ */
