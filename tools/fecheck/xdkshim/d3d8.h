/* d3d8.h stand-in for fecheck builds (VS2022 x86, no XDK).
   Declarations only, written from the decomp's own usage plus the public
   Direct3D 8 API shape and public NV2A hardware facts. Never copied from an
   SDK header. The Xbox header inlines part of its API into push-buffer
   writes; here every such function is a plain extern, so a source that
   leans on an inline body builds but may not match retail.

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
   - D3DTSS_* indices are NOT anchored by decomp evidence: unverified.
   - D3DPUSH_* are public NV2A method numbers (SET_BEGIN_END 0x17fc, INLINE_ARRAY
     0x1818, non-increment bit 0x40000000, count at bit 18).
   - Calling convention: every D3D entry point is WINAPI except
     D3DDevice_SetRenderState_Simple (__fastcall); unverified. */
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

typedef enum _D3DTEXTURESTAGESTATETYPE {   /* all indices unverified */
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
#define D3DVSDE_VERTEX 0xffffffff                          /* unverified */
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

void WINAPI D3DDevice_SetRenderState(D3DRENDERSTATETYPE State, DWORD Value);
void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE State, DWORD Value);
void __fastcall D3DDevice_SetRenderState_Simple(DWORD Method, DWORD Value);
void WINAPI D3DDevice_SetRenderState_ZEnable(DWORD Value);
void WINAPI D3DDevice_GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue);
void WINAPI D3DDevice_SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
void WINAPI D3DDevice_GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue);
void WINAPI D3DDevice_SetTexture(DWORD Stage, D3DBaseTexture *pTexture);
void WINAPI D3DDevice_SetPalette(DWORD Stage, D3DPalette *pPalette);
void WINAPI D3DDevice_SetPixelShader(DWORD Handle);
void WINAPI D3DDevice_SetPixelShaderProgram(const D3DPIXELSHADERDEF *pPSDef);
void WINAPI D3DDevice_SetVertexShader(DWORD Handle);
void WINAPI D3DDevice_SetVertexShaderConstant(INT Register, const void *pConstantData, DWORD ConstantCount);
void WINAPI D3DDevice_SetVertexShaderConstantFast(INT Register, const void *pConstantData, DWORD ConstantCount);
void WINAPI D3DDevice_SetVertexShaderConstantNotInline(INT Register, const void *pConstantData, DWORD ConstantCount);
void WINAPI D3DDevice_LoadVertexShaderProgram(const DWORD *pFunction, DWORD Address);
void WINAPI D3DDevice_SelectVertexShaderDirect(D3DVERTEXATTRIBUTEFORMAT *pVAF, DWORD Address);
void WINAPI D3DDevice_SetVertexShaderInputDirect(D3DVERTEXATTRIBUTEFORMAT *pVAF, UINT StreamCount, D3DSTREAM_INPUT *pStreamInputs);
void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *pIndexData, UINT BaseVertexIndex);
void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE PrimitiveType);
void WINAPI D3DDevice_End(void);
void WINAPI D3DDevice_SetVertexData2f(INT Register, float a, float b);
void WINAPI D3DDevice_SetVertexData2s(INT Register, SHORT a, SHORT b);
void WINAPI D3DDevice_SetVertexData4f(INT Register, float a, float b, float c, float d);
void WINAPI D3DDevice_SetVertexData4ub(INT Register, BYTE a, BYTE b, BYTE c, BYTE d);
void WINAPI D3DDevice_SetVertexData4s(INT Register, SHORT a, SHORT b, SHORT c, SHORT d);
void WINAPI D3DDevice_SetVertexDataColor(INT Register, D3DCOLOR Color);
void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT VertexCount);
void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE PrimitiveType, UINT VertexCount, const WORD *pIndexData);
void WINAPI D3DDevice_Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil);
void WINAPI D3DDevice_SetScissors(DWORD Count, BOOL Exclusive, const D3DRECT *pRects);
void WINAPI D3DDevice_SetDepthClipPlanes(float Near, float Far, DWORD Flags);
void WINAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *pViewport);
void WINAPI D3DDevice_GetViewport(D3DVIEWPORT8 *pViewport);
void WINAPI D3DDevice_SetStipple(const DWORD *pPattern);
void WINAPI D3DDevice_SetRenderTarget(D3DSurface *pRenderTarget, D3DSurface *pNewZStencil);
void WINAPI D3DDevice_SetRenderTargetFast(D3DSurface *pRenderTarget, D3DSurface *pNewZStencil, DWORD Flags);
void WINAPI D3DDevice_GetBackBuffer(INT BackBuffer, D3DBACKBUFFER_TYPE Type, D3DSurface **ppBackBuffer);
D3DSurface *WINAPI D3DDevice_GetBackBuffer2(INT BackBuffer);
void WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **ppZStencilSurface);
void WINAPI D3DDevice_GetRenderTarget(D3DSurface **ppRenderTarget);
void WINAPI D3DDevice_SetBackBufferScale(float x, float y);
void WINAPI D3DDevice_SetFlickerFilter(DWORD Filter);
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL Enable);
void WINAPI D3DDevice_SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp);
void WINAPI D3DDevice_SetVerticalBlankCallback(D3DVBLANKCALLBACK pCallback);
void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE Type, D3DCALLBACK pCallback, DWORD Context);
HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD Index);
HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD Index, UINT *pResult, ULONGLONG *pTimeStamp);
DWORD WINAPI D3DDevice_Swap(DWORD Flags);
HRESULT WINAPI D3DDevice_Reset(D3DPRESENT_PARAMETERS *pPresentationParameters);
HRESULT WINAPI D3DDevice_PersistDisplay(void);
ULONG WINAPI D3DDevice_Release(void);
HRESULT WINAPI Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused, DWORD BehaviorFlags,
                                     D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDevice **ppReturnedDeviceInterface);
void WINAPI Direct3D_SetPushBufferSize(DWORD PushBufferSize, DWORD KickOffSize);
BOOL WINAPI D3DDevice_IsBusy(void);
void WINAPI D3DDevice_KickPushBuffer(void);
void WINAPI D3DDevice_FlushVertexCache(void);
DWORD *WINAPI D3DDevice_BeginPush(DWORD Count);
void WINAPI D3DDevice_EndPush(DWORD *pPush);
DWORD *WINAPI D3DDevice_MakeSpace(void);
D3DTexture *WINAPI D3DDevice_CreateTexture2(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                            D3DFORMAT Format, D3DRESOURCETYPE D3DResourceType);
D3DPalette *WINAPI D3DDevice_CreatePalette2(D3DPALETTESIZE Size);

ULONG WINAPI D3DResource_AddRef(D3DResource *pThis);
ULONG WINAPI D3DResource_Release(D3DResource *pThis);
BOOL WINAPI D3DResource_IsBusy(D3DResource *pThis);
void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *pThis);
void WINAPI D3DResource_Register(D3DResource *pThis, void *pBase);
D3DCOLOR *WINAPI D3DPalette_Lock2(D3DPalette *pThis, DWORD Flags);
void WINAPI D3DPalette_Unlock(D3DPalette *pThis);
void WINAPI D3DSurface_GetDesc(D3DSurface *pThis, D3DSURFACE_DESC *pDesc);
void WINAPI D3DSurface_LockRect(D3DSurface *pThis, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
void WINAPI D3DSurface_UnlockRect(D3DSurface *pThis);
D3DSurface *WINAPI D3DTexture_GetSurfaceLevel2(D3DTexture *pThis, UINT Level);
void WINAPI D3DTexture_LockRect(D3DTexture *pThis, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
void WINAPI D3DTexture_UnlockRect(D3DTexture *pThis, UINT Level);
void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
void WINAPI D3DCubeTexture_UnlockRect(D3DCubeTexture *pThis, D3DCUBEMAP_FACES FaceType, UINT Level);
void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *pThis, UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags);
void WINAPI D3DVolumeTexture_UnlockBox(D3DVolumeTexture *pThis, UINT Level);
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
#endif
#endif
