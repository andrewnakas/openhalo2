/* d3d8.h stand-in for fecheck builds: opaque Xbox Direct3D types only.
   No render-state numbers and no functions: the Xbox header inlines much of
   its API into push-buffer writes, which a declaration cannot reproduce, so
   a source that calls Direct3D stays "unbuildable" in fecheck. */
#pragma once
#ifndef FECHECK_D3D8_H
#define FECHECK_D3D8_H
struct D3DDevice; struct D3DResource; struct D3DBaseTexture; struct D3DTexture; struct D3DVolumeTexture;
struct D3DCubeTexture; struct D3DSurface; struct D3DVolume; struct D3DPalette; struct D3DVertexBuffer;
struct D3DIndexBuffer; struct D3DPushBuffer; struct D3DFixup; struct D3DPixelContainer;
typedef struct D3DDevice D3DDevice; typedef struct D3DResource D3DResource; typedef struct D3DBaseTexture D3DBaseTexture;
typedef struct D3DTexture D3DTexture; typedef struct D3DVolumeTexture D3DVolumeTexture; typedef struct D3DCubeTexture D3DCubeTexture;
typedef struct D3DSurface D3DSurface; typedef struct D3DVolume D3DVolume; typedef struct D3DPalette D3DPalette;
typedef struct D3DVertexBuffer D3DVertexBuffer; typedef struct D3DIndexBuffer D3DIndexBuffer;
typedef struct D3DPushBuffer D3DPushBuffer; typedef struct D3DPixelContainer D3DPixelContainer;
typedef D3DDevice *LPDIRECT3DDEVICE8; typedef D3DTexture *LPDIRECT3DTEXTURE8; typedef D3DSurface *LPDIRECT3DSURFACE8;
#endif
