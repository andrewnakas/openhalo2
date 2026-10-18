/* d3d8.h stand-in for fecheck builds (VS2022 x86, no XDK).
   Written from the decomp's own usage, the public Direct3D 8 API shape,
   public NV2A hardware facts and what retail Halo 2 does at its call sites.
   Never copied from an SDK header.

   The Xbox header inlines part of its API (render/texture state caches,
   push-buffer writes, locks, resource creation). Those APIs are
   `static __forceinline` bodies here that do what retail's call sites do,
   each with the matched function and instruction it was read from; the
   internal routines they call are externs named here and mapped to retail
   VAs in <WORK>/extra_symbols.csv (name, va, evidence). Routines that take
   arguments in eax/ebx/esi/edi have a "_Regs" bench stand-in declaration
   that carries the same values in ecx/edx/stack and pops what retail pops:
   fecheck checks that every value appears at retail's call and replays
   retail's effects, so the stand-in tests the caller, not the convention.
   Known gap: retail's LTCG sometimes inlines one of these routines into a
   caller (SetRenderState_ZEnable, SetVertexShaderConstant1); such callers
   stay "UNLINKED" because the routine has no decomp source.

   Values, and where each comes from:
   - D3DRS_*: indices into the render-state cache. Anchored by the decomp's
     own cache-slot evidence (docs/unknown_03a5c0.md table, the globals
     g_4b82e0..g_4b8450 = g_4b81fc[state], g_4b81fc[0xa6] = D3DRS_MAX):
     ZFUNC 0x39, ALPHABLENDENABLE 0x3b, ALPHATESTENABLE 0x3c, SRCBLEND 0x3e,
     DESTBLEND 0x3f, ZWRITEENABLE 0x40, COLORWRITEENABLE 0x43, BLENDOP 0x4a,
     BLENDCOLOR 0x4b, DEPTHCLIPCONTROL 0x52, ZENABLE 0x8f, STENCILENABLE 0x90,
     CULLMODE 0x93, ZBIAS 0x95, MAX 0xa6. States between anchors follow the
     enum order that fits all anchors; those are marked "unverified".
   - Compare/blend/cull/stencil/fill values are the NV2A (OpenGL) codes; the
     decomp writes the same raw numbers beside the names (0x203, 0x207,
     0x302, 0x303, 0x8001, 0x8006, 0x901).
   - Primitive types are the NV2A begin/end codes (GL + 1); the decomp's
     (D3DPRIMITIVETYPE)9 for a quad strip agrees.
   - Texture address/filter values equal public D3D8 and the raw numbers the
     decomp writes (address 1/3/4, filter 1/2).
   - D3DTSS_* 0..11 are anchored by retail stores into D3D__TextureState (see
     D3DDevice_SetTextureStageState below); the rest follow the enum order.
   - D3DPUSH_* are public NV2A method numbers (SET_BEGIN_END 0x17fc, INLINE_ARRAY
     0x1818, non-increment bit 0x40000000, count at bit 18).
   - D3DRS_* 57..83 are also anchored by the NV2A methods of the simple-state
     encodings (ZFUNC SET_DEPTH_FUNC 0x354 ... STIPPLEENABLE 0x147c), and the
     complex states 136..165 by the per-state routine table (CULLMODE ->
     0x3f53d0, ZBIAS -> 0x3f59b0, as the decomp's call sites show).
   - Calling conventions: as noted per declaration. */
#pragma once
#ifndef FECHECK_D3D8_H
#define FECHECK_D3D8_H

/* ---- resources (defined at the end of the file) ---- */
struct D3DDevice; struct D3DResource; struct D3DBaseTexture; struct D3DTexture; struct D3DVolumeTexture;
struct D3DCubeTexture; struct D3DSurface; struct D3DVolume; struct D3DPalette; struct D3DVertexBuffer;
struct D3DIndexBuffer; struct D3DPushBuffer; struct D3DFixup; struct D3DPixelContainer;
typedef struct D3DDevice D3DDevice; typedef struct D3DResource D3DResource; typedef struct D3DBaseTexture D3DBaseTexture;
typedef struct D3DTexture D3DTexture; typedef struct D3DVolumeTexture D3DVolumeTexture; typedef struct D3DCubeTexture D3DCubeTexture;
typedef struct D3DSurface D3DSurface; typedef struct D3DVolume D3DVolume; typedef struct D3DPalette D3DPalette;
typedef struct D3DVertexBuffer D3DVertexBuffer; typedef struct D3DIndexBuffer D3DIndexBuffer;
typedef struct D3DPushBuffer D3DPushBuffer; typedef struct D3DPixelContainer D3DPixelContainer; typedef struct D3DFixup D3DFixup;
typedef D3DDevice *LPDIRECT3DDEVICE8; typedef D3DTexture *LPDIRECT3DTEXTURE8; typedef D3DSurface *LPDIRECT3DSURFACE8;
typedef D3DBaseTexture IDirect3DBaseTexture8;
typedef D3DDevice IDirect3DDevice8;

typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a,r,g,b) ((D3DCOLOR)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))
#define D3DCOLOR_XRGB(r,g,b) D3DCOLOR_ARGB(0xff,r,g,b)

/* ---- enums ---- */
typedef enum _D3DFORMAT {
    D3DFMT_A8R8G8B8 = 0x06,  /* NV2A swizzled A8R8G8B8 texture code; unverified */
    D3DFMT_D24S8 = 0x2a,     /* NV2A swizzled Z24S8; decomp adds 1 for the float variant; unverified */
    D3DFMT_F24S8 = 0x2b,     /* unverified */
    D3DFMT_UNKNOWN = 0xffffffff,
    D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINELOOP = 3, D3DPT_LINESTRIP = 4,
    D3DPT_TRIANGLELIST = 5, D3DPT_TRIANGLESTRIP = 6, D3DPT_TRIANGLEFAN = 7,
    D3DPT_QUADLIST = 8, D3DPT_QUADSTRIP = 9, D3DPT_POLYGON = 10,
    D3DPT_MAX = 11, D3DPT_INVALID = 0x7fffffff
} D3DPRIMITIVETYPE;

typedef enum _D3DRENDERSTATETYPE {
    /* pixel-shader block 0..56: the decomp anchors only its end (ZFUNC = 0x39) */
    D3DRS_PSALPHAINPUTS0 = 0,
    D3DRS_PSFINALCOMBINERINPUTSABCD = 8,
    D3DRS_PSFINALCOMBINERINPUTSEFG = 9,
    D3DRS_PSCONSTANT0_0 = 10,
    D3DRS_PSCONSTANT1_0 = 18,
    D3DRS_PSALPHAOUTPUTS0 = 26,
    D3DRS_PSRGBINPUTS0 = 34,
    D3DRS_PSCOMPAREMODE = 42,
    D3DRS_PSFINALCOMBINERCONSTANT0 = 43,   /* unverified */
    D3DRS_PSFINALCOMBINERCONSTANT1 = 44,   /* unverified */
    D3DRS_PSRGBOUTPUTS0 = 45,
    D3DRS_PSCOMBINERCOUNT = 53,
    D3DRS_PSDOTMAPPING = 55,
    D3DRS_PSINPUTTEXTURE = 56,
    /* simple states */
    D3DRS_ZFUNC = 57,                      /* anchored */
    D3DRS_ALPHAFUNC = 58,                  /* unverified (between anchors 57 and 59) */
    D3DRS_ALPHABLENDENABLE = 59,           /* anchored */
    D3DRS_ALPHATESTENABLE = 60,            /* anchored */
    D3DRS_ALPHAREF = 61,                   /* unverified (between anchors 60 and 62) */
    D3DRS_SRCBLEND = 62,                   /* anchored */
    D3DRS_DESTBLEND = 63,                  /* anchored */
    D3DRS_ZWRITEENABLE = 64,               /* anchored */
    D3DRS_DITHERENABLE = 65,
    D3DRS_SHADEMODE = 66,
    D3DRS_COLORWRITEENABLE = 67,           /* anchored */
    D3DRS_STENCILZFAIL = 68,               /* unverified */
    D3DRS_STENCILPASS = 69,                /* unverified */
    D3DRS_STENCILFUNC = 70,                /* unverified */
    D3DRS_STENCILREF = 71,                 /* unverified */
    D3DRS_STENCILMASK = 72,                /* unverified */
    D3DRS_STENCILWRITEMASK = 73,           /* unverified */
    D3DRS_BLENDOP = 74,                    /* anchored */
    D3DRS_BLENDCOLOR = 75,                 /* anchored */
    D3DRS_SWATHWIDTH = 76,
    D3DRS_POLYGONOFFSETZSLOPESCALE = 77,   /* unverified */
    D3DRS_POLYGONOFFSETZOFFSET = 78,       /* unverified */
    D3DRS_POINTOFFSETENABLE = 79,
    D3DRS_WIREFRAMEOFFSETENABLE = 80,
    D3DRS_SOLIDOFFSETENABLE = 81,          /* unverified */
    D3DRS_DEPTHCLIPCONTROL = 82,           /* anchored */
    D3DRS_STIPPLEENABLE = 83,              /* unverified */
    /* deferred states 92..135: unverified */
    D3DRS_FOGENABLE = 92,
    D3DRS_FOGTABLEMODE = 93,
    D3DRS_FOGSTART = 94,
    D3DRS_FOGEND = 95,
    D3DRS_FOGDENSITY = 96,
    D3DRS_RANGEFOGENABLE = 97,
    D3DRS_WRAP0 = 98, D3DRS_WRAP1 = 99, D3DRS_WRAP2 = 100, D3DRS_WRAP3 = 101,
    D3DRS_LIGHTING = 102,
    D3DRS_SPECULARENABLE = 103,
    D3DRS_LOCALVIEWER = 104,
    D3DRS_COLORVERTEX = 105,
    D3DRS_POINTSIZE = 116,
    D3DRS_POINTSIZE_MIN = 117,
    D3DRS_POINTSPRITEENABLE = 118,
    D3DRS_POINTSCALEENABLE = 119,
    D3DRS_POINTSCALE_A = 120,
    D3DRS_POINTSCALE_B = 121,
    D3DRS_POINTSCALE_C = 122,
    D3DRS_POINTSIZE_MAX = 123,
    D3DRS_PATCHEDGESTYLE = 124,
    D3DRS_PATCHSEGMENTS = 125,
    D3DRS_SWAPFILTER = 126,
    D3DRS_PRESENTATIONINTERVAL = 127,
    /* complex states */
    D3DRS_PSTEXTUREMODES = 136,            /* unverified */
    D3DRS_VERTEXBLEND = 137,               /* unverified */
    D3DRS_FOGCOLOR = 138,                  /* unverified */
    D3DRS_FILLMODE = 139,                  /* unverified */
    D3DRS_BACKFILLMODE = 140,              /* unverified */
    D3DRS_TWOSIDEDLIGHTING = 141,          /* unverified */
    D3DRS_NORMALIZENORMALS = 142,          /* unverified */
    D3DRS_ZENABLE = 143,                   /* anchored */
    D3DRS_STENCILENABLE = 144,             /* anchored */
    D3DRS_STENCILFAIL = 145,               /* unverified (between anchors 144 and 147) */
    D3DRS_FRONTFACE = 146,                 /* unverified (between anchors 144 and 147) */
    D3DRS_CULLMODE = 147,                  /* anchored */
    D3DRS_TEXTUREFACTOR = 148,             /* unverified (between anchors 147 and 149) */
    D3DRS_ZBIAS = 149,                     /* anchored */
    D3DRS_LOGICOP = 150,                   /* unverified */
    D3DRS_EDGEANTIALIAS = 151,
    D3DRS_MULTISAMPLEANTIALIAS = 152,
    D3DRS_MULTISAMPLEMASK = 153,
    D3DRS_MULTISAMPLEMODE = 154,           /* unverified */
    D3DRS_MULTISAMPLERENDERTARGETMODE = 155,
    D3DRS_SHADOWFUNC = 156,                /* unverified */
    D3DRS_LINEWIDTH = 157,                 /* unverified */
    D3DRS_SAMPLEALPHA = 158,               /* unverified */
    D3DRS_DXT1NOISEENABLE = 159,           /* unverified */
    D3DRS_YUVENABLE = 160,
    D3DRS_OCCLUSIONCULLENABLE = 161,
    D3DRS_STENCILCULLENABLE = 162,
    D3DRS_ROPZCMPALWAYSREAD = 163,
    D3DRS_ROPZREAD = 164,
    D3DRS_DONOTCULLUNCOMPRESSED = 165,
    D3DRS_MAX = 166,                       /* anchored: g_4b81fc[0xa6] */
    D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {   /* 0..11 anchored by function_351a0's stores into D3D__TextureState;
                                              12/22/28..30 by the type ranges in function_18560 */
    D3DTSS_ADDRESSU = 0, D3DTSS_ADDRESSV = 1, D3DTSS_ADDRESSW = 2,
    D3DTSS_MAGFILTER = 3, D3DTSS_MINFILTER = 4, D3DTSS_MIPFILTER = 5,
    D3DTSS_MIPMAPLODBIAS = 6, D3DTSS_MAXMIPLEVEL = 7, D3DTSS_MAXANISOTROPY = 8,
    D3DTSS_COLORKEYOP = 9, D3DTSS_COLORSIGN = 10, D3DTSS_ALPHAKILL = 11,
    D3DTSS_COLOROP = 12, D3DTSS_COLORARG0 = 13, D3DTSS_COLORARG1 = 14, D3DTSS_COLORARG2 = 15,
    D3DTSS_ALPHAOP = 16, D3DTSS_ALPHAARG0 = 17, D3DTSS_ALPHAARG1 = 18, D3DTSS_ALPHAARG2 = 19,
    D3DTSS_RESULTARG = 20, D3DTSS_TEXTURETRANSFORMFLAGS = 21,
    D3DTSS_BUMPENVMAT00 = 22, D3DTSS_BUMPENVMAT01 = 23, D3DTSS_BUMPENVMAT11 = 24, D3DTSS_BUMPENVMAT10 = 25,
    D3DTSS_BUMPENVLSCALE = 26, D3DTSS_BUMPENVLOFFSET = 27, D3DTSS_TEXCOORDINDEX = 28,
    D3DTSS_BORDERCOLOR = 29, D3DTSS_COLORKEYCOLOR = 30, D3DTSS_UNSUPPORTED = 31,
    D3DTSS_MAX = 32, D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;

typedef enum _D3DTEXTUREADDRESS { D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR = 2, D3DTADDRESS_CLAMP = 3,
    D3DTADDRESS_BORDER = 4, D3DTADDRESS_CLAMPTOEDGE = 5, D3DTADDRESS_FORCE_DWORD = 0x7fffffff } D3DTEXTUREADDRESS;
typedef enum _D3DTEXTUREFILTERTYPE { D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2,
    D3DTEXF_ANISOTROPIC = 3, D3DTEXF_FORCE_DWORD = 0x7fffffff } D3DTEXTUREFILTERTYPE;
typedef enum _D3DCMPFUNC { D3DCMP_NEVER = 0x200, D3DCMP_LESS = 0x201, D3DCMP_EQUAL = 0x202, D3DCMP_LESSEQUAL = 0x203,
    D3DCMP_GREATER = 0x204, D3DCMP_NOTEQUAL = 0x205, D3DCMP_GREATEREQUAL = 0x206, D3DCMP_ALWAYS = 0x207,
    D3DCMP_FORCE_DWORD = 0x7fffffff } D3DCMPFUNC;
typedef enum _D3DBLEND { D3DBLEND_ZERO = 0, D3DBLEND_ONE = 1, D3DBLEND_SRCCOLOR = 0x300, D3DBLEND_INVSRCCOLOR = 0x301,
    D3DBLEND_SRCALPHA = 0x302, D3DBLEND_INVSRCALPHA = 0x303, D3DBLEND_DESTALPHA = 0x304, D3DBLEND_INVDESTALPHA = 0x305,
    D3DBLEND_DESTCOLOR = 0x306, D3DBLEND_INVDESTCOLOR = 0x307, D3DBLEND_SRCALPHASAT = 0x308,
    D3DBLEND_CONSTANTCOLOR = 0x8001, D3DBLEND_INVCONSTANTCOLOR = 0x8002, D3DBLEND_CONSTANTALPHA = 0x8003,
    D3DBLEND_INVCONSTANTALPHA = 0x8004, D3DBLEND_FORCE_DWORD = 0x7fffffff } D3DBLEND;
typedef enum _D3DBLENDOP { D3DBLENDOP_ADD = 0x8006, D3DBLENDOP_MIN = 0x8007, D3DBLENDOP_MAX = 0x8008,
    D3DBLENDOP_SUBTRACT = 0x800a, D3DBLENDOP_REVSUBTRACT = 0x800b, D3DBLENDOP_FORCE_DWORD = 0x7fffffff } D3DBLENDOP;
typedef enum _D3DCULL { D3DCULL_NONE = 0, D3DCULL_CW = 0x900, D3DCULL_CCW = 0x901, D3DCULL_FORCE_DWORD = 0x7fffffff } D3DCULL;
typedef enum _D3DFRONT { D3DFRONT_CW = 0x900, D3DFRONT_CCW = 0x901, D3DFRONT_FORCE_DWORD = 0x7fffffff } D3DFRONT;
typedef enum _D3DSTENCILOP { D3DSTENCILOP_ZERO = 0, D3DSTENCILOP_KEEP = 0x1e00, D3DSTENCILOP_REPLACE = 0x1e01,
    D3DSTENCILOP_INCRSAT = 0x1e02, D3DSTENCILOP_DECRSAT = 0x1e03, D3DSTENCILOP_INVERT = 0x150a,
    D3DSTENCILOP_INCR = 0x8507, D3DSTENCILOP_DECR = 0x8508, D3DSTENCILOP_FORCE_DWORD = 0x7fffffff } D3DSTENCILOP;
typedef enum _D3DFILLMODE { D3DFILL_POINT = 0x1b00, D3DFILL_WIREFRAME = 0x1b01, D3DFILL_SOLID = 0x1b02,
    D3DFILL_FORCE_DWORD = 0x7fffffff } D3DFILLMODE;

typedef enum _D3DRESOURCETYPE { D3DRTYPE_NONE = 0, D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3,
    D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
    D3DRTYPE_FORCE_DWORD = 0x7fffffff } D3DRESOURCETYPE;
typedef enum _D3DCUBEMAP_FACES { D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2, D3DCUBEMAP_FACE_NEGATIVE_Y = 3, D3DCUBEMAP_FACE_POSITIVE_Z = 4,
    D3DCUBEMAP_FACE_NEGATIVE_Z = 5, D3DCUBEMAP_FACE_FORCE_DWORD = 0x7fffffff } D3DCUBEMAP_FACES;
typedef enum _D3DPALETTESIZE { D3DPALETTESIZE_FORCE_DWORD = 0x7fffffff } D3DPALETTESIZE;  /* only cast to in the decomp */
typedef enum _D3DBACKBUFFER_TYPE { D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_FORCE_DWORD = 0x7fffffff } D3DBACKBUFFER_TYPE;
typedef enum _D3DSWAPEFFECT { D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3,
    D3DSWAPEFFECT_FORCE_DWORD = 0x7fffffff } D3DSWAPEFFECT;   /* public D3D8 values; Xbox equality unverified */
typedef enum _D3DMULTISAMPLE_TYPE { D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_FORCE_DWORD = 0x7fffffff } D3DMULTISAMPLE_TYPE;
typedef enum _D3DDEVTYPE { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_FORCE_DWORD = 0x7fffffff } D3DDEVTYPE;
typedef enum _D3DCALLBACKTYPE { D3DCALLBACK_READ = 0, D3DCALLBACK_WRITE = 1, D3DCALLBACK_FORCE_DWORD = 0x7fffffff } D3DCALLBACKTYPE; /* unverified */
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L   /* public D3D8 value; Xbox equality unverified */
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000L          /* public D3D8 value; unverified */
#define D3DLOCK_NOOVERWRITE 0x00000020L                    /* unverified */
#define D3DSDCP_SET_VERTEXPROGRAM_PLANES 1                 /* unverified */
#define D3DSDCP_SET_FIXEDFUNCTION_PLANES 2                 /* unverified (decomp passes a literal 2 at one site) */
#define D3DSDCP_USE_DEFAULT_VERTEXPROGRAM_PLANES 3         /* unverified */
#define D3DSDCP_USE_DEFAULT_FIXEDFUNCTION_PLANES 4         /* unverified */
#define D3DVSDE_VERTEX 0xffffffff                          /* -1: SET_VERTEX4F (function_254490) */
#define D3DVSDE_POSITION 0                                  /* unverified */
#define D3DDIRTYFLAG_TEXTURE_STATE_0 0x00000001             /* unverified */

/* push-buffer encoding (public NV2A method numbers) */
#define D3DPUSH_ENCODE(Method, Count) (((Count) << 18) + (Method))
#define D3DPUSH_NOINCREMENT_FLAG 0x40000000
#define D3DPUSH_SET_BEGIN_END 0x17fc
#define D3DPUSH_INLINE_ARRAY 0x1818

/* ---- structures ---- */
typedef struct _D3DLOCKED_RECT { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct _D3DLOCKED_BOX { INT RowPitch; INT SlicePitch; void *pBits; } D3DLOCKED_BOX;
typedef struct _D3DBOX { UINT Left, Top, Right, Bottom, Front, Back; } D3DBOX;
typedef struct _D3DRECT { LONG x1, y1, x2, y2; } D3DRECT;
typedef struct _D3DVIEWPORT8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT8;
typedef struct _D3DGAMMARAMP { BYTE red[256]; BYTE green[256]; BYTE blue[256]; } D3DGAMMARAMP;  /* layout unverified */
typedef struct _D3DVBLANKDATA { DWORD VBlank; DWORD Swap; DWORD Flags; } D3DVBLANKDATA;      /* layout unverified */
typedef void (__cdecl *D3DVBLANKCALLBACK)(D3DVBLANKDATA *pData);
typedef void (__cdecl *D3DCALLBACK)(DWORD Context);
typedef struct _D3DSURFACE_DESC {   /* public D3D8 shape; layout unverified */
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType; UINT Width; UINT Height;
} D3DSURFACE_DESC;
typedef struct _D3DPRESENT_PARAMETERS_ {   /* public D3D8 shape plus Xbox tail; layout unverified */
    UINT BackBufferWidth; UINT BackBufferHeight; D3DFORMAT BackBufferFormat; UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType; D3DSWAPEFFECT SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; D3DFORMAT AutoDepthStencilFormat; DWORD Flags;
    UINT FullScreen_RefreshRateInHz; UINT FullScreen_PresentationInterval;
    D3DSurface *BufferSurfaces[3]; D3DSurface *DepthStencilSurface;
} D3DPRESENT_PARAMETERS;
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 0x00000001   /* public D3D8 value; unverified */
#define D3DPRESENTFLAG_WIDESCREEN 0x00000010            /* unverified */
#define D3DPRESENTFLAG_PROGRESSIVE 0x00000040           /* unverified */
/* 0xf0 bytes; offsets confirmed by docs/unknown_03a5c0.md */
typedef struct _D3DPIXELSHADERDEF {
    DWORD PSAlphaInputs[8];
    DWORD PSFinalCombinerInputsABCD;
    DWORD PSFinalCombinerInputsEFG;
    DWORD PSConstant0[8];
    DWORD PSConstant1[8];
    DWORD PSAlphaOutputs[8];
    DWORD PSRGBInputs[8];
    DWORD PSCompareMode;
    DWORD PSFinalCombinerConstant0;
    DWORD PSFinalCombinerConstant1;
    DWORD PSRGBOutputs[8];
    DWORD PSCombinerCount;
    DWORD PSTextureModes;
    DWORD PSDotMapping;
    DWORD PSInputTexture;
    DWORD PSC0Mapping;
    DWORD PSC1Mapping;
    DWORD PSFinalCombinerConstants;
} D3DPIXELSHADERDEF;
typedef struct _D3DVERTEXATTRIBUTEFORMAT { DWORD Slots[16 * 4]; } D3DVERTEXATTRIBUTEFORMAT;  /* opaque size; unverified */
typedef struct _D3DSTREAM_INPUT { D3DVertexBuffer *VertexBuffer; UINT Stride; UINT Offset; } D3DSTREAM_INPUT; /* unverified */

/* ---- functions ---- */
#ifdef __cplusplus
extern "C" {
#endif
extern D3DDevice *D3D__pDevice;
extern DWORD D3D__RenderState[D3DRS_MAX];
extern DWORD D3D__TextureState[4][D3DTSS_MAX];
extern DWORD D3D__DirtyFlags;
extern DWORD D3D__CommandBufferSize;
extern DWORD D3D__SegmentSize;
extern const DWORD D3DSIMPLERENDERSTATEENCODE[];

void __fastcall D3DDevice_SetRenderState_Simple(DWORD Method, DWORD Value);
/* complex render states: one stdcall routine per state (retail table at 0x4038d8 + 4*state) */
void WINAPI D3DDevice_SetRenderState_PSTextureModes(DWORD Value);
void WINAPI D3DDevice_SetRenderState_VertexBlend(DWORD Value);
void WINAPI D3DDevice_SetRenderState_FogColor(DWORD Value);
void WINAPI D3DDevice_SetRenderState_FillMode(DWORD Value);
void WINAPI D3DDevice_SetRenderState_BackFillMode(DWORD Value);
void WINAPI D3DDevice_SetRenderState_TwoSidedLighting(DWORD Value);
void WINAPI D3DDevice_SetRenderState_NormalizeNormals(DWORD Value);
void WINAPI D3DDevice_SetRenderState_ZEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_StencilEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_StencilFail(DWORD Value);
void WINAPI D3DDevice_SetRenderState_FrontFace(DWORD Value);
void WINAPI D3DDevice_SetRenderState_CullMode(DWORD Value);
void WINAPI D3DDevice_SetRenderState_TextureFactor(DWORD Value);
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD Value);
void WINAPI D3DDevice_SetRenderState_LogicOp(DWORD Value);
void WINAPI D3DDevice_SetRenderState_EdgeAntiAlias(DWORD Value);
void WINAPI D3DDevice_SetRenderState_MultiSampleAntiAlias(DWORD Value);
void WINAPI D3DDevice_SetRenderState_MultiSampleMask(DWORD Value);
void WINAPI D3DDevice_SetRenderState_MultiSampleMode(DWORD Value);
void WINAPI D3DDevice_SetRenderState_MultiSampleRenderTargetMode(DWORD Value);
void WINAPI D3DDevice_SetRenderState_ShadowFunc(DWORD Value);
void WINAPI D3DDevice_SetRenderState_LineWidth(DWORD Value);
void WINAPI D3DDevice_SetRenderState_SampleAlpha(DWORD Value);
void WINAPI D3DDevice_SetRenderState_Dxt1NoiseEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_YuvEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_OcclusionCullEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_StencilCullEnable(DWORD Value);
void WINAPI D3DDevice_SetRenderState_RopZCmpAlwaysRead(DWORD Value);
void WINAPI D3DDevice_SetRenderState_RopZRead(DWORD Value);
void WINAPI D3DDevice_SetRenderState_DoNotCullUncompressed(DWORD Value);
/* deferred render states: dirty bits per state (retail table at 0x43e4d0) */
extern const DWORD D3D__DeferredRenderStateDirty[D3DRS_MAX];
/* complex texture stage states. Retail passes the stage (and for two of them the value)
   in eax/ebx/esi, which C cannot express; these declarations are bench stand-ins that
   carry the same values in ecx/edx (fecheck only checks that each value appears among
   retail's registers/stack at the call) and pop the same stack bytes as retail:
     TEXCOORDINDEX 0x3f5d20: esi = stage, push value, ret 4       (function_18560 0x186cf)
     BUMPENV*      0x3f5e30: eax = stage, push value, push type   (function_18560 0x188cd)
     BORDERCOLOR   0x3f5ea0: eax = stage, ebx = value             (function_18560 0x186e2)
     COLORKEYCOLOR 0x3f5ef0: eax = stage, ebx = value             (function_18560 0x186f5) */
void __fastcall D3DDevice_SetTextureState_TexCoordIndex(DWORD Stage, DWORD Unused, DWORD Value);
void __fastcall D3DDevice_SetTextureState_BumpEnv(DWORD Stage, DWORD Unused, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
void __fastcall D3DDevice_SetTextureState_BorderColor(DWORD Stage, DWORD Value);
void __fastcall D3DDevice_SetTextureState_ColorKeyColor(DWORD Stage, DWORD Value);
/* deferred texture states 12..21: dirty bits per state (retail table at 0x43e5e8) */
extern const DWORD D3D__DeferredTextureStateDirty[D3DTSS_MAX];
/* 0x3f8320 takes eax = stage, push texture, ret 4 (function_1cf50 0x1cf6a); stand-in: stage in ecx/edx */
void __fastcall D3DDevice_SetTexture_Regs(DWORD Stage, DWORD Stage2, D3DBaseTexture *pTexture);
/* 0x3f84d0 takes eax = stage, push palette, ret 4 (function_15b70 0x15b88); stand-in: stage in ecx/edx */
void __fastcall D3DDevice_SetPalette_Regs(DWORD Stage, DWORD Stage2, D3DPalette *pPalette);
/* 0x3fa3d0 takes the pixel-shader handle in eax (function_1ccf0 0x1cd16); bench stand-in passes it in ecx */
void __fastcall D3DDevice_SetPixelShader_Handle(DWORD Handle);
void WINAPI D3DDevice_SetVertexShader(DWORD Handle);
/* vertex shader constant uploads; Register is biased by 96 (function_151c0: -46 -> ecx 0x32) */
void __fastcall D3DDevice_SetVertexShaderConstant1(INT Register, const void *pConstantData);      /* 0x3f71d0 */
void __fastcall D3DDevice_SetVertexShaderConstant1Fast(INT Register, const void *pConstantData);  /* 0x3f7230 */
void __fastcall D3DDevice_SetVertexShaderConstant4(INT Register, const void *pConstantData);      /* 0x3f7280 */
void __fastcall D3DDevice_SetVertexShaderConstantFast_Dwords(INT Register, const void *pConstantData, DWORD DwordCount); /* 0x3f7330 */
/* 0x3f7410 takes ebx = biased register, edx = data, eax = dword count and pops nothing (function_40890
   0x40d01); the decomp calls it with those values; bench stand-in: cdecl */
void __cdecl D3DDevice_SetVertexShaderConstantNotInline(INT Register, const void *pConstantData, DWORD DwordCount);
extern DWORD D3D__VertexShaderConstants[192][4];   /* shadow copy written beside the push (0x407738) */
extern DWORD D3D__PushFlags;                        /* bit 0x10: no constant shadowing (0x404fe8) */
void WINAPI D3DDevice_LoadVertexShaderProgram(const DWORD *pFunction, DWORD Address);
/* 0x3f6cf0 takes ecx = format, eax = address, pops nothing (function_1c710 0x1c7bf); stand-in: ecx/edx */
void __fastcall D3DDevice_SelectVertexShaderDirect_Regs(D3DVERTEXATTRIBUTEFORMAT *pVAF, DWORD Address);
void WINAPI D3DDevice_SetVertexShaderInput(DWORD Handle, UINT StreamCount, D3DSTREAM_INPUT *pStreamInputs);   /* 0x3f7440 */
extern DWORD D3D__DirectVertexShader[5 + 64];   /* device-owned shader object for the Direct calls (0x408518) */
void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *pIndexData, UINT BaseVertexIndex);
/* 0x3faed0 takes ecx = start vertex, eax = vertex count, push primitive type, ret 4 (function_4ede0
   0x4ef7c); bench stand-in carries start/count in ecx/edx */
void __fastcall D3DDevice_DrawVertices_Regs(UINT StartVertex, UINT VertexCount, D3DPRIMITIVETYPE PrimitiveType);
void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE PrimitiveType, UINT VertexCount, const WORD *pIndexData);
void WINAPI D3DDevice_Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil);
void WINAPI D3DDevice_SetScissors(DWORD Count, BOOL Exclusive, const D3DRECT *pRects);
/* 0x3f6190 takes eax = push pointer, esi = device and returns the new push pointer (function_15720
   0x1576e); bench stand-in: ecx/edx */
DWORD *__fastcall D3D_PushViewportState(DWORD *Push, D3DDevice *Device);
void WINAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *pViewport);
void WINAPI D3DDevice_SetStipple(const DWORD *pPattern);
void WINAPI D3DDevice_SetRenderTarget(D3DSurface *pRenderTarget, D3DSurface *pNewZStencil);
/* 0x3f7b30 takes ebx = depth stencil, push render target, ret 4; the flags are not passed
   (function_1cd30 0x1cd62); stand-in: depth stencil in ecx/edx */
void __fastcall D3DDevice_SetRenderTargetFast_Regs(D3DSurface *pNewZStencil, D3DSurface *pNewZStencil2, D3DSurface *pRenderTarget);
void WINAPI D3DDevice_GetRenderTarget(D3DSurface **ppRenderTarget);
void WINAPI D3DDevice_SetBackBufferScale(float x, float y);
/* kernel export (thunk at 0x4116ec), public xboxkrnl shape */
__declspec(dllimport) void __stdcall AvSendTVEncoderOption(void *RegisterBase, ULONG Option, ULONG Param, ULONG *Result);
extern DWORD D3D__FlickerFilterSet;     /* 0x408640 */
extern DWORD D3D__FlickerFilter;        /* 0x408638 */
/* 0x3f8c70 takes esi = enable, pops nothing (function_13f10 0x14170); stand-in: ecx */
void __fastcall D3DDevice_SetSoftDisplayFilter_Regs(BOOL Enable);
/* 0x3ff5c6 takes eax = device +0x1c28, pops nothing (function_12a50 0x12d08); stand-in: ecx */
void __fastcall D3D_CommitGammaRamp_Regs(BYTE *Display);
/* 0x3f85d0 takes eax = index, pops nothing; 0x3f8630 takes eax = index, edx = result, push timestamp,
   ret 4 (function_20bb0 0x20d0f / 0x20cbb); stand-ins: index in ecx */
HRESULT __fastcall D3DDevice_EndVisibilityTest_Regs(DWORD Index);
HRESULT __fastcall D3DDevice_GetVisibilityTestResult_Regs(DWORD Index, UINT *pResult, ULONGLONG *pTimeStamp);
/* 0x3f9d00 takes eax = flags, pops nothing (function_13f10 0x140fc); stand-in: ecx */
DWORD __fastcall D3DDevice_Swap_Regs(DWORD Flags);
/* 0x3f77c0 takes ebx = parameters, pops nothing (function_128c0 0x129d8); stand-in: ecx */
HRESULT __fastcall D3DDevice_Reset_Regs(D3DPRESENT_PARAMETERS *pPresentationParameters);
HRESULT WINAPI D3DDevice_PersistDisplay(void);
ULONG WINAPI D3DDevice_Release(void);
/* 0x3f5240 takes eax = behavior flags, ecx = the device out-pointer, push parameters, ret 4
   (function_12490 0x1251b); adapter, type and the window are not passed. Stand-in: out-pointer in
   ecx, flags in edx */
HRESULT __fastcall Direct3D_CreateDevice_Regs(D3DDevice **ppReturnedDeviceInterface, DWORD BehaviorFlags,
                                              D3DPRESENT_PARAMETERS *pPresentationParameters);
/* 0x3fac30 takes edx = device and keeps it (function_12d0a0 0x12d0c4); stand-in: device in ecx and edx */
void __fastcall D3DDevice_KickOff_Regs(D3DDevice *Device, D3DDevice *Device2);
/* 0x3fae10 takes esi = dword count and returns the push pointer (function_4b220 0x4b24b); stand-in: ecx */
DWORD *__fastcall D3DDevice_BeginPush_Regs(DWORD Count);
DWORD *WINAPI D3DDevice_MakeSpace_Dwords(DWORD Dwords);     /* 0x3faae0 stdcall, returns the push pointer */
void WINAPI D3DDevice_SetStateBeforeBegin(void);            /* 0x400fa0: flushes the lazy state before a Begin */
DWORD WINAPI D3DDevice_SetFence(DWORD Flags);               /* 0x3fa680 (End when the device asked for a fence) */
/* 0x3fde00 takes eax = width, edx = height, then (depth, levels, format, 0, cube, volume, &format,
   &size) on the stack, ret 0x20, and returns the data size (function_158f0 0x15931, function_159b0
   0x159e2, function_15a60 0x15a97); stand-in: width/height in ecx/edx */
DWORD __fastcall D3D_InitializeTexture_Regs(UINT Width, UINT Height, UINT Depth, UINT Levels, D3DFORMAT Format,
                                            DWORD Zero, BOOL Cube, BOOL Volume, DWORD *pFormat, DWORD *pSize);
extern const DWORD D3D__PaletteBytes[4];   /* palette data bytes per D3DPALETTESIZE (0x404050) */
LPVOID WINAPI XMemAllocDefault(SIZE_T dwSize, DWORD dwAllocAttributes);
void WINAPI XMemFreeDefault(PVOID pAddress, DWORD dwAllocAttributes);

ULONG WINAPI D3DResource_AddRef(D3DResource *pThis);    /* 0x3fb600 */
ULONG WINAPI D3DResource_Release(D3DResource *pThis);   /* 0x3fb640 */
BOOL WINAPI D3DResource_IsBusy(D3DResource *pThis);
/* 0x3fad50 takes the resource in eax and pops nothing (function_1d5e0 0x1d609); bench stand-in: ecx */
void __fastcall D3DResource_BlockUntilNotBusy(D3DResource *pThis);
/* 0x3fdbc0, stdcall: (container, face, level, &bits, &row pitch, then three more outputs whose
   meaning no call site here reads); cube locks pass the face second (function_1d0e0 0x1d257) */
void WINAPI D3D_FindSurfaceWithinTexture(D3DPixelContainer *pPixels, DWORD FaceType, UINT Level, BYTE **ppbData,
                                         DWORD *pRowPitch, DWORD *pSlicePitch, DWORD *pOut6, DWORD *pOut7);
/* 0x3fdb20 takes edi = surface, esi = description, ebx = 0 and pops nothing (function_13d80 0x13e06);
   bench stand-in: ecx/edx */
void __fastcall D3DSurface_GetDesc_Regs(D3DSurface *pThis, D3DSURFACE_DESC *pDesc);
/* 0x3fa970, stdcall (time, flags): wait for the GPU (function_1d3f0 0x1d442: palette lock) */
void WINAPI D3D_BlockOnTime(DWORD Time, DWORD Flags);
/* 0x3fb7a0 takes eax = level data, edi = parent texture, push (format, size), ret 8, and returns a
   new surface (function_14850 0x148aa); stand-in: data/parent in ecx/edx */
D3DSurface *__fastcall D3D_CreateSurface_Regs(DWORD Data, D3DBaseTexture *pParent, DWORD Format, DWORD Size);

/* D3DDevice_SetRenderState, as retail expands it at every call site (matched
   function_18900 shows the whole body with a variable state; function_2c370 /
   function_496f0 show the folded constant-state forms):
     state <  92 (simple):   __fastcall 0x3f5cf0(ecx = D3DSIMPLERENDERSTATEENCODE[state], edx = value),
                             then D3D__RenderState[state] = value
     state < 136 (deferred): D3D__DirtyFlags |= per-state bits; D3D__RenderState[state] = value
     otherwise (complex):    stdcall per-state routine(value)
   The simple-state encodings are NV2A methods (count 1 << 18 | method): ZFUNC 0x354
   SET_DEPTH_FUNC, SOLIDOFFSETENABLE 0x338 POLYGON_OFFSET_FILL_ENABLE, DEPTHCLIPCONTROL
   0x1d78, ... which also anchors the D3DRS_* numbering 57..83. */
static __forceinline void WINAPI D3DDevice_SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if ((DWORD)State < 92) {
        D3DDevice_SetRenderState_Simple(D3DSIMPLERENDERSTATEENCODE[State], Value);
        D3D__RenderState[State] = Value;
    } else if ((DWORD)State < 136) {
        D3D__DirtyFlags |= D3D__DeferredRenderStateDirty[State];
        D3D__RenderState[State] = Value;
    } else switch (State) {
    case D3DRS_PSTEXTUREMODES: D3DDevice_SetRenderState_PSTextureModes(Value); break;
    case D3DRS_VERTEXBLEND: D3DDevice_SetRenderState_VertexBlend(Value); break;
    case D3DRS_FOGCOLOR: D3DDevice_SetRenderState_FogColor(Value); break;
    case D3DRS_FILLMODE: D3DDevice_SetRenderState_FillMode(Value); break;
    case D3DRS_BACKFILLMODE: D3DDevice_SetRenderState_BackFillMode(Value); break;
    case D3DRS_TWOSIDEDLIGHTING: D3DDevice_SetRenderState_TwoSidedLighting(Value); break;
    case D3DRS_NORMALIZENORMALS: D3DDevice_SetRenderState_NormalizeNormals(Value); break;
    case D3DRS_ZENABLE: D3DDevice_SetRenderState_ZEnable(Value); break;
    case D3DRS_STENCILENABLE: D3DDevice_SetRenderState_StencilEnable(Value); break;
    case D3DRS_STENCILFAIL: D3DDevice_SetRenderState_StencilFail(Value); break;
    case D3DRS_FRONTFACE: D3DDevice_SetRenderState_FrontFace(Value); break;
    case D3DRS_CULLMODE: D3DDevice_SetRenderState_CullMode(Value); break;
    case D3DRS_TEXTUREFACTOR: D3DDevice_SetRenderState_TextureFactor(Value); break;
    case D3DRS_ZBIAS: D3DDevice_SetRenderState_ZBias(Value); break;
    case D3DRS_LOGICOP: D3DDevice_SetRenderState_LogicOp(Value); break;
    case D3DRS_EDGEANTIALIAS: D3DDevice_SetRenderState_EdgeAntiAlias(Value); break;
    case D3DRS_MULTISAMPLEANTIALIAS: D3DDevice_SetRenderState_MultiSampleAntiAlias(Value); break;
    case D3DRS_MULTISAMPLEMASK: D3DDevice_SetRenderState_MultiSampleMask(Value); break;
    case D3DRS_MULTISAMPLEMODE: D3DDevice_SetRenderState_MultiSampleMode(Value); break;
    case D3DRS_MULTISAMPLERENDERTARGETMODE: D3DDevice_SetRenderState_MultiSampleRenderTargetMode(Value); break;
    case D3DRS_SHADOWFUNC: D3DDevice_SetRenderState_ShadowFunc(Value); break;
    case D3DRS_LINEWIDTH: D3DDevice_SetRenderState_LineWidth(Value); break;
    case D3DRS_SAMPLEALPHA: D3DDevice_SetRenderState_SampleAlpha(Value); break;
    case D3DRS_DXT1NOISEENABLE: D3DDevice_SetRenderState_Dxt1NoiseEnable(Value); break;
    case D3DRS_YUVENABLE: D3DDevice_SetRenderState_YuvEnable(Value); break;
    case D3DRS_OCCLUSIONCULLENABLE: D3DDevice_SetRenderState_OcclusionCullEnable(Value); break;
    case D3DRS_STENCILCULLENABLE: D3DDevice_SetRenderState_StencilCullEnable(Value); break;
    case D3DRS_ROPZCMPALWAYSREAD: D3DDevice_SetRenderState_RopZCmpAlwaysRead(Value); break;
    case D3DRS_ROPZREAD: D3DDevice_SetRenderState_RopZRead(Value); break;
    case D3DRS_DONOTCULLUNCOMPRESSED: D3DDevice_SetRenderState_DoNotCullUncompressed(Value); break;
    default: break;
    }
}
static __forceinline void WINAPI D3DDevice_GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
    *pValue = D3D__RenderState[State];     /* function_1a170 0x1a831: mov esi, [edi*4 + 0x407498] */
}

/* D3DDevice_SetTextureStageState as retail expands it (function_18560 with a variable
   type, function_351a0 / configure_351a0_texture with constant ones):
     type < 12:  D3D__DirtyFlags |= 1 << stage; D3D__TextureState[stage][type] = value
     type < 22:  D3D__DirtyFlags |= per-type bits; D3D__TextureState[stage][type] = value
     28 / 22..27 / 29 / 30: the complex routines above */
static __forceinline void WINAPI D3DDevice_SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if ((DWORD)Type < 12) {
        D3D__DirtyFlags |= 1u << Stage;
        D3D__TextureState[Stage][Type] = Value;
    } else if ((DWORD)Type < 22) {
        D3D__DirtyFlags |= D3D__DeferredTextureStateDirty[Type];
        D3D__TextureState[Stage][Type] = Value;
    } else if (Type == D3DTSS_TEXCOORDINDEX) {
        D3DDevice_SetTextureState_TexCoordIndex(Stage, Value, Value);
    } else if (Type == D3DTSS_BORDERCOLOR) {
        D3DDevice_SetTextureState_BorderColor(Stage, Value);
    } else if (Type == D3DTSS_COLORKEYCOLOR) {
        D3DDevice_SetTextureState_ColorKeyColor(Stage, Value);
    } else if ((DWORD)Type <= 27) {
        D3DDevice_SetTextureState_BumpEnv(Stage, Value, Type, Value);
    }
}

static __forceinline void WINAPI D3DDevice_GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    *pValue = D3D__TextureState[Stage][Type];   /* function_1ab50 0x1ad63: mov ebx, [ecx*4 + 0x404dd0] */
}
/* ---- push-buffer inlines. The device's first words are the push pointer, the push limit and
   state flags (function_1ee60 / function_1ef70 / function_36880, all matched). ---- */
static __forceinline DWORD *WINAPI D3DDevice_MakeSpace(void)
{
    return D3DDevice_MakeSpace_Dwords(D3D__SegmentSize >> 1);   /* mov eax,[0x408648]; shr; push; call 0x3faae0 */
}
static __forceinline DWORD *D3D__PushReserve(void)
{
    DWORD *dev = (DWORD *)D3D__pDevice;
    DWORD *push = (DWORD *)dev[0];
    if (push >= (DWORD *)dev[1])
        push = D3DDevice_MakeSpace();
    return push;
}
#define D3D__PushCommit(p) (((DWORD *)D3D__pDevice)[0] = (DWORD)(p))
static __forceinline void WINAPI Direct3D_SetPushBufferSize(DWORD PushBufferSize, DWORD KickOffSize)
{
    D3D__CommandBufferSize = PushBufferSize;   /* function_12560 0x125b8: mov [0x40864c], 0x100000 */
    D3D__SegmentSize = KickOffSize;            /*                0x125c2: mov [0x408648], 0x8000 */
}
static __forceinline void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE PrimitiveType)
{
    DWORD *dev = (DWORD *)D3D__pDevice;
    DWORD *push;
    D3DDevice_SetStateBeforeBegin();
    push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(D3DPUSH_SET_BEGIN_END, 1);
    push[1] = PrimitiveType;
    dev[0] = (DWORD)(push + 2);
    dev[2] |= 0x800;
}
static __forceinline void WINAPI D3DDevice_End(void)
{
    DWORD *dev = (DWORD *)D3D__pDevice;
    DWORD *push = D3D__PushReserve();
    DWORD flags;
    push[0] = D3DPUSH_ENCODE(D3DPUSH_SET_BEGIN_END, 1);
    push[1] = 0;
    dev[0] = (DWORD)(push + 2);
    flags = dev[2];
    dev[2] = flags & ~0x1800;
    if (flags & 0x1000)
        D3DDevice_SetFence(1);
}
/* NV2A vertex data methods: 4F 0x1a00 + 16r, 2F 0x1880 + 8r, 2S 0x1900 + 4r, 4UB 0x1940 + 4r,
   4S 0x1980 + 8r (retail: 0x101a90 for 4f(9), 0x81880 for 2f(0), 0x41900 for 2s(0), 0x41964 for
   color(9); 4ub and 4s follow the same encoding, unverified) */
static __forceinline void WINAPI D3DDevice_SetVertexData4f(INT Register, float a, float b, float c, float d)
{
    DWORD *push = D3D__PushReserve();
    /* D3DVSDE_VERTEX (-1) is SET_VERTEX4F 0x1518, which emits the vertex (function_254490: 0x101518) */
    push[0] = Register == -1 ? D3DPUSH_ENCODE(0x1518, 4) : D3DPUSH_ENCODE(0x1a00 + 16 * Register, 4);
    ((float *)push)[1] = a; ((float *)push)[2] = b; ((float *)push)[3] = c; ((float *)push)[4] = d;
    D3D__PushCommit(push + 5);
}
static __forceinline void WINAPI D3DDevice_SetVertexData2f(INT Register, float a, float b)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1880 + 8 * Register, 2);
    ((float *)push)[1] = a; ((float *)push)[2] = b;
    D3D__PushCommit(push + 3);
}
static __forceinline void WINAPI D3DDevice_SetVertexData2s(INT Register, SHORT a, SHORT b)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1900 + 4 * Register, 1);
    push[1] = ((DWORD)b << 16) | (WORD)a;
    D3D__PushCommit(push + 2);
}
static __forceinline void WINAPI D3DDevice_SetVertexData4s(INT Register, SHORT a, SHORT b, SHORT c, SHORT d)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1980 + 8 * Register, 2);
    push[1] = ((DWORD)b << 16) | (WORD)a;
    push[2] = ((DWORD)d << 16) | (WORD)c;
    D3D__PushCommit(push + 3);
}
static __forceinline void WINAPI D3DDevice_SetVertexData4ub(INT Register, BYTE a, BYTE b, BYTE c, BYTE d)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1940 + 4 * Register, 1);
    push[1] = (DWORD)a | ((DWORD)b << 8) | ((DWORD)c << 16) | ((DWORD)d << 24);
    D3D__PushCommit(push + 2);
}
static __forceinline void WINAPI D3DDevice_SetVertexDataColor(INT Register, D3DCOLOR Color)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1940 + 4 * Register, 1);
    push[1] = (Color & 0xff00ff00) | ((Color >> 16) & 0xff) | ((Color & 0xff) << 16);   /* ARGB -> ABGR */
    D3D__PushCommit(push + 2);
}

/* ---- shaders ---- */
static __forceinline void WINAPI D3DDevice_SetPixelShader(DWORD Handle)
{
    D3DDevice_SetPixelShader_Handle(Handle);
}
/* function_1ccf0 / function_1b050: a program becomes a handle built in the device at +0x924 */
static __forceinline void WINAPI D3DDevice_SetPixelShaderProgram(const D3DPIXELSHADERDEF *pPSDef)
{
    if (pPSDef) {
        DWORD *dev = (DWORD *)D3D__pDevice;
        dev[0x924 / 4] = 1;
        dev[0x928 / 4] = 0;
        dev[0x92c / 4] = (DWORD)pPSDef;
        D3DDevice_SetPixelShader_Handle((DWORD)&dev[0x924 / 4]);
    } else {
        D3DDevice_SetPixelShader_Handle(0);
    }
}
/* function_24040 shows all three count forms: 1 -> 0x3f71d0, 4 -> 0x3f7280, else a shadow copy
   unless D3D__PushFlags & 0x10, then 0x3f7330 with the dword count */
static __forceinline void WINAPI D3DDevice_SetVertexShaderConstant(INT Register, const void *pConstantData, DWORD ConstantCount)
{
    if (ConstantCount == 1) {
        D3DDevice_SetVertexShaderConstant1(Register + 96, pConstantData);
    } else if (ConstantCount == 4) {
        D3DDevice_SetVertexShaderConstant4(Register + 96, pConstantData);
    } else {
        if (!(D3D__PushFlags & 0x10)) {
            DWORD *dst = D3D__VertexShaderConstants[Register + 96];
            const DWORD *src = (const DWORD *)pConstantData;
            DWORD n = ConstantCount * 4;
            while (n--)
                *dst++ = *src++;
        }
        D3DDevice_SetVertexShaderConstantFast_Dwords(Register + 96, pConstantData, ConstantCount * 4);
    }
}
/* function_151e0: count 1 -> 0x3f7230, else 0x3f7330 */
static __forceinline void WINAPI D3DDevice_SetVertexShaderConstantFast(INT Register, const void *pConstantData, DWORD ConstantCount)
{
    if (ConstantCount == 1)
        D3DDevice_SetVertexShaderConstant1Fast(Register + 96, pConstantData);
    else
        D3DDevice_SetVertexShaderConstantFast_Dwords(Register + 96, pConstantData, ConstantCount * 4);
}
/* resource inlines: defined after the resource structures below */
static __forceinline ULONG WINAPI D3D__AddRefInline(D3DResource *pThis);
static __forceinline D3DSurface *WINAPI D3DDevice_GetBackBuffer2(INT BackBuffer);
static __forceinline void WINAPI D3DSurface_GetDesc(D3DSurface *pThis, D3DSURFACE_DESC *pDesc);
static __forceinline void WINAPI D3DSurface_LockRect(D3DSurface *pThis, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
static __forceinline void WINAPI D3DTexture_LockRect(D3DTexture *pThis, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
static __forceinline void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
static __forceinline void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *pThis, UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags);
static __forceinline void WINAPI D3DSurface_UnlockRect(D3DSurface *pThis);
static __forceinline void WINAPI D3DTexture_UnlockRect(D3DTexture *pThis, UINT Level);
static __forceinline void WINAPI D3DCubeTexture_UnlockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level);
static __forceinline void WINAPI D3DVolumeTexture_UnlockBox(D3DVolumeTexture *pThis, UINT Level);
static __forceinline D3DCOLOR *WINAPI D3DPalette_Lock2(D3DPalette *pThis, DWORD Flags);
static __forceinline void WINAPI D3DPalette_Unlock(D3DPalette *pThis);
static __forceinline void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT VertexCount)
{
    D3DDevice_DrawVertices_Regs(StartVertex, VertexCount, PrimitiveType);
}
/* function_15720 (flags 1) and function_404b0 (flags 2) expand identically: near/far into the
   device at +0x1c10/+0x1c14, dirty bit 2 at +0x1c18, then the viewport push. Flags 3/4: no evidence. */
static __forceinline void WINAPI D3DDevice_SetDepthClipPlanes(float Near, float Far, DWORD Flags)
{
    DWORD *dev = (DWORD *)D3D__pDevice;
    DWORD *push;
    (void)Flags;
    ((float *)dev)[0x1c10 / 4] = Near;
    ((float *)dev)[0x1c14 / 4] = Far;
    dev[0x1c18 / 4] |= 2;
    push = D3D__PushReserve();
    dev[0] = (DWORD)D3D_PushViewportState(push, D3D__pDevice);
}
/* function_1c710: the attribute format is copied into the device's own shader object at +0x14 and
   that object (address | 1) is selected; no format selects handle 0 */
static __forceinline void WINAPI D3DDevice_SetVertexShaderInputDirect(D3DVERTEXATTRIBUTEFORMAT *pVAF, UINT StreamCount, D3DSTREAM_INPUT *pStreamInputs)
{
    if (pVAF) {
        *(D3DVERTEXATTRIBUTEFORMAT *)&D3D__DirectVertexShader[5] = *pVAF;
        D3DDevice_SetVertexShaderInput((DWORD)D3D__DirectVertexShader | 1, StreamCount, pStreamInputs);
    } else {
        D3DDevice_SetVertexShaderInput(0, StreamCount, pStreamInputs);
    }
}
static __forceinline void WINAPI D3DDevice_SelectVertexShaderDirect(D3DVERTEXATTRIBUTEFORMAT *pVAF, DWORD Address)
{
    D3DDevice_SelectVertexShaderDirect_Regs(pVAF, Address);
}
/* function_14ac0: the viewport is a copy of device +0xee0 */
static __forceinline void WINAPI D3DDevice_GetViewport(D3DVIEWPORT8 *pViewport)
{
    *pViewport = *(D3DVIEWPORT8 *)((BYTE *)D3D__pDevice + 0xee0);
}
static __forceinline HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD Index)
{
    return D3DDevice_EndVisibilityTest_Regs(Index);
}
static __forceinline HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD Index, UINT *pResult, ULONGLONG *pTimeStamp)
{
    return D3DDevice_GetVisibilityTestResult_Regs(Index, pResult, pTimeStamp);
}
static __forceinline void WINAPI D3DDevice_KickPushBuffer(void)
{
    D3DDevice_KickOff_Regs(D3D__pDevice, D3D__pDevice);
}
/* function_12d0a0 0x12d0cf: GPU state at device +0x934; when its two words at +0x3240/+0x3244 agree
   retail reads the register at +0x400700. Every decomp call discards the result, so the value
   returned here (busy when the words differ or the register is non-zero) is unverified. */
static __forceinline BOOL WINAPI D3DDevice_IsBusy(void)
{
    BYTE *gpu = (BYTE *)((DWORD *)D3D__pDevice)[0x934 / 4];
    if (*(DWORD *)(gpu + 0x3240) != *(DWORD *)(gpu + 0x3244))
        return TRUE;
    return *(volatile DWORD *)(gpu + 0x400700) != 0;
}
/* function_245c0 0x2460e (WRITE): method 0x1d8c x2 = callback, context; WAIT_FOR_IDLE (0x110) 0;
   NO_OPERATION (0x100) 7. The READ form has no call site here: its last word is unverified. */
static __forceinline void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE Type, D3DCALLBACK pCallback, DWORD Context)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1d8c, 2);
    push[1] = (DWORD)pCallback;
    push[2] = Context;
    push[3] = D3DPUSH_ENCODE(0x110, 1);
    push[4] = 0;
    push[5] = D3DPUSH_ENCODE(0x100, 1);
    push[6] = Type == D3DCALLBACK_WRITE ? 7 : 6;
    D3D__PushCommit(push + 7);
}
/* function_13f10 0x1412c: the encoder option is sent only when the filter changes */
static __forceinline void WINAPI D3DDevice_SetFlickerFilter(DWORD Filter)
{
    if (!D3D__FlickerFilterSet || D3D__FlickerFilter != Filter) {
        AvSendTVEncoderOption(*(void **)((BYTE *)D3D__pDevice + 0x1c28), 0xb, Filter, 0);
        D3D__FlickerFilterSet = 1;
        D3D__FlickerFilter = Filter;
    }
}
static __forceinline void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL Enable)
{
    D3DDevice_SetSoftDisplayFilter_Regs(Enable);
}
static __forceinline DWORD WINAPI D3DDevice_Swap(DWORD Flags)
{
    return D3DDevice_Swap_Regs(Flags);
}
static __forceinline void WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **ppZStencilSurface);
static __forceinline void WINAPI D3DDevice_GetBackBuffer(INT BackBuffer, D3DBACKBUFFER_TYPE Type, D3DSurface **ppBackBuffer);
static __forceinline void WINAPI D3DDevice_SetTexture(DWORD Stage, D3DBaseTexture *pTexture)
{
    D3DDevice_SetTexture_Regs(Stage, Stage, pTexture);
}
/* function_12b070: the callback is device +0x1db8 */
static __forceinline void WINAPI D3DDevice_SetVerticalBlankCallback(D3DVBLANKCALLBACK pCallback)
{
    *(D3DVBLANKCALLBACK *)((BYTE *)D3D__pDevice + 0x1db8) = pCallback;
}
/* function_12a50 (flags 0): the display block at device +0x1c28 keeps two ramps at +0x1dc (0x300
   bytes each); when the new ramp differs from the current one (+0x7e4 = device +0x240c) it is
   copied into slot (device +0x2478) & 1, that slot becomes current and the ramp is committed.
   Other flags: no evidence. */
static __forceinline void WINAPI D3DDevice_SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    BYTE *dev = (BYTE *)D3D__pDevice;
    BYTE *display = dev + 0x1c28;
    DWORD slot = *(DWORD *)(dev + 0x2478) & 1;
    const DWORD *cur = (const DWORD *)(display + 0x1dc + *(DWORD *)(dev + 0x240c) * 0x300);
    const DWORD *src = (const DWORD *)pRamp;
    DWORD i;
    (void)Flags;
    for (i = 0; i < 0xc0; i++)
        if (cur[i] != src[i])
            break;
    if (i < 0xc0) {
        DWORD *dst = (DWORD *)(display + 0x1dc + slot * 0x300);
        for (i = 0; i < 0xc0; i++)
            dst[i] = src[i];
        *(DWORD *)(display + 0x7e4) = slot;
        D3D_CommitGammaRamp_Regs(display);
    }
}
static __forceinline DWORD *WINAPI D3DDevice_BeginPush(DWORD Count)
{
    return D3DDevice_BeginPush_Regs(Count);
}
/* function_4b220 0x4b2a1: EndPush only stores the push pointer back into the device */
static __forceinline void WINAPI D3DDevice_EndPush(DWORD *pPush)
{
    D3D__PushCommit(pPush);
}
static __forceinline HRESULT WINAPI D3DDevice_Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    return D3DDevice_Reset_Regs(pPresentationParameters);
}
static __forceinline HRESULT WINAPI Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused, DWORD BehaviorFlags,
                                                          D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDevice **ppReturnedDeviceInterface)
{
    (void)Adapter; (void)DeviceType; (void)pUnused;
    return Direct3D_CreateDevice_Regs(ppReturnedDeviceInterface, BehaviorFlags, pPresentationParameters);
}
static __forceinline D3DTexture *WINAPI D3DDevice_CreateTexture2(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                                 D3DFORMAT Format, D3DRESOURCETYPE D3DResourceType);
static __forceinline D3DPalette *WINAPI D3DDevice_CreatePalette2(D3DPALETTESIZE Size);
/* function_439d0 0x43a1a: method 0x1710 (count 1) with 0 */
static __forceinline void WINAPI D3DDevice_FlushVertexCache(void)
{
    DWORD *push = D3D__PushReserve();
    push[0] = D3DPUSH_ENCODE(0x1710, 1);
    push[1] = 0;
    D3D__PushCommit(push + 2);
}
static __forceinline void WINAPI D3DResource_Register(D3DResource *pThis, void *pBase);
static __forceinline void WINAPI D3DDevice_SetRenderTargetFast(D3DSurface *pRenderTarget, D3DSurface *pNewZStencil, DWORD Flags)
{
    (void)Flags;
    D3DDevice_SetRenderTargetFast_Regs(pNewZStencil, pNewZStencil, pRenderTarget);
}
static __forceinline void WINAPI D3DDevice_SetPalette(DWORD Stage, D3DPalette *pPalette)
{
    D3DDevice_SetPalette_Regs(Stage, Stage, pPalette);
}
static __forceinline D3DSurface *WINAPI D3DTexture_GetSurfaceLevel2(D3DTexture *pThis, UINT Level);
/* the NotInline name expands to the same body in retail (function_18900) */
static __forceinline void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE State, DWORD Value)
{
    D3DDevice_SetRenderState(State, Value);
}
#ifdef __cplusplus
}

/* Resource layout is the public Xbox one (Common, Data, Lock; pixel containers
   add Format, Size; the decomp reads these fields by name). The C++ members
   are one-line forwarders to the C entry points above, the public C++ face of
   the same API; they add no behaviour. */
#define FECHECK_D3D_FWD __forceinline
/* IDirect3DDevice8_Clear: the C-style device call; the device argument is unused on Xbox */
#define IDirect3DDevice8_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil) D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil)
struct D3DResource {
    DWORD Common; DWORD Data; DWORD Lock;
    FECHECK_D3D_FWD ULONG WINAPI AddRef() { return D3DResource_AddRef(this); }
    FECHECK_D3D_FWD ULONG WINAPI Release() { return D3DResource_Release(this); }
    FECHECK_D3D_FWD BOOL WINAPI IsBusy() { return D3DResource_IsBusy(this); }
    FECHECK_D3D_FWD void WINAPI BlockUntilNotBusy() { D3DResource_BlockUntilNotBusy(this); }
    FECHECK_D3D_FWD void WINAPI Register(void *pBase) { D3DResource_Register(this, pBase); }
};
struct D3DPixelContainer : public D3DResource { DWORD Format; DWORD Size; };
struct D3DBaseTexture : public D3DPixelContainer {};
struct D3DTexture : public D3DBaseTexture {
    FECHECK_D3D_FWD D3DSurface *WINAPI GetSurfaceLevel2(UINT Level) { return D3DTexture_GetSurfaceLevel2(this, Level); }
    FECHECK_D3D_FWD HRESULT WINAPI LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
        { D3DTexture_LockRect(this, Level, pLockedRect, pRect, Flags); return S_OK; }
    FECHECK_D3D_FWD HRESULT WINAPI UnlockRect(UINT Level) { D3DTexture_UnlockRect(this, Level); return S_OK; }
};
struct D3DVolumeTexture : public D3DBaseTexture {};
struct D3DCubeTexture : public D3DBaseTexture {};
struct D3DSurface : public D3DPixelContainer {
    D3DBaseTexture *Parent;
    FECHECK_D3D_FWD HRESULT WINAPI LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
        { D3DSurface_LockRect(this, pLockedRect, pRect, Flags); return S_OK; }
    FECHECK_D3D_FWD HRESULT WINAPI UnlockRect() { D3DSurface_UnlockRect(this); return S_OK; }
    FECHECK_D3D_FWD HRESULT WINAPI GetDesc(D3DSURFACE_DESC *pDesc) { D3DSurface_GetDesc(this, pDesc); return S_OK; }
};
struct D3DVolume : public D3DPixelContainer { D3DBaseTexture *Parent; };
struct D3DPalette : public D3DResource {};
struct D3DVertexBuffer : public D3DResource {};
struct D3DIndexBuffer : public D3DResource {};
struct D3DPushBuffer : public D3DResource { DWORD Size; DWORD AllocationSize; };
struct D3DFixup : public D3DResource { DWORD Run; DWORD Next; DWORD Size; };
struct D3DDevice {
    static FECHECK_D3D_FWD void WINAPI SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) { D3DDevice_SetRenderState(State, Value); }
    static FECHECK_D3D_FWD void WINAPI GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) { D3DDevice_GetRenderState(State, pValue); }
    static FECHECK_D3D_FWD void WINAPI SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { D3DDevice_SetTextureStageState(Stage, Type, Value); }
    static FECHECK_D3D_FWD void WINAPI GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) { D3DDevice_GetTextureStageState(Stage, Type, pValue); }
    static FECHECK_D3D_FWD void WINAPI Begin(D3DPRIMITIVETYPE PrimitiveType) { D3DDevice_Begin(PrimitiveType); }
    static FECHECK_D3D_FWD void WINAPI End() { D3DDevice_End(); }
    static FECHECK_D3D_FWD void WINAPI SetVertexData2f(INT Register, float a, float b) { D3DDevice_SetVertexData2f(Register, a, b); }
    static FECHECK_D3D_FWD void WINAPI SetVertexData2s(INT Register, SHORT a, SHORT b) { D3DDevice_SetVertexData2s(Register, a, b); }
    static FECHECK_D3D_FWD void WINAPI SetVertexData4f(INT Register, float a, float b, float c, float d) { D3DDevice_SetVertexData4f(Register, a, b, c, d); }
    static FECHECK_D3D_FWD void WINAPI SetVertexData4ub(INT Register, BYTE a, BYTE b, BYTE c, BYTE d) { D3DDevice_SetVertexData4ub(Register, a, b, c, d); }
    static FECHECK_D3D_FWD void WINAPI SetVertexDataColor(INT Register, D3DCOLOR Color) { D3DDevice_SetVertexDataColor(Register, Color); }
    static FECHECK_D3D_FWD void WINAPI Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil); }
    static FECHECK_D3D_FWD void WINAPI GetBackBuffer(INT BackBuffer, D3DBACKBUFFER_TYPE Type, D3DSurface **ppBackBuffer) { D3DDevice_GetBackBuffer(BackBuffer, Type, ppBackBuffer); }
    static FECHECK_D3D_FWD void WINAPI GetRenderTarget(D3DSurface **ppRenderTarget) { D3DDevice_GetRenderTarget(ppRenderTarget); }
    FECHECK_D3D_FWD ULONG WINAPI Release() { return D3DDevice_Release(); }
    static FECHECK_D3D_FWD void WINAPI GetDepthStencilSurface(D3DSurface **ppZStencilSurface) { D3DDevice_GetDepthStencilSurface(ppZStencilSurface); }
};
/* ---- resources and locks ---- */
/* AddRef as retail inlines it (function_13d80 0x13dd8..0x13dfe): a surface (type bits 0x50000) whose
   own count is 0 and that has a parent texture (+0x14) references the parent first */
static __forceinline ULONG WINAPI D3D__AddRefInline(D3DResource *pThis)
{
    DWORD c = pThis->Common;
    if (!(c & 0xffff) && (c & 0x70000) == 0x50000 && ((DWORD *)pThis)[5])
        D3DResource_AddRef((D3DResource *)((DWORD *)pThis)[5]);
    return ++pThis->Common;
}
/* GetBackBuffer2: 0 = device +0x1a14 (function_13d80 0x13dd2), -1 (front) = +0x1a18 (function_14600
   0x14641), add-ref'd inline; indices above 0: no evidence */
static __forceinline D3DSurface *WINAPI D3DDevice_GetBackBuffer2(INT BackBuffer)
{
    D3DSurface *s = ((D3DSurface **)D3D__pDevice)[0x1a14 / 4 - BackBuffer];
    D3D__AddRefInline((D3DResource *)s);
    return s;
}
static __forceinline void WINAPI D3DSurface_GetDesc(D3DSurface *pThis, D3DSURFACE_DESC *pDesc)
{
    D3DSurface_GetDesc_Regs(pThis, pDesc);
}
/* LockRect as retail inlines it (function_1d5e0 texture, function_1d2f0 / function_13d80 surface,
   function_1d0e0 cube + volume): block unless D3DLOCK_NOOVERWRITE, find the level, flag 0x40 sets
   the write-combined address bits. pRect / pBox: no retail evidence (every decomp call passes NULL). */
static __forceinline void D3D__LockLevel(D3DPixelContainer *pPixels, DWORD FaceType, UINT Level, DWORD Flags,
                                         BYTE **ppBits, DWORD *pRowPitch, DWORD *pSlicePitch)
{
    DWORD slice, out6, out7;
    if (!(Flags & D3DLOCK_NOOVERWRITE))
        D3DResource_BlockUntilNotBusy(pPixels);
    D3D_FindSurfaceWithinTexture(pPixels, FaceType, Level, ppBits, pRowPitch, pSlicePitch ? pSlicePitch : &slice,
                                 &out6, &out7);
    if (Flags & 0x40)
        *ppBits = (BYTE *)((DWORD)*ppBits | 0xf0000000);
}
static __forceinline void WINAPI D3DSurface_LockRect(D3DSurface *pThis, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    BYTE *bits; DWORD pitch;
    (void)pRect;
    D3D__LockLevel(pThis, 0, 0, Flags, &bits, &pitch, 0);
    pLockedRect->Pitch = pitch;
    pLockedRect->pBits = bits;
}
static __forceinline void WINAPI D3DTexture_LockRect(D3DTexture *pThis, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    BYTE *bits; DWORD pitch;
    (void)pRect;
    D3D__LockLevel(pThis, 0, Level, Flags, &bits, &pitch, 0);
    pLockedRect->Pitch = pitch;
    pLockedRect->pBits = bits;
}
static __forceinline void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    BYTE *bits; DWORD pitch;
    (void)pRect;
    D3D__LockLevel(pThis, FaceType, Level, Flags, &bits, &pitch, 0);
    pLockedRect->Pitch = pitch;
    pLockedRect->pBits = bits;
}
static __forceinline void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *pThis, UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
    BYTE *bits; DWORD row, slice;
    (void)pBox;
    D3D__LockLevel(pThis, 0, Level, Flags, &bits, &row, &slice);
    pLockedVolume->RowPitch = row;
    pLockedVolume->SlicePitch = slice;
    pLockedVolume->pBits = bits;
}
/* unlocks are empty in retail (function_13d80 goes straight to Release) */
static __forceinline void WINAPI D3DSurface_UnlockRect(D3DSurface *pThis) { (void)pThis; }
static __forceinline void WINAPI D3DTexture_UnlockRect(D3DTexture *pThis, UINT Level) { (void)pThis; (void)Level; }
static __forceinline void WINAPI D3DCubeTexture_UnlockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level) { (void)pThis; (void)FaceType; (void)Level; }
static __forceinline void WINAPI D3DVolumeTexture_UnlockBox(D3DVolumeTexture *pThis, UINT Level) { (void)pThis; (void)Level; }
/* function_1d3f0: wait on the palette (pending in the push buffer: device +0x2c with flag 2;
   else its Lock time with flag 0x10), then the cached address of its data */
static __forceinline D3DCOLOR *WINAPI D3DPalette_Lock2(D3DPalette *pThis, DWORD Flags)
{
    DWORD *dev = (DWORD *)D3D__pDevice;
    if (!(Flags & D3DLOCK_NOOVERWRITE) && dev) {
        if (pThis->Common & 0x780000)
            D3D_BlockOnTime(dev[0x2c / 4], 2);
        else if (pThis->Lock)
            D3D_BlockOnTime(pThis->Lock, 0x10);
    }
    return (D3DCOLOR *)(pThis->Data | 0x80000000);
}
static __forceinline void WINAPI D3DPalette_Unlock(D3DPalette *pThis) { (void)pThis; }
/* function_14600 (matched): the depth stencil is device +0x1a08 (may be NULL), add-ref'd inline */
static __forceinline void WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **ppZStencilSurface)
{
    D3DSurface *s = *(D3DSurface **)((BYTE *)D3D__pDevice + 0x1a08);
    if (s)
        D3D__AddRefInline((D3DResource *)s);
    *ppZStencilSurface = s;
}
static __forceinline void WINAPI D3DDevice_GetBackBuffer(INT BackBuffer, D3DBACKBUFFER_TYPE Type, D3DSurface **ppBackBuffer)
{
    (void)Type;
    *ppBackBuffer = D3DDevice_GetBackBuffer2(BackBuffer);
}
/* CreateTexture2 as retail inlines it (function_158f0 texture, function_159b0 cube, function_15a60
   volume): 0x3fde00 encodes the format and returns the data size; usage bit 0x10000 clears format
   bit 3; a 0x14-byte header and the data come from XMemAllocDefault; header = refcount 1 |
   0x1040000, data (low 28 bits), lock 0, format, size. */
static __forceinline D3DTexture *WINAPI D3DDevice_CreateTexture2(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                                 D3DFORMAT Format, D3DRESOURCETYPE D3DResourceType)
{
    DWORD format, size, bytes;
    D3DTexture *texture;
    void *data;
    bytes = D3D_InitializeTexture_Regs(Width, Height, Depth, Levels, Format, 0, D3DResourceType == D3DRTYPE_CUBETEXTURE,
                                       D3DResourceType == D3DRTYPE_VOLUMETEXTURE, &format, &size);
    if (Usage & 0x10000)
        format &= ~8;
    texture = (D3DTexture *)XMemAllocDefault(0x14, 0x64800000);
    if (!texture)
        return 0;
    data = XMemAllocDefault(bytes, 0xb7800000);
    if (!data) {
        XMemFreeDefault(texture, 0x24800000);
        return 0;
    }
    texture->Data = (DWORD)data & 0x0fffffff;
    texture->Format = format;
    texture->Common = 0x1040001;
    texture->Size = size;
    texture->Lock = 0;
    return texture;
}
/* function_15b10: a 0xc-byte header, data of D3D__PaletteBytes[size] bytes, header = refcount 1 |
   0x1030000 | size << 30 (the lock word is left as allocated) */
static __forceinline D3DPalette *WINAPI D3DDevice_CreatePalette2(D3DPALETTESIZE Size)
{
    D3DPalette *palette = (D3DPalette *)XMemAllocDefault(0xc, 0x64800000);
    void *data;
    if (!palette)
        return 0;
    data = XMemAllocDefault(D3D__PaletteBytes[Size], 0xb6800000);
    if (!data) {
        XMemFreeDefault(palette, 0x24800000);
        return 0;
    }
    palette->Common = ((DWORD)Size << 30) | 0x1030001;
    palette->Data = (DWORD)data & 0x0fffffff;
    return palette;
}
/* function_1df20 0x1dfff: the base is added to Data, which keeps only its low 28 bits unless the
   resource type bits are 0x20000 */
static __forceinline void WINAPI D3DResource_Register(D3DResource *pThis, void *pBase)
{
    DWORD data = pThis->Data + (DWORD)pBase;
    if ((pThis->Common & 0x70000) != 0x20000)
        data &= 0x0fffffff;
    pThis->Data = data;
}
/* function_14850 0x14873: find the level, then wrap it in a surface whose parent is the texture */
static __forceinline D3DSurface *WINAPI D3DTexture_GetSurfaceLevel2(D3DTexture *pThis, UINT Level)
{
    BYTE *data; DWORD pitch, slice, format, size;
    D3D_FindSurfaceWithinTexture(pThis, 0, Level, &data, &pitch, &slice, &format, &size);
    return D3D_CreateSurface_Regs((DWORD)data, pThis, format, size);
}
#endif
#endif
