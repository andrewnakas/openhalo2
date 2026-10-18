/* xhv.h stand-in for fecheck builds (VS2022 x86, no XDK): the voice engine.
   Declarations only, from the decomp's usage (method and field names) and the
   public shape of the XHV API. Never copied from an SDK header.
   UNVERIFIED: every layout, constant and the XHVEngine virtual-method order
   (vtable offsets therefore may not match retail). ITitleXHV's order follows
   the order include/network_voice.h overrides it in. */
#pragma once
#ifndef FECHECK_XHV_H
#define FECHECK_XHV_H
#include "xtl.h"
#ifndef STDMETHOD
#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method
#endif

typedef DWORD XHV_PROCESSING_MODE, *PXHV_PROCESSING_MODE;
typedef DWORD XHV_PLAYBACK_PRIORITY;
typedef DWORD XHV_VOICE_COMMUNICATOR_STATUS;
#define XHV_VOICE_COMMUNICATOR_STATUS_INSERTED 0   /* unverified */
#define XHV_VOICE_COMMUNICATOR_STATUS_REMOVED 1    /* unverified */
#define XHV_VOICE_MASK_PARAM_DISABLED (-1.0f)      /* unverified */

typedef struct _XHV_VOICE_MASK { FLOAT fSpecEnergyWeight; FLOAT fPitchScale; FLOAT fWhisperValue; FLOAT fRoboticValue; } XHV_VOICE_MASK;
#ifdef __cplusplus
extern "C" {
#endif
extern const XHV_VOICE_MASK XHV_VOICE_MASK_NONE;        /* a constant initializer in the real header; unverified */
extern const XHV_VOICE_MASK XHV_VOICE_MASK_ANONYMOUS;   /* unverified */
#ifdef __cplusplus
}
#endif
typedef struct _XHV_LOCAL_TALKER_STATUS { XHV_VOICE_COMMUNICATOR_STATUS communicatorStatus; XHV_PROCESSING_MODE processingMode;
    XHV_VOICE_MASK voiceMask; } XHV_LOCAL_TALKER_STATUS, *PXHV_LOCAL_TALKER_STATUS;
typedef struct _XHV_SR_ITEM { DWORD dwWordID; FLOAT fConfidence; } XHV_SR_ITEM, *PXHV_SR_ITEM;

#ifdef __cplusplus
struct ITitleXHV {
    STDMETHOD(LocalChatDataReady)(DWORD dwPort, DWORD dwSize, VOID *pvData) = 0;
    STDMETHOD(CommunicatorStatusUpdate)(DWORD dwPort, XHV_VOICE_COMMUNICATOR_STATUS status) = 0;
    STDMETHOD(SpeechRecognized)(DWORD dwPort, XHV_SR_ITEM *pItems, DWORD dwItemCount) = 0;
    STDMETHOD(VoiceMailDataReady)(DWORD dwPort, DWORD dwDuration, DWORD dwSize) = 0;
    STDMETHOD(VoiceMailStopped)(DWORD dwPort) = 0;
    STDMETHOD(MicrophoneRawDataReady)(DWORD dwPort, DWORD dwSize, VOID *pvData, BOOL *pbVoiceDetected) = 0;
};
typedef ITitleXHV *PITITLEXHV;

struct XHV_RUNTIME_PARAMS;
struct XHVEngine {
    STDMETHOD_(ULONG, AddRef)() = 0;
    STDMETHOD_(ULONG, Release)() = 0;
    STDMETHOD(SetCallbackInterface)(ITitleXHV *pCallbackInterface) = 0;
    STDMETHOD(EnableProcessingMode)(XHV_PROCESSING_MODE processingMode) = 0;
    STDMETHOD(DisableProcessingMode)(XHV_PROCESSING_MODE processingMode) = 0;
    STDMETHOD(SetProcessingMode)(DWORD dwPort, XHV_PROCESSING_MODE processingMode) = 0;
    STDMETHOD(GetProcessingMode)(DWORD dwPort, PXHV_PROCESSING_MODE pProcessingMode) = 0;
    STDMETHOD(SetVoiceMask)(DWORD dwPort, const XHV_VOICE_MASK *pVoiceMask) = 0;
    STDMETHOD(GetVoiceMask)(DWORD dwPort, XHV_VOICE_MASK *pVoiceMask) = 0;
    STDMETHOD(SetMaxPlaybackStreamsCount)(DWORD dwStreamsCount) = 0;
    STDMETHOD(GetMaxPlaybackStreamsCount)(PDWORD pdwStreamsCount) = 0;
    STDMETHOD(SetPlaybackPriority)(XUID xuidRemoteTalker, DWORD dwPort, XHV_PLAYBACK_PRIORITY priority) = 0;
    STDMETHOD(SetMixBinMapping)(XUID xuidRemoteTalker, DWORD dwPort, const DSMIXBINS *pMixBins) = 0;
    STDMETHOD(RegisterLocalTalker)(DWORD dwPort) = 0;
    STDMETHOD(UnregisterLocalTalker)(DWORD dwPort) = 0;
    STDMETHOD(RegisterRemoteTalker)(XUID xuidRemoteTalker) = 0;
    STDMETHOD(UnregisterRemoteTalker)(XUID xuidRemoteTalker) = 0;
    STDMETHOD(GetRemoteTalkers)(PDWORD pdwRemoteTalkersCount, XUID *pxuidRemoteTalkers) = 0;
    STDMETHOD_(BOOL, IsTalking)(XUID xuidRemoteTalker) = 0;
    STDMETHOD(GetLocalTalkerStatus)(DWORD dwPort, XHV_LOCAL_TALKER_STATUS *pLocalTalkerStatus) = 0;
    STDMETHOD(DoWork)() = 0;
    STDMETHOD(SubmitIncomingVoicePacket)(XUID xuidRemoteTalker, VOID *pvData, DWORD dwSize) = 0;
    STDMETHOD(VoiceMailRecord)(DWORD dwPort, DWORD dwMaxDurationInMilliseconds, DWORD dwSize, PBYTE pbBuffer) = 0;
    STDMETHOD(VoiceMailPlay)(DWORD dwPort, DWORD dwSize, const BYTE *pbData, BOOL bTransmit) = 0;
    STDMETHOD(VoiceMailStop)(DWORD dwPort) = 0;
};
typedef XHVEngine *PXHVENGINE, *LPXHVENGINE;

struct XHV_RUNTIME_PARAMS {
    DWORD dwMaxRemoteTalkers;
    DWORD dwMaxLocalTalkers;
    DWORD dwMaxCompressedBuffers;
    DWORD dwFlags;
    LPDSEFFECTIMAGEDESC pEffectImageDesc;
    DWORD dwEffectsStartIndex;
    DWORD dwOutOfSyncThreshold;
    BOOL bCustomVADProvided;
    BOOL bHeadphoneAlwaysOn;
};
typedef XHV_RUNTIME_PARAMS *PXHV_RUNTIME_PARAMS;
extern "C" HRESULT WINAPI XHVEngineCreate(PXHV_RUNTIME_PARAMS pParams, PXHVENGINE *ppEngine);
#endif
#endif
