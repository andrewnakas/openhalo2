/* dsound.h stand-in for fecheck builds (VS2022 x86, no XDK), included by xtl.h.
   Declarations only, from the decomp's usage (field and method names) and the
   public shape of Xbox DirectSound. Never copied from an SDK header.
   ALL structure layouts and constant values here are unverified except
   where a decomp size assert passes through them (DSCAPS sits in
   s_sound_driver_globals, whose size and member offsets are asserted).
   C++ members: XMediaObject's are virtual (codecs implement that interface);
   the IDirectSound / IDirectSoundBuffer / IDirectSoundStream members are
   one-line forwarders to the C entry points. */
#pragma once
#ifndef FECHECK_DSOUND_H
#define FECHECK_DSOUND_H

#ifndef _WAVEFORMATEX_
#define _WAVEFORMATEX_
typedef struct tWAVEFORMATEX { WORD wFormatTag; WORD nChannels; DWORD nSamplesPerSec; DWORD nAvgBytesPerSec;
    WORD nBlockAlign; WORD wBitsPerSample; WORD cbSize; } WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
#endif
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif
#define WAVE_FORMAT_XBOX_ADPCM 0x0069   /* public Xbox ADPCM format tag */
typedef struct xbox_adpcmwaveformat_tag { WAVEFORMATEX wfx; WORD wSamplesPerBlock; } XBOXADPCMWAVEFORMAT, *PXBOXADPCMWAVEFORMAT, *LPXBOXADPCMWAVEFORMAT;

typedef void *FECHECK_LPUNKNOWN;
typedef LONGLONG REFERENCE_TIME, *PREFERENCE_TIME, *LPREFERENCE_TIME;
typedef struct _XMEDIAPACKET {
    LPVOID pvBuffer;
    DWORD dwMaxSize;
    PDWORD pdwCompletedSize;
    PDWORD pdwStatus;
    union { HANDLE hCompletionEvent; PVOID pContext; };
    PREFERENCE_TIME prtTimestamp;
} XMEDIAPACKET, *PXMEDIAPACKET, *LPXMEDIAPACKET;
typedef const XMEDIAPACKET *LPCXMEDIAPACKET;
#define XMEDIAPACKET_STATUS_SUCCESS 0x00000000   /* unverified */
#define XMEDIAPACKET_STATUS_PENDING 0x8000000a   /* unverified (E_PENDING) */
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004   /* unverified */
typedef struct _XMEDIAINFO { DWORD dwFlags; DWORD dwInputSize; DWORD dwOutputSize; DWORD dwMaxLookahead; } XMEDIAINFO, *LPXMEDIAINFO;
typedef void (CALLBACK *LPFNXMEDIAOBJECTCALLBACK)(LPVOID pStreamContext, LPVOID pPacketContext, DWORD dwStatus);

typedef struct _DSMIXBINVOLUMEPAIR { DWORD dwMixBin; LONG lVolume; } DSMIXBINVOLUMEPAIR, *LPDSMIXBINVOLUMEPAIR;
typedef const DSMIXBINVOLUMEPAIR *LPCDSMIXBINVOLUMEPAIR;
typedef struct _DSMIXBINS { DWORD dwMixBinCount; LPCDSMIXBINVOLUMEPAIR lpMixBinVolumePairs; } DSMIXBINS, *LPDSMIXBINS;
typedef const DSMIXBINS *LPCDSMIXBINS;
typedef struct _DSCAPS { DWORD dwFree2DBuffers; DWORD dwFree3DBuffers; DWORD dwFreeBufferSGEs; DWORD dwMemoryAllocated; } DSCAPS, *LPDSCAPS;
typedef struct _DSBUFFERDESC { DWORD dwSize; DWORD dwFlags; DWORD dwBufferBytes; LPWAVEFORMATEX lpwfxFormat;
    LPCDSMIXBINS lpMixBins; DWORD dwInputMixBin; } DSBUFFERDESC, *LPDSBUFFERDESC;
typedef const DSBUFFERDESC *LPCDSBUFFERDESC;
typedef struct _DSSTREAMDESC { DWORD dwFlags; DWORD dwMaxAttachedPackets; LPWAVEFORMATEX lpwfxFormat;
    LPFNXMEDIAOBJECTCALLBACK lpfnCallback; LPVOID lpvContext; LPCDSMIXBINS lpMixBins; } DSSTREAMDESC, *LPDSSTREAMDESC;
typedef const DSSTREAMDESC *LPCDSSTREAMDESC;
typedef struct _DSENVELOPEDESC { DWORD dwEG; DWORD dwMode; DWORD dwDelay; DWORD dwAttack; DWORD dwHold; DWORD dwDecay;
    DWORD dwRelease; DWORD dwSustain; LONG lPitchScale; LONG lFilterCutOff; } DSENVELOPEDESC, *LPDSENVELOPEDESC;
typedef const DSENVELOPEDESC *LPCDSENVELOPEDESC;
typedef struct _DSFILTERDESC { DWORD dwMode; DWORD dwQCoefficient; DWORD adwCoefficients[4]; } DSFILTERDESC, *LPDSFILTERDESC;
typedef const DSFILTERDESC *LPCDSFILTERDESC;
typedef struct _DSLFODESC { DWORD dwLFO; DWORD dwDelay; DWORD dwDelta; LONG lPitchModulation; LONG lFilterCutOffRange;
    LONG lAmplitudeModulation; } DSLFODESC, *LPDSLFODESC;
typedef const DSLFODESC *LPCDSLFODESC;
typedef struct _DSI3DL2OBSTRUCTION { LONG lHFLevel; FLOAT flLFRatio; } DSI3DL2OBSTRUCTION;
typedef struct _DSI3DL2OCCLUSION { LONG lHFLevel; FLOAT flLFRatio; } DSI3DL2OCCLUSION;
typedef struct _DSI3DL2BUFFER { LONG lDirect; LONG lDirectHF; LONG lRoom; LONG lRoomHF; FLOAT flRoomRolloffFactor;
    DSI3DL2OBSTRUCTION Obstruction; DSI3DL2OCCLUSION Occlusion; } DSI3DL2BUFFER, *LPDSI3DL2BUFFER;
typedef const DSI3DL2BUFFER *LPCDSI3DL2BUFFER;
typedef struct _DSI3DL2LISTENER { LONG lRoom; LONG lRoomHF; FLOAT flRoomRolloffFactor; FLOAT flDecayTime; FLOAT flDecayHFRatio;
    LONG lReflections; FLOAT flReflectionsDelay; LONG lReverb; FLOAT flReverbDelay; FLOAT flDiffusion; FLOAT flDensity;
    FLOAT flHFReference; } DSI3DL2LISTENER, *LPDSI3DL2LISTENER;
typedef struct _DSVOICEPROPS { DWORD dwMixBinCount; DSMIXBINVOLUMEPAIR MixBinVolumePairs[8]; LONG lPitch; LONG l3DDistanceVolume;
    LONG l3DConeVolume; LONG l3DDopplerPitch; LONG lI3DL2DirectVolume; LONG lI3DL2RoomVolume; } DSVOICEPROPS, *LPDSVOICEPROPS;
typedef struct _DSEFFECTMAP { LPVOID lpvCodeSegment; DWORD dwCodeSize; LPVOID lpvStateSegment; DWORD dwStateSize;
    LPVOID lpvYMemorySegment; DWORD dwYMemorySize; LPVOID lpvScratchSegment; DWORD dwScratchSize; } DSEFFECTMAP;
typedef struct _DSEFFECTIMAGEDESC { DWORD dwEffectCount; DWORD dwTotalScratchSize; DSEFFECTMAP aEffectMaps[1]; } DSEFFECTIMAGEDESC, *LPDSEFFECTIMAGEDESC;
typedef struct _DSEFFECTIMAGELOC { DWORD dwI3DL2ReverbIndex; DWORD dwCrosstalkIndex; } DSEFFECTIMAGELOC, *LPDSEFFECTIMAGELOC;
typedef const DSEFFECTIMAGELOC *LPCDSEFFECTIMAGELOC;
typedef struct _DSFX_HIGH_LEVEL_EFFECT_DESCRIPTION { DWORD effectType; DSI3DL2LISTENER I3DL2Reverb; } DSFX_HIGH_LEVEL_EFFECT_DESCRIPTION, *LPDSFX_HIGH_LEVEL_EFFECT_DESCRIPTION;
typedef const DSFX_HIGH_LEVEL_EFFECT_DESCRIPTION *LPCDSFX_HIGH_LEVEL_EFFECT_DESCRIPTION;

/* constants: all unverified */
#define DSBCAPS_CTRL3D 0x00000010
#define DSBCAPS_MIXIN 0x00002000
#define DSBCAPS_FXIN 0x00080000
#define DSBCAPS_FXIN2 0x00100000
#define DSBPAUSE_RESUME 0x00000000
#define DSBPAUSE_PAUSE 0x00000001
#define DSBPAUSE_SYNCHPLAYBACK 0x00000002
#define DSBSTATUS_PLAYING 0x00000001
#define DSSTREAMPAUSE_RESUME 0x00000000
#define DSSTREAMPAUSE_PAUSE 0x00000001
#define DSSTREAMPAUSE_SYNCHPLAYBACK 0x00000002
#define DSSTREAMSTATUS_READY 0x00000001
#define DSSTREAMSTATUS_PLAYING 0x00010000
#define DSSTREAMSTATUS_PAUSED 0x00020000
#define DSSTREAMFLUSHEX_IMMEDIATE 0x00000000
#define DSSTREAMFLUSHEX_ASYNC 0x00000001
#define DSSTREAMFLUSHEX_ENVELOPE 0x00000002
#define DS3D_IMMEDIATE 0x00000000
#define DS3D_DEFERRED 0x00000001
#define DSFX_IMMEDIATE 0x00000000
#define DSFX_DEFERRED 0x00000001
#define DSEG_AMPLITUDE 0x00000000
#define DSEG_MODE_DISABLE 0x00000000
#define DSEG_MODE_DELAY 0x00000001
#define DSEG_MODE_ATTACK 0x00000002
#define DSFILTER_MODE_BYPASS 0x00000000
#define DSFILTER_MODE_DLS2 0x00000001
#define DSFILTER_MODE_PARAMEQ 0x00000002
#define DSFILTER_MODE_MULTI 0x00000003
#define DSFX_EFFECT_TYPE_I3DL2REVERB 0x00000000
#define XAUDIO_DOWNLOADFX_XBESECTION 0x00000001

#ifdef __cplusplus
struct XMediaObject;
struct IDirectSound; struct IDirectSoundBuffer; struct IDirectSoundStream;
#else
typedef struct XMediaObject XMediaObject;
typedef struct IDirectSound IDirectSound; typedef struct IDirectSoundBuffer IDirectSoundBuffer; typedef struct IDirectSoundStream IDirectSoundStream;
#endif
typedef XMediaObject *LPXMEDIAOBJECT;
typedef IDirectSound *LPDIRECTSOUND, *LPDIRECTSOUND8;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER, *LPDIRECTSOUNDBUFFER8;
typedef IDirectSoundStream *LPDIRECTSOUNDSTREAM;

#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI DirectSoundCreate(LPGUID pGuid, LPDIRECTSOUND *ppDirectSound, FECHECK_LPUNKNOWN pUnkOuter);
HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC pdsbd, LPDIRECTSOUNDBUFFER *ppBuffer);
HRESULT WINAPI DirectSoundCreateStream(LPCDSSTREAMDESC pdssd, LPDIRECTSOUNDSTREAM *ppStream);
void WINAPI DirectSoundDoWork(void);
HRESULT WINAPI XAudioDownloadEffectsImage(LPCSTR pszImageName, LPCDSEFFECTIMAGELOC pEffectLoc, DWORD dwFlags, LPDSEFFECTIMAGEDESC *ppImageDesc);
HRESULT WINAPI XAudioSetEffectData(DWORD dwEffectIndex, LPCDSFX_HIGH_LEVEL_EFFECT_DESCRIPTION pDesc, LPVOID pReserved);
HRESULT WINAPI WmaCreateInMemoryDecoder(void *pfnCallback, LPVOID pvContext, DWORD dwYieldRate, LPWAVEFORMATEX pwfxCompressed, LPXMEDIAOBJECT *ppMediaObject);

ULONG WINAPI IDirectSound_AddRef(LPDIRECTSOUND pThis);
ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND pThis);
HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND pThis, LPDSCAPS pdsc);
HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND pThis, LPCDSBUFFERDESC pdsbd, LPDIRECTSOUNDBUFFER *ppBuffer, FECHECK_LPUNKNOWN pUnkOuter);
HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND pThis, LPCDSSTREAMDESC pdssd, LPDIRECTSOUNDSTREAM *ppStream, FECHECK_LPUNKNOWN pUnkOuter);
HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND pThis, LPDWORD pdwSpeakerConfig);
HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND pThis, FLOAT flDistanceFactor, DWORD dwApply);
HRESULT WINAPI IDirectSound_SetDopplerFactor(LPDIRECTSOUND pThis, FLOAT flDopplerFactor, DWORD dwApply);
HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND pThis, FLOAT flRolloffFactor, DWORD dwApply);
HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND pThis, FLOAT xFront, FLOAT yFront, FLOAT zFront, FLOAT xTop, FLOAT yTop, FLOAT zTop, DWORD dwApply);
HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND pThis, FLOAT x, FLOAT y, FLOAT z, DWORD dwApply);
HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND pThis, FLOAT x, FLOAT y, FLOAT z, DWORD dwApply);
HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND pThis);
HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND pThis, DWORD dwMixBin, DWORD dwHeadroom);
HRESULT WINAPI IDirectSound_SetEffectData(LPDIRECTSOUND pThis, DWORD dwEffectIndex, DWORD dwOffset, LPCVOID pvData, DWORD dwDataSize, DWORD dwApply);
HRESULT WINAPI IDirectSound_GetEffectData(LPDIRECTSOUND pThis, DWORD dwEffectIndex, DWORD dwOffset, LPVOID pvData, DWORD dwDataSize);
HRESULT WINAPI IDirectSound_SynchPlayback(LPDIRECTSOUND pThis);

ULONG WINAPI IDirectSoundBuffer_AddRef(LPDIRECTSOUNDBUFFER pThis);
ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER pThis);
HRESULT WINAPI IDirectSoundBuffer_GetStatus(LPDIRECTSOUNDBUFFER pThis, LPDWORD pdwStatus);
HRESULT WINAPI IDirectSoundBuffer_Lock(LPDIRECTSOUNDBUFFER pThis, DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID *ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags);
HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER pThis, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags);
HRESULT WINAPI IDirectSoundBuffer_Pause(LPDIRECTSOUNDBUFFER pThis, DWORD dwPause);
HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER pThis);
HRESULT WINAPI IDirectSoundBuffer_StopEx(LPDIRECTSOUNDBUFFER pThis, REFERENCE_TIME rtTimeStamp, DWORD dwFlags);
HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER pThis, LPVOID pvBufferData, DWORD dwBufferBytes);
HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER pThis, DWORD dwPlayCursor);
HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER pThis, LONG lVolume);
HRESULT WINAPI IDirectSoundBuffer_SetHeadroom(LPDIRECTSOUNDBUFFER pThis, DWORD dwHeadroom);
HRESULT WINAPI IDirectSoundBuffer_SetMixBins(LPDIRECTSOUNDBUFFER pThis, LPCDSMIXBINS pMixBins);
HRESULT WINAPI IDirectSoundBuffer_SetMixBinVolumes(LPDIRECTSOUNDBUFFER pThis, LPCDSMIXBINS pMixBins);
HRESULT WINAPI IDirectSoundBuffer_SetFilter(LPDIRECTSOUNDBUFFER pThis, LPCDSFILTERDESC pFilterDesc);
HRESULT WINAPI IDirectSoundBuffer_SetI3DL2Source(LPDIRECTSOUNDBUFFER pThis, LPCDSI3DL2BUFFER pds3db, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetPosition(LPDIRECTSOUNDBUFFER pThis, FLOAT x, FLOAT y, FLOAT z, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetMinDistance(LPDIRECTSOUNDBUFFER pThis, FLOAT flMinDistance, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetMaxDistance(LPDIRECTSOUNDBUFFER pThis, FLOAT flMaxDistance, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetRolloffFactor(LPDIRECTSOUNDBUFFER pThis, FLOAT flRolloffFactor, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetDopplerFactor(LPDIRECTSOUNDBUFFER pThis, FLOAT flDopplerFactor, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_SetConeOutsideVolume(LPDIRECTSOUNDBUFFER pThis, LONG lConeOutsideVolume, DWORD dwApply);
HRESULT WINAPI IDirectSoundBuffer_GetVoiceProperties(LPDIRECTSOUNDBUFFER pThis, LPDSVOICEPROPS pVoiceProps);

HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM pThis, LONG lVolume);
HRESULT WINAPI IDirectSoundStream_SetPitch(LPDIRECTSOUNDSTREAM pThis, LONG lPitch);
HRESULT WINAPI IDirectSoundStream_SetLFO(LPDIRECTSOUNDSTREAM pThis, LPCDSLFODESC pLFODesc);
HRESULT WINAPI IDirectSoundStream_SetEG(LPDIRECTSOUNDSTREAM pThis, LPCDSENVELOPEDESC pEnvelopeDesc);
HRESULT WINAPI IDirectSoundStream_SetFilter(LPDIRECTSOUNDSTREAM pThis, LPCDSFILTERDESC pFilterDesc);
HRESULT WINAPI IDirectSoundStream_SetHeadroom(LPDIRECTSOUNDSTREAM pThis, DWORD dwHeadroom);
HRESULT WINAPI IDirectSoundStream_SetOutputBuffer(LPDIRECTSOUNDSTREAM pThis, LPDIRECTSOUNDBUFFER pOutputBuffer);
HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM pThis, LPCDSMIXBINS pMixBins);
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM pThis, LPCDSMIXBINS pMixBins);
HRESULT WINAPI IDirectSoundStream_Pause(LPDIRECTSOUNDSTREAM pThis, DWORD dwPause);
HRESULT WINAPI IDirectSoundStream_FlushEx(LPDIRECTSOUNDSTREAM pThis, REFERENCE_TIME rtTimeStamp, DWORD dwFlags);
HRESULT WINAPI IDirectSoundStream_GetVoiceProperties(LPDIRECTSOUNDSTREAM pThis, LPDSVOICEPROPS pVoiceProps);
#ifdef __cplusplus
}

struct XMediaObject {
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInfo(LPXMEDIAINFO pInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD pdwStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE Process(LPCXMEDIAPACKET pInputPacket, LPCXMEDIAPACKET pOutputPacket) = 0;
    virtual HRESULT STDMETHODCALLTYPE Discontinuity() = 0;
    virtual HRESULT STDMETHODCALLTYPE Flush() = 0;
};
#define FECHECK_DS_FWD __forceinline HRESULT STDMETHODCALLTYPE
struct IDirectSoundStream : public XMediaObject {
    FECHECK_DS_FWD SetVolume(LONG v) { return IDirectSoundStream_SetVolume(this, v); }
    FECHECK_DS_FWD SetPitch(LONG v) { return IDirectSoundStream_SetPitch(this, v); }
    FECHECK_DS_FWD SetLFO(LPCDSLFODESC d) { return IDirectSoundStream_SetLFO(this, d); }
    FECHECK_DS_FWD SetEG(LPCDSENVELOPEDESC d) { return IDirectSoundStream_SetEG(this, d); }
    FECHECK_DS_FWD SetFilter(LPCDSFILTERDESC d) { return IDirectSoundStream_SetFilter(this, d); }
    FECHECK_DS_FWD SetHeadroom(DWORD v) { return IDirectSoundStream_SetHeadroom(this, v); }
    FECHECK_DS_FWD SetOutputBuffer(LPDIRECTSOUNDBUFFER b) { return IDirectSoundStream_SetOutputBuffer(this, b); }
    FECHECK_DS_FWD SetMixBins(LPCDSMIXBINS m) { return IDirectSoundStream_SetMixBins(this, m); }
    FECHECK_DS_FWD SetMixBinVolumes(LPCDSMIXBINS m) { return IDirectSoundStream_SetMixBinVolumes(this, m); }
    FECHECK_DS_FWD Pause(DWORD v) { return IDirectSoundStream_Pause(this, v); }
    FECHECK_DS_FWD FlushEx(REFERENCE_TIME t, DWORD f) { return IDirectSoundStream_FlushEx(this, t, f); }
    FECHECK_DS_FWD GetVoiceProperties(LPDSVOICEPROPS p) { return IDirectSoundStream_GetVoiceProperties(this, p); }
};
struct IDirectSoundBuffer {
    __forceinline ULONG STDMETHODCALLTYPE AddRef() { return IDirectSoundBuffer_AddRef(this); }
    __forceinline ULONG STDMETHODCALLTYPE Release() { return IDirectSoundBuffer_Release(this); }
    FECHECK_DS_FWD GetStatus(LPDWORD s) { return IDirectSoundBuffer_GetStatus(this, s); }
    FECHECK_DS_FWD Play(DWORD r1, DWORD r2, DWORD f) { return IDirectSoundBuffer_Play(this, r1, r2, f); }
    FECHECK_DS_FWD Pause(DWORD v) { return IDirectSoundBuffer_Pause(this, v); }
    FECHECK_DS_FWD Stop() { return IDirectSoundBuffer_Stop(this); }
    FECHECK_DS_FWD StopEx(REFERENCE_TIME t, DWORD f) { return IDirectSoundBuffer_StopEx(this, t, f); }
    FECHECK_DS_FWD SetMinDistance(FLOAT d, DWORD a) { return IDirectSoundBuffer_SetMinDistance(this, d, a); }
    FECHECK_DS_FWD SetMaxDistance(FLOAT d, DWORD a) { return IDirectSoundBuffer_SetMaxDistance(this, d, a); }
    FECHECK_DS_FWD SetVolume(LONG v) { return IDirectSoundBuffer_SetVolume(this, v); }
    FECHECK_DS_FWD SetHeadroom(DWORD v) { return IDirectSoundBuffer_SetHeadroom(this, v); }
    FECHECK_DS_FWD SetMixBins(LPCDSMIXBINS m) { return IDirectSoundBuffer_SetMixBins(this, m); }
    FECHECK_DS_FWD SetMixBinVolumes(LPCDSMIXBINS m) { return IDirectSoundBuffer_SetMixBinVolumes(this, m); }
    FECHECK_DS_FWD SetFilter(LPCDSFILTERDESC d) { return IDirectSoundBuffer_SetFilter(this, d); }
    FECHECK_DS_FWD GetVoiceProperties(LPDSVOICEPROPS p) { return IDirectSoundBuffer_GetVoiceProperties(this, p); }
};
struct IDirectSound {
    __forceinline ULONG STDMETHODCALLTYPE AddRef() { return IDirectSound_AddRef(this); }
    __forceinline ULONG STDMETHODCALLTYPE Release() { return IDirectSound_Release(this); }
    FECHECK_DS_FWD GetCaps(LPDSCAPS c) { return IDirectSound_GetCaps(this, c); }
    FECHECK_DS_FWD CreateSoundBuffer(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *b, FECHECK_LPUNKNOWN u) { return IDirectSound_CreateSoundBuffer(this, d, b, u); }
    FECHECK_DS_FWD CreateSoundStream(LPCDSSTREAMDESC d, LPDIRECTSOUNDSTREAM *s, FECHECK_LPUNKNOWN u) { return IDirectSound_CreateSoundStream(this, d, s, u); }
    FECHECK_DS_FWD GetSpeakerConfig(LPDWORD c) { return IDirectSound_GetSpeakerConfig(this, c); }
    FECHECK_DS_FWD SetDistanceFactor(FLOAT f, DWORD a) { return IDirectSound_SetDistanceFactor(this, f, a); }
    FECHECK_DS_FWD SetDopplerFactor(FLOAT f, DWORD a) { return IDirectSound_SetDopplerFactor(this, f, a); }
    FECHECK_DS_FWD SetRolloffFactor(FLOAT f, DWORD a) { return IDirectSound_SetRolloffFactor(this, f, a); }
    FECHECK_DS_FWD CommitDeferredSettings() { return IDirectSound_CommitDeferredSettings(this); }
    FECHECK_DS_FWD SetMixBinHeadroom(DWORD m, DWORD h) { return IDirectSound_SetMixBinHeadroom(this, m, h); }
};
#endif
#endif
