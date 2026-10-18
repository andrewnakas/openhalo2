/* xonline.h stand-in for fecheck builds (VS2022 x86, no XDK).
   Declarations only, from the decomp's own usage (field names, call shapes,
   raw HRESULTs it compares against) and the public shape of the Xbox Live
   API. Never copied from an SDK header.

   Layout status: XONLINE_USER is 0x70 bytes (include/player_slots.h asserts
   user at +0x470 and the next member at +0x4e0) and XUID 12 bytes
   (include/online_friends.h, pack(1)); every other structure's layout is
   UNVERIFIED (public field names, best-known order). Constants marked
   "unverified" have no decomp evidence. Helpers that are macros or inlines in
   the real header (XOnlineIsUserGuest, XOnlineUserGuestNumber,
   XOnlineAreUsersIdentical, XOnlineTitleIdIsSameTitle) are plain externs. */
#pragma once
#ifndef FECHECK_XONLINE_H
#define FECHECK_XONLINE_H
#include "xtl.h"

#define XONLINE_MAX_LOGON_USERS 4
#define XONLINE_GAMERTAG_SIZE 16
#define XONLINE_PASSCODE_LENGTH 4
#define XONLINE_USER_RESERVED_SIZE 72        /* follows from the 0x70 size */
#define MAX_FRIENDS 100                      /* unverified */
#define MAX_MUTELISTUSERS 250                /* unverified */
#define MAX_STATEDATA_SIZE 32                /* unverified */
#define XONLINE_TEAM_NAME_SIZE 16            /* unverified */
#define XONLINE_TEAM_DESCRIPTION_SIZE 256    /* unverified */
#define XONLINE_TEAM_MOTTO_SIZE 256          /* unverified */
#define XONLINE_TEAM_URL_SIZE 256            /* unverified */
#define XONLINE_TEAM_DATA_SIZE 100           /* unverified */
#define XONLINE_TEAM_MEMBER_DATA_SIZE 100    /* unverified */

/* ---- HRESULTs ----
   CANNOT_ACCESS_SERVICE, UPDATE_REQUIRED, SERVERS_TOO_BUSY and INVALID_USER
   are cross-checked: src/online_tasks.cpp maps them to results 4, 3, 8 and 6
   by name, and function_6c360 maps 0x80151001/2/3/6 to the same results. */
#define XONLINE_E_LOGON_NO_NETWORK_CONNECTION            ((HRESULT)0x80151000L)  /* decomp raw at the same spot */
#define XONLINE_E_LOGON_CANNOT_ACCESS_SERVICE            ((HRESULT)0x80151001L)
#define XONLINE_E_LOGON_UPDATE_REQUIRED                  ((HRESULT)0x80151002L)
#define XONLINE_E_LOGON_SERVERS_TOO_BUSY                 ((HRESULT)0x80151003L)
#define XONLINE_E_LOGON_CONNECTION_LOST                  ((HRESULT)0x80151004L)  /* unverified */
#define XONLINE_E_LOGON_KICKED_BY_DUPLICATE_LOGON        ((HRESULT)0x80151005L)  /* unverified */
#define XONLINE_E_LOGON_INVALID_USER                     ((HRESULT)0x80151006L)
#define XONLINE_E_LOGON_USER_ACCOUNT_REQUIRES_MANAGEMENT ((HRESULT)0x80151007L)  /* unverified */
#define XONLINE_S_LOGON_CONNECTION_ESTABLISHED           ((HRESULT)0x001510F0L)  /* unverified */
#define XONLINE_S_LOGON_USER_HAS_MESSAGE                 ((HRESULT)0x001510F1L)  /* unverified */
#define XONLINETASK_S_RUNNING                            ((HRESULT)0x00150001L)  /* unverified */
#define XONLINETASK_S_RUNNING_IDLE                       ((HRESULT)0x00150002L)  /* unverified */
#define XONLINETASK_S_RESULTS_AVAIL                      ((HRESULT)0x00150003L)  /* unverified */

/* ---- handles ---- */
typedef struct XONLINETASK_CONTEXT *XONLINETASK_HANDLE, **PXONLINETASK_HANDLE;
typedef struct XONLINE_MSG_CONTEXT *XONLINE_MSG_HANDLE;

/* ---- users and friends ---- */
typedef struct _XONLINE_USER {
    XUID xuid;
    CHAR szGamertag[XONLINE_GAMERTAG_SIZE];
    DWORD dwUserOptions;
    BYTE passcode[XONLINE_PASSCODE_LENGTH];
    BYTE reserved[XONLINE_USER_RESERVED_SIZE];
    HRESULT hr;
} XONLINE_USER, *PXONLINE_USER;
#define XONLINE_USER_OPTION_REQUIRE_PASSCODE 0x00000001   /* unverified */

typedef struct _XONLINE_FRIEND {   /* layout unverified */
    XUID xuid;
    CHAR szGamertag[XONLINE_GAMERTAG_SIZE];
    DWORD dwFriendState;
    XNKID sessionID;
    DWORD dwTitleID;
    FILETIME gameinviteTime;
    DWORD StateDataSize;
    BYTE StateData[MAX_STATEDATA_SIZE];
} XONLINE_FRIEND, *PXONLINE_FRIEND;
/* friend-state / presence flags: all unverified */
#define XONLINE_FRIENDSTATE_FLAG_NONE            0x00000000
#define XONLINE_FRIENDSTATE_FLAG_ONLINE          0x00000001
#define XONLINE_FRIENDSTATE_FLAG_PLAYING         0x00000002
#define XONLINE_FRIENDSTATE_FLAG_CLOAKED         0x00000004
#define XONLINE_FRIENDSTATE_FLAG_VOICE           0x00000008
#define XONLINE_FRIENDSTATE_FLAG_JOINABLE        0x00000010
#define XONLINE_FRIENDSTATE_FLAG_SENTINVITE      0x04000000
#define XONLINE_FRIENDSTATE_FLAG_RECEIVEDINVITE  0x08000000
#define XONLINE_FRIENDSTATE_FLAG_INVITEACCEPTED  0x10000000
#define XONLINE_FRIENDSTATE_FLAG_INVITEREJECTED  0x20000000
#define XONLINE_FRIENDSTATE_FLAG_SENTREQUEST     0x40000000
#define XONLINE_FRIENDSTATE_FLAG_RECEIVEDREQUEST 0x80000000
#define XONLINE_PRESENCE_FLAG_ONLINE                 0x00000001
#define XONLINE_PRESENCE_FLAG_PLAYING                0x00000002
#define XONLINE_PRESENCE_FLAG_VOICE                  0x00000008
#define XONLINE_PRESENCE_FLAG_JOINABLE               0x00000010
#define XONLINE_PRESENCE_FLAG_RECEIVEDTITLECUSTOM    0x00010000
#define XONLINE_PRESENCE_FLAG_RECEIVEDTEAMRECRUIT    0x00020000
#define XONLINE_PRESENCE_FLAG_RECEIVEDCOMPREQUEST    0x00040000
#define XONLINE_PRESENCE_FLAG_RECEIVEDCOMPREMINDER   0x00080000
#define XONLINE_PRESENCE_FLAG_RECEIVEDINVITE         0x08000000
#define XONLINE_PRESENCE_FLAG_RECEIVEDREQUEST        0x80000000

typedef enum { XONLINE_REQUEST_NO = 0, XONLINE_REQUEST_YES = 1, XONLINE_REQUEST_BLOCK = 2 } XONLINE_REQUEST_ANSWER_TYPE;   /* unverified */
typedef enum { XONLINE_GAMEINVITE_NO = 0, XONLINE_GAMEINVITE_YES = 1, XONLINE_GAMEINVITE_REMOVE = 2 } XONLINE_GAMEINVITE_ANSWER_TYPE; /* unverified */
typedef enum { XONLINE_PEER_ANSWER_NO = 0, XONLINE_PEER_ANSWER_YES = 1, XONLINE_PEER_ANSWER_NEVER = 2 } XONLINE_PEER_ANSWER_TYPE;   /* unverified */
typedef enum { XONLINE_NAT_OPEN = 1, XONLINE_NAT_MODERATE = 2, XONLINE_NAT_STRICT = 3 } XONLINE_NAT_TYPE;                           /* unverified */

typedef struct _XONLINE_ACCEPTED_GAMEINVITE {   /* layout unverified */
    XONLINE_FRIEND InvitingFriend;
    XUID xuidAcceptedFriend;
    FILETIME InviteAcceptTime;
} XONLINE_ACCEPTED_GAMEINVITE, *PXONLINE_ACCEPTED_GAMEINVITE;
#define XONLINE_ACCEPTED_GAMEINVITE_EXPIRATION_INTERVAL 0   /* unverified placeholder */
typedef struct _XONLINE_GAMEINVITE_ANSWER_INFO {   /* layout unverified */
    XUID xuidInvitingUser;
    CHAR szInvitingUserGamertag[XONLINE_GAMERTAG_SIZE];
    DWORD dwTitleID;
    XNKID SessionID;
    FILETIME GameInviteTime;
    DWORD dwMessageID;
} XONLINE_GAMEINVITE_ANSWER_INFO, *PXONLINE_GAMEINVITE_ANSWER_INFO;

typedef struct _XONLINE_PRESENCE {   /* layout unverified */
    XUID xuid;
    DWORD dwUserState;
    XNKID SessionID;
    DWORD dwTitleID;
    FILETIME ftUserTime;
    DWORD StateDataSize;
    BYTE StateData[MAX_STATEDATA_SIZE];
} XONLINE_PRESENCE, *PXONLINE_PRESENCE;

typedef struct _XONLINE_MUTELISTUSER { XUID xuid; DWORD dwReserved; } XONLINE_MUTELISTUSER, *PXONLINE_MUTELISTUSER;  /* unverified */

/* ---- messages and notifications ---- */
typedef struct _XONLINE_MSG_SUMMARY {   /* layout unverified */
    XUID xuidSender;
    DWORD dwMessageID;
    FILETIME ftSentTime;
    DWORD dwMessageContext;
    DWORD dwMessageFlags;
    DWORD dwTitleID;
    WORD wExpireMinutes;
    BYTE bMessageType;
    BYTE bReserved;
    CHAR szSenderName[XONLINE_GAMERTAG_SIZE];
} XONLINE_MSG_SUMMARY, *PXONLINE_MSG_SUMMARY;
#define XONLINE_MSG_FLAG_READ 0x00000001   /* unverified */
typedef struct _XONLINE_NOTIFICATION_EX_INFO {   /* layout unverified */
    DWORD dwNotifyFlags;
    DWORD dwMessageID;
    BYTE bMessageType;
} XONLINE_NOTIFICATION_EX_INFO, *PXONLINE_NOTIFICATION_EX_INFO;

typedef enum {   /* order unverified */
    XONLINE_FEEDBACK_NEG_NICKNAME, XONLINE_FEEDBACK_NEG_GAMEPLAY, XONLINE_FEEDBACK_NEG_SCREAMING,
    XONLINE_FEEDBACK_NEG_HARASSMENT, XONLINE_FEEDBACK_NEG_LEWDNESS, XONLINE_FEEDBACK_POS_ATTITUDE,
    XONLINE_FEEDBACK_POS_SESSION
} XONLINE_FEEDBACK_TYPE;
typedef struct _XONLINE_FEEDBACK_PARAMS { LPCWSTR lpStringParam; } XONLINE_FEEDBACK_PARAMS, *PXONLINE_FEEDBACK_PARAMS;

/* passcode buttons: unverified */
#define XONLINE_PASSCODE_DPAD_UP 1
#define XONLINE_PASSCODE_DPAD_DOWN 2
#define XONLINE_PASSCODE_DPAD_LEFT 3
#define XONLINE_PASSCODE_DPAD_RIGHT 4
#define XONLINE_PASSCODE_GAMEPAD_X 5
#define XONLINE_PASSCODE_GAMEPAD_Y 6
#define XONLINE_PASSCODE_GAMEPAD_LEFT_TRIGGER 9
#define XONLINE_PASSCODE_GAMEPAD_RIGHT_TRIGGER 10

/* ---- teams ---- */
typedef struct _XONLINE_TEAM_PROPERTIES {   /* layout unverified */
    WCHAR wszTeamName[XONLINE_TEAM_NAME_SIZE];
    WCHAR wszDescription[XONLINE_TEAM_DESCRIPTION_SIZE];
    WCHAR wszMotto[XONLINE_TEAM_MOTTO_SIZE];
    WCHAR wszURL[XONLINE_TEAM_URL_SIZE];
    DWORD TeamDataSize;
    BYTE TeamData[XONLINE_TEAM_DATA_SIZE];
} XONLINE_TEAM_PROPERTIES, *PXONLINE_TEAM_PROPERTIES;
/* sizeof(XONLINE_TEAM) + sizeof(XONLINE_TEAM_MEMBER) = 0x734 (src/unknown_18f62b.cpp
   s_slot_update_view asserts); the split between the two FILETIMEs is unverified */
typedef struct _XONLINE_TEAM {   /* layout unverified */
    XUID xuidTeam;
    DWORD dwNumMembers;
    FILETIME ftCreated;
    XONLINE_TEAM_PROPERTIES TeamProperties;
} XONLINE_TEAM, *PXONLINE_TEAM;
typedef struct _XONLINE_TEAM_MEMBER_PROPERTIES {   /* layout unverified */
    DWORD dwPrivileges;
    DWORD TeamMemberDataSize;
    BYTE TeamMemberData[XONLINE_TEAM_MEMBER_DATA_SIZE];
} XONLINE_TEAM_MEMBER_PROPERTIES, *PXONLINE_TEAM_MEMBER_PROPERTIES;
typedef struct _XONLINE_TEAM_MEMBER {   /* layout unverified */
    XUID xuidTeamMember;
    CHAR szGamertag[XONLINE_GAMERTAG_SIZE];
    DWORD dwFlags;
    FILETIME ftJoined;
    XONLINE_TEAM_MEMBER_PROPERTIES TeamMemberProperties;
} XONLINE_TEAM_MEMBER, *PXONLINE_TEAM_MEMBER;

/* ---- matchmaking ---- */
#define X_ATTRIBUTE_DATATYPE_MASK    0x00F00000   /* unverified */
#define X_ATTRIBUTE_DATATYPE_INTEGER 0x00000000   /* unverified */
#define X_ATTRIBUTE_DATATYPE_STRING  0x00100000   /* unverified */
#define X_ATTRIBUTE_DATATYPE_BLOB    0x00200000   /* unverified */
#define X_ATTRIBUTE_DATATYPE_NULL    0x00F00000   /* unverified */
typedef struct _XONLINE_ATTRIBUTE {   /* layout unverified */
    DWORD dwAttributeID;
    BOOL fChanged;
    union {
        struct { ULONGLONG qwValue; } integer;
        struct { LPWSTR lpValue; } string;
        struct { PVOID pvValue; DWORD dwLength; } blob;
    } info;
} XONLINE_ATTRIBUTE, *PXONLINE_ATTRIBUTE;
typedef struct _XONLINE_ATTRIBUTE_SPEC { DWORD type; DWORD length; } XONLINE_ATTRIBUTE_SPEC, *PXONLINE_ATTRIBUTE_SPEC;  /* unverified */
typedef struct _XONLINE_MATCH_SEARCHRESULT {   /* layout unverified */
    DWORD dwReserved;
    XNKID SessionID;
    XNADDR HostAddress;
    XNKEY KeyExchangeKey;
    DWORD dwPublicOpen;
    DWORD dwPrivateOpen;
    DWORD dwPublicFilled;
    DWORD dwPrivateFilled;
    DWORD dwNumAttributes;
} XONLINE_MATCH_SEARCHRESULT, *PXONLINE_MATCH_SEARCHRESULT;

/* ---- stats and arbitration ---- */
typedef struct _XONLINE_STAT {   /* layout unverified */
    WORD wID;
    BYTE type;
    union { ULONG lValue; LONGLONG llValue; double dValue; LPWSTR lpString; };
} XONLINE_STAT, *PXONLINE_STAT;
typedef struct _XONLINE_STAT_SPEC {   /* layout unverified */
    XUID xuidUser;
    DWORD dwLeaderBoardID;
    DWORD dwNumStats;
    PXONLINE_STAT pStats;
} XONLINE_STAT_SPEC, *PXONLINE_STAT_SPEC;
typedef struct _XONLINE_STAT_UPDATE { XUID xuid; DWORD dwLeaderBoardID; DWORD dwNumStats; PXONLINE_STAT pStats; } XONLINE_STAT_UPDATE;  /* unverified */
typedef struct _XONLINE_STAT_PROC {   /* layout unverified */
    WORD wProcedureID;
    union { XONLINE_STAT_UPDATE Update; BYTE reserved[0x20]; };
} XONLINE_STAT_PROC, *PXONLINE_STAT_PROC;
typedef struct _XONLINE_STAT_ATTACHMENT_REFERENCE { DWORD dwReserved[4]; } XONLINE_STAT_ATTACHMENT_REFERENCE, *PXONLINE_STAT_ATTACHMENT_REFERENCE;  /* unverified */
typedef struct _XONLINE_ARB_ID { XNKID SessionID; ULONGLONG qwRoundID; } XONLINE_ARB_ID, *PXONLINE_ARB_ID;  /* layout unverified */

/* ---- services and logon ---- */
typedef struct _XONLINE_SERVICE_INFO { DWORD dwServiceID; IN_ADDR serviceIP; WORD wServicePort; WORD wReserved; } XONLINE_SERVICE_INFO, *PXONLINE_SERVICE_INFO;  /* unverified */
typedef struct _XONLINE_STARTUP_PARAMS { DWORD dwReserved; } XONLINE_STARTUP_PARAMS, *PXONLINE_STARTUP_PARAMS;  /* unverified */
typedef struct _XONLINE_LOGON_STATE { BYTE ab[0x200]; } XONLINE_LOGON_STATE, *PXONLINE_LOGON_STATE;  /* opaque; size unverified */
typedef struct _LD_DOWNLOADER { XONLINE_LOGON_STATE LogonState; BYTE Reserved[MAX_LAUNCH_DATA_SIZE - sizeof(XONLINE_LOGON_STATE)]; } LD_DOWNLOADER;  /* layout unverified */

#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI XOnlineStartup(const XONLINE_STARTUP_PARAMS *pxosp);
HRESULT WINAPI XOnlineCleanup(void);
HRESULT WINAPI XOnlineTaskContinue(XONLINETASK_HANDLE hTask);
HRESULT WINAPI XOnlineTaskClose(XONLINETASK_HANDLE hTask);
HRESULT WINAPI XOnlineGetUsers(XONLINE_USER *pUsers, DWORD *pcUsers);
XONLINE_USER *WINAPI XOnlineGetLogonUsers(void);
HRESULT WINAPI XOnlineLogon(const XONLINE_USER *pUsers, const DWORD *pdwServiceIDs, DWORD cServices, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineLogonTaskGetResults(XONLINETASK_HANDLE hLogonTask);
HRESULT WINAPI XOnlineChangeLogonUsers(const XONLINE_USER *pUsers, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineChangeLogonUsersTaskGetResults(XONLINETASK_HANDLE hTask, HRESULT *phrUsers);
HRESULT WINAPI XOnlineGetServiceInfo(DWORD dwServiceID, XONLINE_SERVICE_INFO *pServiceInfo);
DWORD WINAPI XOnlineGetNatType(void);
HRESULT WINAPI XOnlineSaveLogonState(XONLINE_LOGON_STATE *pLogonState);
BOOL WINAPI XOnlineIsUserGuest(DWORD dwUserFlags);
DWORD WINAPI XOnlineUserGuestNumber(DWORD dwUserFlags);
BOOL WINAPI XOnlineAreUsersIdentical(const XUID *pxuid1, const XUID *pxuid2);
BOOL WINAPI XOnlineTitleIdIsSameTitle(DWORD dwTitleID);
BOOL WINAPI XOnlineGetNotificationEx(DWORD dwUserIndex, XONLINE_NOTIFICATION_EX_INFO *pInfo, DWORD *pdwStateFlags);
HRESULT WINAPI XOnlineNotificationSetState(DWORD dwUserIndex, DWORD dwStateFlags, XNKID sessionID, DWORD cbStateData, const BYTE *pStateData);

HRESULT WINAPI XOnlineFriendsStartup(HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineFriendsEnumerate(DWORD dwUserIndex, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineFriendsEnumerateFinish(XONLINETASK_HANDLE hTask);
DWORD WINAPI XOnlineFriendsGetLatest(DWORD dwUserIndex, DWORD dwFriendBufferCount, XONLINE_FRIEND *pFriendBuffer);
HRESULT WINAPI XOnlineFriendsRemove(DWORD dwUserIndex, const XONLINE_FRIEND *pFriend);
HRESULT WINAPI XOnlineFriendsRequestEx(DWORD dwUserIndex, XUID xuidToUser, XONLINE_MSG_HANDLE hMsg, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineFriendsRequestByNameEx(DWORD dwUserIndex, LPCSTR lpUserName, XONLINE_MSG_HANDLE hMsg, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineFriendsAnswerRequest(DWORD dwUserIndex, const XONLINE_FRIEND *pToFriend, XONLINE_REQUEST_ANSWER_TYPE Answer);
HRESULT WINAPI XOnlineFriendsGameInvite(DWORD dwUserIndex, XNKID SessionID, DWORD dwFriendListCount, const XONLINE_FRIEND *pToFriendList);
HRESULT WINAPI XOnlineFriendsAnswerGameInvite(DWORD dwUserIndex, const XONLINE_FRIEND *pToFriend, XONLINE_GAMEINVITE_ANSWER_TYPE Answer);
HRESULT WINAPI XOnlineFriendsGetAcceptedGameInvite(XONLINE_ACCEPTED_GAMEINVITE *pAcceptedGameInvite);
HRESULT WINAPI XOnlineFriendsJoinGame(DWORD dwUserIndex, const XONLINE_FRIEND *pToFriend);
HRESULT WINAPI XOnlineFriendsGetTitleName(DWORD dwTitleID, DWORD dwLanguage, DWORD cTitleName, WCHAR *lpTitleName);
HRESULT WINAPI XOnlineGameInviteSend(DWORD dwUserIndex, DWORD cInvitees, const XUID *pxuidInvitees, XNKID SessionID, DWORD dwFlags, XONLINE_MSG_HANDLE hMsg, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineGameInviteAnswer(DWORD dwUserIndex, const XONLINE_GAMEINVITE_ANSWER_INFO *pInfo, XONLINE_PEER_ANSWER_TYPE Answer, HANDLE hEvent, PXONLINETASK_HANDLE phTask);

HRESULT WINAPI XOnlineMutelistStartup(HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMutelistGet(DWORD dwUserIndex, DWORD dwMutelistUserBufferCount, HANDLE hEvent, PXONLINETASK_HANDLE phTask, XONLINE_MUTELISTUSER *pMutelistUsers, DWORD *pdwNumMutelistUsers);
HRESULT WINAPI XOnlineMutelistAdd(DWORD dwUserIndex, XUID xuidMutedUser);
HRESULT WINAPI XOnlineMutelistRemove(DWORD dwUserIndex, XUID xuidMutedUser);

HRESULT WINAPI XOnlinePresenceInit(DWORD dwUserIndex, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlinePresenceAdd(XONLINETASK_HANDLE hTask, DWORD dwGroupID, DWORD cUsers, const XUID *pxuidUsers);
HRESULT WINAPI XOnlinePresenceClear(XONLINETASK_HANDLE hTask);
HRESULT WINAPI XOnlinePresenceSubmit(XONLINETASK_HANDLE hTask);
HRESULT WINAPI XOnlinePresenceGetLatest(XONLINETASK_HANDLE hTask, DWORD dwGroupID, DWORD cPresences, XONLINE_PRESENCE *pPresences);

HRESULT WINAPI XOnlineMessageEnumerate(DWORD dwUserIndex, XONLINE_MSG_SUMMARY *pMsgSummaries, DWORD *pdwNumMsgs);
HRESULT WINAPI XOnlineMessageDelete(DWORD dwUserIndex, DWORD dwMessageID, BOOL fBlockSender);
HRESULT WINAPI XOnlineMessageDetails(DWORD dwUserIndex, DWORD dwMessageID, DWORD dwSetFlags, DWORD dwUnsetFlags, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMessageDetailsGetResultsProperty(XONLINETASK_HANDLE hTask, WORD wPropTag, DWORD cbPropValueBuffer, void *pvPropValue, DWORD *pcbPropValue, DWORD *pdwAttachmentFlags);
HRESULT WINAPI XOnlineMessageDownloadAttachmentToMemory(XONLINETASK_HANDLE hTask, WORD wPropTag, BYTE *pbBuffer, DWORD cbBuffer, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMessageDownloadAttachmentToMemoryGetResults(XONLINETASK_HANDLE hTask, BYTE **ppbBuffer, DWORD *pcbBuffer, DWORD *pcbTotalMsgLen);
HRESULT WINAPI XOnlineMessageCreate(BYTE bMsgType, WORD wNumProperties, WORD wPropTotalSize, DWORD dwMessageContext, DWORD dwMessageFlags, DWORD dwExpireMinutes, XONLINE_MSG_HANDLE *phMsg);
HRESULT WINAPI XOnlineMessageDestroy(XONLINE_MSG_HANDLE hMsg);
HRESULT WINAPI XOnlineMessageSetProperty(XONLINE_MSG_HANDLE hMsg, WORD wPropTag, DWORD cbPropValue, const void *pvPropValue, DWORD dwFlags);
HRESULT WINAPI XOnlineMessageSend(DWORD dwUserIndex, XONLINE_MSG_HANDLE hMsg, DWORD dwNumRecipients, const XUID *pxuidRecipients, HANDLE hEvent, PXONLINETASK_HANDLE phTask);

HRESULT WINAPI XOnlineFeedbackSend(DWORD dwUserIndex, XUID xTargetUser, XONLINE_FEEDBACK_TYPE FeedbackType, const XONLINE_FEEDBACK_PARAMS *pParams, HANDLE hEvent, PXONLINETASK_HANDLE phTask);

DWORD WINAPI XOnlineMatchSearchResultsLen(DWORD dwNumResults, DWORD dwNumSessionAttributes, const XONLINE_ATTRIBUTE_SPEC *pSessionAttributeSpec);
HRESULT WINAPI XOnlineMatchSearch(DWORD dwProcedureIndex, DWORD dwNumResults, DWORD dwNumAttributes, const XONLINE_ATTRIBUTE *pAttributes, DWORD dwResultsLen, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMatchSearchGetResults(XONLINETASK_HANDLE hTask, PXONLINE_MATCH_SEARCHRESULT **prgpSearchResults, DWORD *pdwReturnedResults);
HRESULT WINAPI XOnlineMatchSearchParse(const XONLINE_MATCH_SEARCHRESULT *pSearchResult, DWORD dwNumSessionAttributes, const XONLINE_ATTRIBUTE_SPEC *pSessionAttributeSpec, void *pQuerySession);
HRESULT WINAPI XOnlineMatchSessionCreate(DWORD dwPublicFilled, DWORD dwPublicOpen, DWORD dwPrivateFilled, DWORD dwPrivateOpen, DWORD dwNumAttributes, const XONLINE_ATTRIBUTE *pAttributes, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMatchSessionUpdate(XNKID SessionID, DWORD dwPublicFilled, DWORD dwPublicOpen, DWORD dwPrivateFilled, DWORD dwPrivateOpen, DWORD dwNumAttributes, const XONLINE_ATTRIBUTE *pAttributes, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMatchSessionGetInfo(XONLINETASK_HANDLE hTask, XNKID *pSessionID, XNKEY *pKeyExchangeKey);
HRESULT WINAPI XOnlineMatchSessionDelete(XNKID SessionID, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineMatchSessionFindFromID(XNKID SessionID, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);

HRESULT WINAPI XOnlineStatRead(DWORD dwNumStatSpecs, XONLINE_STAT_SPEC *pStatSpecs, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineStatReadGetResult(XONLINETASK_HANDLE hTask, DWORD dwNumStatSpecs, XONLINE_STAT_SPEC *pStatSpecs, DWORD dwNumExtraBytes, void *pExtraBytes);
HRESULT WINAPI XOnlineStatWrite(DWORD dwNumStatSpecs, const XONLINE_STAT_SPEC *pStatSpecs, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineStatWriteGetResult(XONLINETASK_HANDLE hTask, HANDLE *phResult, PXONLINE_STAT_ATTACHMENT_REFERENCE *ppReferences, DWORD *pdwNumReferences);
HRESULT WINAPI XOnlineArbitrationCreateRoundID(ULONGLONG *pqwRoundID);
HRESULT WINAPI XOnlineArbitrationRegister(const XONLINE_ARB_ID *pArbID, DWORD dwSeconds, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineArbitrationExtendRound(const XONLINE_ARB_ID *pArbID, DWORD dwSeconds, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineArbitrationReport(const XONLINE_ARB_ID *pArbID, DWORD dwNumStatProcs, const XONLINE_STAT_PROC *pStatProcs, void *pReserved, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);

HRESULT WINAPI XOnlineStorageCreateServerPath(DWORD dwFacility, ULONGLONG qwUserPUID, ULONGLONG qwTeamPUID, LPCWSTR lpName, LPWSTR lpServerPath, DWORD *pcchServerPath);
HRESULT WINAPI XOnlineStorageDownload(DWORD dwFacility, DWORD dwUserIndex, LPCWSTR lpServerPath, LPCSTR lpLocalPath, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineStorageDownloadToMemory(DWORD dwFacility, DWORD dwUserIndex, LPCWSTR lpServerPath, BYTE *pbBuffer, DWORD cbBuffer, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineStorageDownloadToMemoryGetResults(XONLINETASK_HANDLE hTask, BYTE **ppbBuffer, DWORD *pcbBuffer, DWORD *pcbTotal, ULONGLONG *pqwOwnerPUID, FILETIME *pftCreated);
HRESULT WINAPI XOnlineStorageUploadByServerPath(DWORD dwFacility, DWORD dwUserIndex, LPCWSTR lpServerPath, FILETIME ftExpiration, LPCSTR lpLocalPath, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineStorageUploadFromMemory(DWORD dwFacility, DWORD dwUserIndex, LPCWSTR lpServerPath, FILETIME ftExpiration, const BYTE *pbBuffer, DWORD cbBuffer, DWORD dwFlags, HANDLE hWorkEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineOfferingIsNewContentAvailable(DWORD dwBitFilter, HANDLE hEvent, PXONLINETASK_HANDLE phTask);

HRESULT WINAPI XOnlineTeamCreate(DWORD dwUserIndex, const XONLINE_TEAM_PROPERTIES *pTeamProperties, const XONLINE_TEAM_MEMBER_PROPERTIES *pCreatorProperties, DWORD dwMaxMembers, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamDelete(DWORD dwUserIndex, XUID xuidTeam, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamEnumerateByUserXUID(DWORD dwUserIndex, XUID xuidUser, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamEnumerateGetResults(XONLINETASK_HANDLE hTask, DWORD *pdwNumTeams, XUID *pTeams);
HRESULT WINAPI XOnlineTeamGetDetails(XONLINETASK_HANDLE hTask, XUID xuidTeam, XONLINE_TEAM *pTeam);
HRESULT WINAPI XOnlineTeamMembersEnumerate(DWORD dwUserIndex, XUID xuidTeam, BOOL fShowRecruits, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamMembersEnumerateGetResults(XONLINETASK_HANDLE hTask, DWORD *pdwNumMembers, XUID *pMembers);
HRESULT WINAPI XOnlineTeamMemberGetDetails(XONLINETASK_HANDLE hTask, XUID xuidMember, XONLINE_TEAM_MEMBER *pMember);
HRESULT WINAPI XOnlineTeamMemberRecruit(DWORD dwUserIndex, XUID xuidTeam, XUID xuidUser, const XONLINE_TEAM_MEMBER_PROPERTIES *pProperties, XONLINE_MSG_HANDLE hMsg, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamMemberRecruitByName(DWORD dwUserIndex, XUID xuidTeam, LPCSTR lpUserName, const XONLINE_TEAM_MEMBER_PROPERTIES *pProperties, XONLINE_MSG_HANDLE hMsg, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamMemberAnswerRecruit(DWORD dwUserIndex, XUID xuidTeam, XONLINE_PEER_ANSWER_TYPE Answer, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamMemberRemove(DWORD dwUserIndex, XUID xuidTeam, XUID xuidMember, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
HRESULT WINAPI XOnlineTeamMemberSetProperties(DWORD dwUserIndex, XUID xuidTeam, XUID xuidMember, const XONLINE_TEAM_MEMBER_PROPERTIES *pProperties, HANDLE hEvent, PXONLINETASK_HANDLE phTask);
#ifdef __cplusplus
}
#endif
#endif
