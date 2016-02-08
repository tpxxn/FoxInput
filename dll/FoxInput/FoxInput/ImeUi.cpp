//--------------------------------------------------------------------------------------
// File: ImeUi.cpp
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=320437
//--------------------------------------------------------------------------------------
#include "stdafx.h"
#include <tchar.h>
#include <assert.h>
#undef min
#include <algorithm>
#pragma comment ( lib, "imm32.lib" )
#pragma comment ( lib, "Version.lib" )
#define SAFE_RELEASE(p)      { if (p) { (p)->Release(); (p)=NULL; } }

#include "ImeUi.h"
#include <msctf.h>
#include "AutoLock.h"

// Ignore typecast warnings
#pragma warning( disable : 4312 )
#pragma warning( disable : 4244 )
#pragma warning( disable : 4311 )

#pragma prefast( disable : 28159, "GetTickCount() is fine for a blinking cursor" )

#define MAX_CANDIDATE_LENGTH 256
#define COUNTOF(a) ( sizeof( a ) / sizeof( ( a )[0] ) )
#define POSITION_UNINITIALIZED	((DWORD)-1)

#define LANG_CHT	MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)
#define LANG_CHS	MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)

#define MAKEIMEVERSION(major,minor) ( (DWORD)( ( (BYTE)( major ) << 24 ) | ( (BYTE)( minor ) << 16 ) ) )
#define IMEID_VER(dwId)		( ( dwId ) & 0xffff0000 )
#define IMEID_LANG(dwId)	( ( dwId ) & 0x0000ffff )

#define _CHT_HKL_DAYI				( (HKL)0xE0060404 )	// DaYi
#define _CHT_HKL_NEW_PHONETIC		( (HKL)0xE0080404 )	// New Phonetic
#define _CHT_HKL_NEW_CHANG_JIE		( (HKL)0xE0090404 )	// New Chang Jie
#define _CHT_HKL_NEW_QUICK			( (HKL)0xE00A0404 )	// New Quick
#define _CHT_HKL_HK_CANTONESE		( (HKL)0xE00B0404 )	// Hong Kong Cantonese
#define _CHT_IMEFILENAME	"TINTLGNT.IME"	// New Phonetic
#define _CHT_IMEFILENAME2	"CINTLGNT.IME"	// New Chang Jie
#define _CHT_IMEFILENAME3	"MSTCIPHA.IME"	// Phonetic 5.1
#define IMEID_CHT_VER42 ( LANG_CHT | MAKEIMEVERSION( 4, 2 ) )	// New(Phonetic/ChanJie)IME98  : 4.2.x.x // Win98
#define IMEID_CHT_VER43 ( LANG_CHT | MAKEIMEVERSION( 4, 3 ) )	// New(Phonetic/ChanJie)IME98a : 4.3.x.x // Win2k
#define IMEID_CHT_VER44 ( LANG_CHT | MAKEIMEVERSION( 4, 4 ) )	// New ChanJie IME98b          : 4.4.x.x // WinXP
#define IMEID_CHT_VER50 ( LANG_CHT | MAKEIMEVERSION( 5, 0 ) )	// New(Phonetic/ChanJie)IME5.0 : 5.0.x.x // WinME
#define IMEID_CHT_VER51 ( LANG_CHT | MAKEIMEVERSION( 5, 1 ) )	// New(Phonetic/ChanJie)IME5.1 : 5.1.x.x // IME2002(w/OfficeXP)
#define IMEID_CHT_VER52 ( LANG_CHT | MAKEIMEVERSION( 5, 2 ) )	// New(Phonetic/ChanJie)IME5.2 : 5.2.x.x // IME2002a(w/WinXP)
#define IMEID_CHT_VER60 ( LANG_CHT | MAKEIMEVERSION( 6, 0 ) )	// New(Phonetic/ChanJie)IME6.0 : 6.0.x.x // New IME 6.0(web download)
#define IMEID_CHT_VER_VISTA ( LANG_CHT | MAKEIMEVERSION( 7, 0 ) )	// All TSF TIP under Cicero UI-less mode: a hack to make GetImeId() return non-zero value

#define _CHS_HKL		( (HKL)0xE00E0804 )	// MSPY
#define _CHS_IMEFILENAME	"PINTLGNT.IME"	// MSPY1.5/2/3
#define _CHS_IMEFILENAME2	"MSSCIPYA.IME"	// MSPY3 for OfficeXP
#define IMEID_CHS_VER41	( LANG_CHS | MAKEIMEVERSION( 4, 1 ) )	// MSPY1.5	// SCIME97 or MSPY1.5 (w/Win98, Office97)
#define IMEID_CHS_VER42	( LANG_CHS | MAKEIMEVERSION( 4, 2 ) )	// MSPY2	// Win2k/WinME
#define IMEID_CHS_VER53	( LANG_CHS | MAKEIMEVERSION( 5, 3 ) )	// MSPY3	// WinXP

static CHAR signature[] = "%%%IMEUILIB:070111%%%";

// Definition from Win98DDK version of IMM.H
typedef struct
tagINPUTCONTEXT2
{
    HWND hWnd;
    BOOL fOpen;
    POINT ptStatusWndPos;
    POINT ptSoftKbdPos;
    DWORD fdwConversion;
    DWORD fdwSentence;
    union
    {
        LOGFONTA A;
        LOGFONTW W;
    } lfFont;
    COMPOSITIONFORM cfCompForm;
    CANDIDATEFORM cfCandForm[4];
    HIMCC hCompStr;
    HIMCC hCandInfo;
    HIMCC hGuideLine;
    HIMCC hPrivate;
    DWORD dwNumMsgBuf;
    HIMCC hMsgBuf;
    DWORD fdwInit;
    DWORD dwReserve[3];
}
INPUTCONTEXT2, *PINPUTCONTEXT2, NEAR *NPINPUTCONTEXT2,
FAR*                            LPINPUTCONTEXT2;


// Class to disable Cicero in case ImmDisableTextFrameService() doesn't disable it completely
class CDisableCicero
{
public:
    CDisableCicero() : m_ptim(nullptr),
        m_bComInit(false)
    {
    }
    ~CDisableCicero()
    {
        Uninitialize();
    }
    void    Initialize()
    {
        if (m_bComInit)
        {
            return;
        }
        HRESULT hr;
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr))
        {
            m_bComInit = true;
            hr = CoCreateInstance(CLSID_TF_ThreadMgr,
                nullptr,
                CLSCTX_INPROC_SERVER,
                __uuidof(ITfThreadMgr),
                (void**)&m_ptim);
        }
    }
    void    Uninitialize()
    {
        if (m_ptim)
        {
            m_ptim->Release();
            m_ptim = nullptr;
        }
        if (m_bComInit)
            CoUninitialize();
        m_bComInit = false;
    }

    void    DisableCiceroOnThisWnd(HWND hwnd)
    {
        if (!m_ptim)
            return;
        ITfDocumentMgr* pdimPrev; // the dim that is associated previously.
                                  // Associate nullptr dim to the window.
                                  // When this window gets the focus, Cicero does not work and IMM32 IME
                                  // will be activated.
        if (SUCCEEDED(m_ptim->AssociateFocus(hwnd, nullptr, &pdimPrev)))
        {
            if (pdimPrev)
                pdimPrev->Release();
        }
    }
private:
    ITfThreadMgr* m_ptim;
    bool m_bComInit;
};
static CDisableCicero           g_disableCicero;

#define _IsLeadByte(x) ( LeadByteTable[(BYTE)( x )] )
static void _PumpMessage();
static BYTE LeadByteTable[256];
#define _ImmGetContext	ImmGetContext
#define _ImmReleaseContext	ImmReleaseContext
#define _ImmAssociateContext	ImmAssociateContext
static LONG(WINAPI* _ImmGetCompositionString)(HIMC himc, DWORD dwIndex, LPVOID lpBuf, DWORD dwBufLen);
#define _ImmGetOpenStatus	ImmGetOpenStatus
#define _ImmSetOpenStatus	ImmSetOpenStatus
#define _ImmGetConversionStatus	ImmGetConversionStatus
static DWORD(WINAPI* _ImmGetCandidateList)(HIMC himc, DWORD deIndex, LPCANDIDATELIST lpCandList, DWORD dwBufLen);
static LPINPUTCONTEXT2(WINAPI* _ImmLockIMC)(HIMC hIMC);
static BOOL(WINAPI* _ImmUnlockIMC)(HIMC hIMC);
static LPVOID(WINAPI* _ImmLockIMCC)(HIMCC hIMCC);
static BOOL(WINAPI* _ImmUnlockIMCC)(HIMCC hIMCC);
#define _ImmGetDefaultIMEWnd	ImmGetDefaultIMEWnd
#define _ImmGetIMEFileNameA	ImmGetIMEFileNameA
#define _ImmGetVirtualKey	ImmGetVirtualKey
#define _ImmNotifyIME	ImmNotifyIME
#define _ImmSetConversionStatus	ImmSetConversionStatus
#define _ImmSimulateHotKey	ImmSimulateHotKey
#define _ImmIsIME	ImmIsIME

// private API provided by CHT IME. Available on version 6.0 or later.
UINT(WINAPI*_GetReadingString)(HIMC himc, UINT uReadingBufLen, LPWSTR lpwReadingBuf, PINT pnErrorIndex,
    BOOL* pfIsVertical, PUINT puMaxReadingLen);
BOOL(WINAPI*_ShowReadingWindow)(HIMC himc, BOOL bShow);

// Callbacks
void*                           (__cdecl*ImeUiCallback_Malloc)(size_t bytes);
void(__cdecl*ImeUiCallback_Free)(void* ptr);
void (CALLBACK*ImeUiCallback_OnChar)(WCHAR wc);

CRITICAL_SECTION ImeUiSyncRoot;

static void(*_SendCompString)();
static LRESULT(WINAPI* _SendMessage)(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) = SendMessageA;
static DWORD(*_GetCandidateList)(HIMC himc, DWORD dwIndex, LPCANDIDATELIST* ppCandList);

static HWND                     g_hwndMain;
static HWND                     g_hwndCurr;
static HIMC                     g_himcOrg;
static bool                     g_bImeEnabled = false;
static TCHAR g_szCompositionString[256];
static BYTE g_szCompAttrString[256];
static DWORD                    g_IMECursorBytes = 0;
static DWORD                    g_IMECursorChars = 0;
static TCHAR g_szCandidate[MAX_CANDLIST][MAX_CANDIDATE_LENGTH];
static DWORD                    g_dwSelection, g_dwCount;
static UINT                     g_uCandPageSize;
static DWORD                    g_bDisableImeCompletely = false;
static DWORD                    g_dwIMELevel;
static DWORD                    g_dwIMELevelSaved;
static TCHAR g_szMultiLineCompString[256 * (3 - sizeof(TCHAR))];
static bool                     g_bReadingWindow = false;
static bool                     g_bHorizontalReading = false;
static bool                     g_bVerticalCand = true;
static UINT                     g_uCaretBlinkLast = 0;
static bool                     g_bCaretDraw = false;
static bool                     g_bChineseIME;
static bool                     g_bInsertMode = true;
static TCHAR g_szReadingString[32];	// Used only in case of horizontal reading window
static int                      g_iReadingError;	// Used only in case of horizontal reading window
static DWORD                    g_dwPrevFloat;
static bool                     bIsSendingKeyMessage = false;
static OSVERSIONINFOA           g_osi;
static bool                     g_bInitialized = false;
static bool                     g_bCandList = false;
static DWORD                    g_hCompChar;
static int                      g_iCandListIndexBase;
static DWORD                    g_dwImeUiFlags = IMEUI_FLAG_SUPPORT_CARET;
static bool                     g_bUILessMode = false;
static HMODULE                  g_hImmDll = nullptr;

#define IsNT() (g_osi.dwPlatformId == VER_PLATFORM_WIN32_NT)

static DWORD                    g_dwState = IMEUI_STATE_OFF;
static DWORD                    swirl = 0;
static double                   lastSwirl;

#define INDICATOR_NON_IME	0
#define INDICATOR_CHS		1
#define INDICATOR_CHT		2
#define INDICATOR_KOREAN	3
#define INDICATOR_JAPANESE	4

#define GETLANG()		LOWORD(g_hklCurrent)
#define GETPRIMLANG()	((WORD)PRIMARYLANGID(GETLANG()))
#define GETSUBLANG()	SUBLANGID(GETLANG())

#define LANG_CHS MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
#define LANG_CHT MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)

static HKL                      g_hklCurrent = 0;
static UINT                     g_uCodePage = 0;
static LPTSTR g_aszIndicator[] =
{
    TEXT("A"),
    TEXT("\x7B80"),
    TEXT("\x7E41"),
    TEXT("\xac00"),
    TEXT("\x3042"),
};
static LPTSTR                   g_pszIndicatior = g_aszIndicator[0];

static void GetReadingString(_In_ HWND hWnd);
static DWORD GetImeId(_In_ UINT uIndex = 0);
static void CheckToggleState();
static void GetReadingWindowOrientation(_In_ DWORD dwId);
static void OnInputLangChangeWorker();
static void OnInputLangChange();
static void SetImeApi();
static void CheckInputLocale();
static void SetSupportLevel(_In_ DWORD dwImeLevel);
void ImeUi_SetSupportLevel(_In_ DWORD dwImeLevel);


//
//	local helper functions
//
inline LRESULT SendKeyMsg(HWND hwnd, UINT msg, WPARAM wp)
{
    bIsSendingKeyMessage = true;
    LRESULT lRc = _SendMessage(hwnd, msg, wp, 1);
    bIsSendingKeyMessage = false;
    return lRc;
}
#define SendKeyMsg_DOWN(hwnd,vk)	SendKeyMsg(hwnd, WM_KEYDOWN, vk)
#define SendKeyMsg_UP(hwnd,vk)		SendKeyMsg(hwnd, WM_KEYUP, vk)

///////////////////////////////////////////////////////////////////////////////
//
//  CTsfUiLessMode
//      Handles IME events using Text Service Framework (TSF). Before Vista,
//      IMM (Input Method Manager) API has been used to handle IME events and
//      inqueries. Some IMM functions lose backward compatibility due to design
//      of TSF, so we have to use new TSF interfaces.
//
///////////////////////////////////////////////////////////////////////////////
class CTsfUiLessMode
{
protected:
    // Sink receives event notifications
    class CUIElementSink : public ITfUIElementSink,
        public ITfInputProcessorProfileActivationSink,
        public ITfCompartmentEventSink
    {
    public:
        CUIElementSink();
        virtual ~CUIElementSink();

        // IUnknown
        STDMETHODIMP    QueryInterface(_In_ REFIID riid, _COM_Outptr_ void** ppvObj);
        STDMETHODIMP_(ULONG)
            AddRef();
        STDMETHODIMP_(ULONG)
            Release();

        // ITfUIElementSink
        //   Notifications for Reading Window events. We could process candidate as well, but we'll use IMM for simplicity sake.
        STDMETHODIMP    BeginUIElement(DWORD dwUIElementId, BOOL* pbShow);
        STDMETHODIMP    UpdateUIElement(DWORD dwUIElementId);
        STDMETHODIMP    EndUIElement(DWORD dwUIElementId);

        // ITfInputProcessorProfileActivationSink
        //   Notification for keyboard input locale change
        STDMETHODIMP    OnActivated(DWORD dwProfileType, LANGID langid, _In_ REFCLSID clsid, _In_ REFGUID catid,
            _In_ REFGUID guidProfile, HKL hkl, DWORD dwFlags);

        // ITfCompartmentEventSink
        //    Notification for open mode (toggle state) change
        STDMETHODIMP    OnChange(_In_ REFGUID rguid);

    private:
        LONG _cRef;
    };

    static void MakeReadingInformationString(ITfReadingInformationUIElement* preading);
    static void MakeCandidateStrings(ITfCandidateListUIElement* pcandidate);
    static ITfUIElement* GetUIElement(DWORD dwUIElementId);
    static BOOL GetCompartments(ITfCompartmentMgr** ppcm, ITfCompartment** ppTfOpenMode,
        ITfCompartment** ppTfConvMode);
    static BOOL SetupCompartmentSinks(BOOL bResetOnly = FALSE, ITfCompartment* pTfOpenMode = nullptr,
        ITfCompartment* ppTfConvMode = nullptr);

    static ITfThreadMgrEx* m_tm;
    static DWORD m_dwUIElementSinkCookie;
    static DWORD m_dwAlpnSinkCookie;
    static DWORD m_dwOpenModeSinkCookie;
    static DWORD m_dwConvModeSinkCookie;
    static CUIElementSink* m_TsfSink;
    static int m_nCandidateRefCount;	// Some IME shows multiple candidate lists but the Library doesn't support multiple candidate list. 
                                        // So track open / close events to make sure the candidate list opened last is shown.
    CTsfUiLessMode()
    {
    }	// this class can't be instanciated

public:
    static BOOL SetupSinks();
    static void ReleaseSinks();
    static BOOL CurrentInputLocaleIsIme();
    static void UpdateImeState(BOOL bResetCompartmentEventSink = FALSE);
    static void EnableUiUpdates(bool bEnable);
};

ITfThreadMgrEx*                 CTsfUiLessMode::m_tm;
DWORD                           CTsfUiLessMode::m_dwUIElementSinkCookie = TF_INVALID_COOKIE;
DWORD                           CTsfUiLessMode::m_dwAlpnSinkCookie = TF_INVALID_COOKIE;
DWORD                           CTsfUiLessMode::m_dwOpenModeSinkCookie = TF_INVALID_COOKIE;
DWORD                           CTsfUiLessMode::m_dwConvModeSinkCookie = TF_INVALID_COOKIE;
CTsfUiLessMode::CUIElementSink* CTsfUiLessMode::m_TsfSink = nullptr;
int                             CTsfUiLessMode::m_nCandidateRefCount = 0;

static unsigned long _strtoul(LPCSTR psz, LPTSTR*, int)
{
    if (!psz)
        return 0;

    ULONG ulRet = 0;
    if (psz[0] == '0' && (psz[1] == 'x' || psz[1] == 'X'))
    {
        psz += 2;
        ULONG ul = 0;
        while (*psz)
        {
            if ('0' <= *psz && *psz <= '9')
                ul = *psz - '0';
            else if ('A' <= *psz && *psz <= 'F')
                ul = *psz - 'A' + 10;
            else if ('a' <= *psz && *psz <= 'f')
                ul = *psz - 'a' + 10;
            else
                break;
            ulRet = ulRet * 16 + ul;
            psz++;
        }
    }
    else
    {
        while (*psz && ('0' <= *psz && *psz <= '9'))
        {
            ulRet = ulRet * 10 + (*psz - '0');
            psz++;
        }
    }
    return ulRet;
}

#define GetCharCount(psz) (int)wcslen(psz)
#define GetCharCountFromBytes(psz,iBytes) (iBytes)

static void ComposeCandidateLine(int index, LPCTSTR pszCandidate)
{
    LPTSTR psz = g_szCandidate[index];
    *psz++ = (TCHAR)(TEXT('0') + ((index + g_iCandListIndexBase) % 10));
    if (g_bVerticalCand)
    {
        *psz++ = TEXT(' ');
    }
    while (*pszCandidate && (COUNTOF(g_szCandidate[index]) > (psz - g_szCandidate[index])))
    {
        *psz++ = *pszCandidate++;
    }
    *psz = 0;
}

static void SendCompString()
{
    int i, iLen = (int)_tcslen(g_szCompositionString);
    if (ImeUiCallback_OnChar)
    {
        LPCWSTR pwz;
        pwz = g_szCompositionString;
        for (i = 0; i < iLen; i++)
        {
            ImeUiCallback_OnChar(pwz[i]);
        }
        return;
    }
    for (i = 0; i < iLen; i++)
    {
        SendKeyMsg(g_hwndCurr, WM_CHAR,
            (WPARAM)g_szCompositionString[i]
            );
    }
}

static DWORD GetCandidateList(HIMC himc, DWORD dwIndex, LPCANDIDATELIST* ppCandList)
{
    DWORD dwBufLen = _ImmGetCandidateList(himc, dwIndex, nullptr, 0);
    if (dwBufLen)
    {
        *ppCandList = (LPCANDIDATELIST)ImeUiCallback_Malloc(dwBufLen);
        dwBufLen = _ImmGetCandidateList(himc, dwIndex, *ppCandList, dwBufLen);
    }
    return dwBufLen;
}

static void SendControlKeys(UINT vk, UINT num)
{
    if (num == 0)
        return;
    for (UINT i = 0; i < num; i++)
    {
        SendKeyMsg_DOWN(g_hwndCurr, vk);
    }
    SendKeyMsg_UP(g_hwndCurr, vk);
}

// send key messages to erase composition string.
static void CancelCompString(HWND hwnd, bool bUseBackSpace = true, int iNewStrLen = 0)
{
    if (g_dwIMELevel != 3)
        return;
    int cc = GetCharCount(g_szCompositionString);
    int i;
    // move caret to the end of composition string
    SendControlKeys(VK_RIGHT, cc - g_IMECursorChars);

    if (bUseBackSpace || g_bInsertMode)
        iNewStrLen = 0;

    // The caller sets bUseBackSpace to false if there's possibility of sending 
    // new composition string to the app right after this function call.
    // 
    // If the app is in overwriting mode and new comp string is 
    // shorter than current one, delete previous comp string 
    // till it's same long as the new one. Then move caret to the beginning of comp string.
    // New comp string will overwrite old one.
    if (iNewStrLen < cc)
    {
        for (i = 0; i < cc - iNewStrLen; i++)
        {
            SendKeyMsg_DOWN(hwnd, VK_BACK);
            SendKeyMsg(hwnd, WM_CHAR, 8);	//Backspace character
        }
        SendKeyMsg_UP(hwnd, VK_BACK);
    }
    else
        iNewStrLen = cc;

    SendControlKeys(VK_LEFT, iNewStrLen);
}

// initialize composition string data.
static void InitCompStringData()
{
    g_IMECursorBytes = 0;
    g_IMECursorChars = 0;
    memset(&g_szCompositionString, 0, sizeof(g_szCompositionString));
    memset(&g_szCompAttrString, 0, sizeof(g_szCompAttrString));
}

static void CloseCandidateList()
{
    g_bCandList = false;
    if (!g_bReadingWindow)	// fix for Ent Gen #120.
    {
        g_dwCount = 0;
        memset(&g_szCandidate, 0, sizeof(g_szCandidate));
    }
}

//
//	ProcessIMEMessages()
//	Processes IME related messages and acquire information
//
#pragma warning(push)
#pragma warning( disable : 4616 6305 )
_Use_decl_annotations_
LPARAM ImeUi_ProcessMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam, bool* trapped)
{
    HIMC himc;
    int len;
    static LPARAM lAlt = 0x80000000, lCtrl = 0x80000000, lShift = 0x80000000;

    *trapped = false;
    if (!g_bInitialized || g_bDisableImeCompletely)
    {
        return 0;
    }

    Autolock _lock(ImeUiSyncRoot);

    switch (uMsg)
    {
        //
        //	IME Handling
        //
    case WM_INPUTLANGCHANGE:
        OnInputLangChange();
        break;

    case WM_IME_SETCONTEXT:
        //
        // We don't want anything to display, so we have to clear lParam and pass it to DefWindowProc().
        // Expecially important in Vista to receive IMN_CHANGECANDIDATE correctly.
        //
        lParam = 0;
        break;

    case WM_IME_STARTCOMPOSITION:
        InitCompStringData();
        *trapped = true;
        break;

    case WM_IME_COMPOSITION:
    {
        LONG lRet;
        TCHAR szCompStr[COUNTOF(g_szCompositionString)];

        *trapped = true;
        himc = ImmGetContext(hWnd);
        if (!himc)
        {
            break;
        }

        // ResultStr must be processed before composition string.
        if (lParam & GCS_RESULTSTR)
        {
            lRet = (LONG)_ImmGetCompositionString(himc, GCS_RESULTSTR, szCompStr,
                COUNTOF(szCompStr)) / sizeof(TCHAR);
            szCompStr[lRet] = 0;
            CancelCompString(g_hwndCurr, false, GetCharCount(szCompStr));
            wcscpy_s(g_szCompositionString, COUNTOF(g_szCompositionString), szCompStr);
            _SendCompString();
            InitCompStringData();
        }
        //
        // Reads in the composition string.
        //
        if (lParam & GCS_COMPSTR)
        {
            //////////////////////////////////////////////////////
            // Retrieve the latest user-selected IME candidates
            lRet = (LONG)_ImmGetCompositionString(himc, GCS_COMPSTR, szCompStr,
                COUNTOF(szCompStr)) / sizeof(TCHAR);
            szCompStr[lRet] = 0;
            //
            // Remove the whole of the string
            //
            CancelCompString(g_hwndCurr, false, GetCharCount(szCompStr));

            wcscpy_s(g_szCompositionString, COUNTOF(g_szCompositionString), szCompStr);
            lRet = _ImmGetCompositionString(himc, GCS_COMPATTR, g_szCompAttrString,
                COUNTOF(g_szCompAttrString));
            g_szCompAttrString[lRet] = 0;
            // Older CHT IME uses composition string for reading string
            if (GETLANG() == LANG_CHT && !GetImeId())
            {
                int i, chars = (int)wcslen(g_szCompositionString) / (3 - sizeof(TCHAR));
                if (chars)
                {
                    g_dwCount = 4;
                    g_dwSelection = (DWORD)-1;	// don't select any candidate

                    for (i = 3; i >= 0; i--)
                    {
                        if (i > chars - 1)
                            g_szCandidate[i][0] = 0;
                        else
                        {
                            g_szCandidate[i][0] = g_szCompositionString[i];
                            g_szCandidate[i][1] = 0;
                        }
                    }
                    g_uCandPageSize = MAX_CANDLIST;
                    memset(g_szCompositionString, 0, 8);
                    g_bReadingWindow = true;
                    GetReadingWindowOrientation(0);
                    if (g_bHorizontalReading)
                    {
                        g_iReadingError = -1;
                        g_szReadingString[0] = 0;
                        for (i = 0; i < (int)g_dwCount; i++)
                        {
                            if (g_dwSelection == (DWORD)i)
                                g_iReadingError = (int)wcslen(g_szReadingString);
                            LPCTSTR pszTmp = g_szCandidate[i];
                            wcscat_s(g_szReadingString, COUNTOF(g_szReadingString), pszTmp);
                        }
                    }
                }
                else
                    g_dwCount = 0;
            }

            // get caret position in composition string
            g_IMECursorBytes = _ImmGetCompositionString(himc, GCS_CURSORPOS, nullptr, 0);
            g_IMECursorChars = GetCharCountFromBytes(g_szCompositionString, g_IMECursorBytes);

            if (g_dwIMELevel == 3)
            {
                // send composition string via WM_CHAR
                _SendCompString();
                // move caret to appropreate location
                len = GetCharCount(g_szCompositionString + g_IMECursorBytes);
                SendControlKeys(VK_LEFT, len);
            }
        }
        _ImmReleaseContext(hWnd, himc);
    }
    break;

    case WM_IME_ENDCOMPOSITION:
        CancelCompString(g_hwndCurr);
        InitCompStringData();
        break;

    case WM_IME_NOTIFY:
        switch (wParam)
        {
        case IMN_SETCONVERSIONMODE:
        {
            // Disable CHT IME software keyboard.
            static bool bNoReentrance = false;
            if (LANG_CHT == GETLANG() && !bNoReentrance)
            {
                bNoReentrance = true;
                DWORD dwConvMode, dwSentMode;
                _ImmGetConversionStatus(g_himcOrg, &dwConvMode, &dwSentMode);
                const DWORD dwFlag = IME_CMODE_SOFTKBD | IME_CMODE_SYMBOL;
                if (dwConvMode & dwFlag)
                    _ImmSetConversionStatus(g_himcOrg, dwConvMode & ~dwFlag, dwSentMode);
            }
            bNoReentrance = false;
        }
        // fall through
        case IMN_SETOPENSTATUS:
            if (g_bUILessMode)
                break;
            CheckToggleState();
            break;

        case IMN_OPENCANDIDATE:
        case IMN_CHANGECANDIDATE:
            if (g_bUILessMode)
            {
                break;
            }
            {
                g_bCandList = true;
                *trapped = true;
                himc = _ImmGetContext(hWnd);
                if (!himc)
                    break;

                LPCANDIDATELIST lpCandList;
                DWORD dwIndex, dwBufLen;

                g_bReadingWindow = false;
                dwIndex = 0;
                dwBufLen = _GetCandidateList(himc, dwIndex, &lpCandList);

                if (dwBufLen)
                {
                    g_dwSelection = lpCandList->dwSelection;
                    g_dwCount = lpCandList->dwCount;

                    int startOfPage = 0;
                    if (GETLANG() == LANG_CHS && GetImeId())
                    {
                        // MSPY (CHS IME) has variable number of candidates in candidate window
                        // find where current page starts, and the size of current page
                        const int maxCandChar = 18 * (3 - sizeof(TCHAR));
                        UINT cChars = 0;
                        UINT i;
                        for (i = 0; i < g_dwCount; i++)
                        {
                            UINT uLen = (int)wcslen(
                                (LPTSTR)((UINT_PTR)lpCandList + lpCandList->dwOffset[i])) +
                                (3 - sizeof(TCHAR));
                            if (uLen + cChars > maxCandChar)
                            {
                                if (i > g_dwSelection)
                                {
                                    break;
                                }
                                startOfPage = i;
                                cChars = uLen;
                            }
                            else
                            {
                                cChars += uLen;
                            }
                        }
                        g_uCandPageSize = i - startOfPage;
                    }
                    else
                    {
                        g_uCandPageSize = std::min<UINT>(lpCandList->dwPageSize, MAX_CANDLIST);
                        startOfPage = g_bUILessMode ? lpCandList->dwPageStart :
                            (g_dwSelection / g_uCandPageSize) * g_uCandPageSize;
                    }

                    g_dwSelection = (GETLANG() == LANG_CHS && !GetImeId()) ? (DWORD)-1
                        : g_dwSelection - startOfPage;

                    memset(&g_szCandidate, 0, sizeof(g_szCandidate));
                    for (UINT i = startOfPage, j = 0;
                    (DWORD)i < lpCandList->dwCount && j < g_uCandPageSize;
                        i++, j++)
                    {
                        ComposeCandidateLine(j,
                            (LPTSTR)((UINT_PTR)lpCandList + lpCandList->dwOffset[i]));
                    }
                    ImeUiCallback_Free((HANDLE)lpCandList);
                    _ImmReleaseContext(hWnd, himc);

                    // don't display selection in candidate list in case of Korean and old Chinese IME.
                    if (GETPRIMLANG() == LANG_KOREAN ||
                        GETLANG() == LANG_CHT && !GetImeId())
                        g_dwSelection = (DWORD)-1;
                }
                break;
            }

        case IMN_CLOSECANDIDATE:
            if (g_bUILessMode)
            {
                break;
            }
            CloseCandidateList();
            *trapped = true;
            break;

            // Jun.16,2000 05:21 by yutaka.
        case IMN_PRIVATE:
        {
            if (!g_bCandList)
            {
                GetReadingString(hWnd);
            }
            // Trap some messages to hide reading window
            DWORD dwId = GetImeId();
            switch (dwId)
            {
            case IMEID_CHT_VER42:
            case IMEID_CHT_VER43:
            case IMEID_CHT_VER44:
            case IMEID_CHS_VER41:
            case IMEID_CHS_VER42:
                if ((lParam == 1) || (lParam == 2))
                {
                    *trapped = true;
                }
                break;
            case IMEID_CHT_VER50:
            case IMEID_CHT_VER51:
            case IMEID_CHT_VER52:
            case IMEID_CHT_VER60:
            case IMEID_CHS_VER53:
                if ((lParam == 16) || (lParam == 17) || (lParam == 26) || (lParam == 27) ||
                    (lParam == 28))
                {
                    *trapped = true;
                }
                break;
            }
        }
        break;

        default:
            *trapped = true;
            break;
        }
        break;

        // fix for #15386 - When Text Service Framework is installed in Win2K, Alt+Shift and Ctrl+Shift combination (to switch 
        // input locale / keyboard layout) doesn't send WM_KEYUP message for the key that is released first. We need to check
        // if these keys are actually up whenever we receive key up message for other keys.
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (!(lAlt & 0x80000000) && wParam != VK_MENU && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0)
        {
            PostMessageA(GetFocus(), WM_KEYUP, (WPARAM)VK_MENU, (lAlt & 0x01ff0000) | 0xC0000001);
        }
        else if (!(lCtrl & 0x80000000) && wParam != VK_CONTROL &&
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0)
        {
            PostMessageA(GetFocus(), WM_KEYUP, (WPARAM)VK_CONTROL, (lCtrl & 0x01ff0000) | 0xC0000001);
        }
        else if (!(lShift & 0x80000000) && wParam != VK_SHIFT && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0)
        {
            PostMessageA(GetFocus(), WM_KEYUP, (WPARAM)VK_SHIFT, (lShift & 0x01ff0000) | 0xC0000001);
        }
        // fall through WM_KEYDOWN / WM_SYSKEYDOWN
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        switch (wParam)
        {
        case VK_MENU:
            lAlt = lParam;
            break;
        case VK_SHIFT:
            lShift = lParam;
            break;
        case VK_CONTROL:
            lCtrl = lParam;
            break;
        }
    }
    break;
    }
    return 0;
}
#pragma warning(pop)

void ImeUi_SetState(_In_ DWORD dwState)
{
    if (!g_bInitialized)
        return;
    HIMC himc;
    if (dwState == IMEUI_STATE_ON)
    {
        ImeUi_EnableIme(true);
    }
    himc = _ImmGetContext(g_hwndCurr);
    if (himc)
    {
        if (g_bDisableImeCompletely)
            dwState = IMEUI_STATE_OFF;

        bool bOn = dwState == IMEUI_STATE_ON;	// for non-Chinese IME
        switch (GETPRIMLANG())
        {
        case LANG_CHINESE:
        {
            // toggle Chinese IME
            DWORD dwId;
            DWORD dwConvMode = 0, dwSentMode = 0;
            if ((g_bChineseIME && dwState == IMEUI_STATE_OFF) ||
                (!g_bChineseIME && dwState != IMEUI_STATE_OFF))
            {
                _ImmSimulateHotKey(g_hwndCurr, IME_THOTKEY_IME_NONIME_TOGGLE);
                _PumpMessage();
            }
            if (dwState != IMEUI_STATE_OFF)
            {
                dwId = GetImeId();
                if (dwId)
                {
                    _ImmGetConversionStatus(himc, &dwConvMode, &dwSentMode);
                    dwConvMode = (dwState == IMEUI_STATE_ON)
                        ? (dwConvMode | IME_CMODE_NATIVE)
                        : (dwConvMode & ~IME_CMODE_NATIVE);
                    _ImmSetConversionStatus(himc, dwConvMode, dwSentMode);
                }
            }
            break;
        }
        case LANG_KOREAN:
            // toggle Korean IME
            if ((bOn && g_dwState != IMEUI_STATE_ON) || (!bOn && g_dwState == IMEUI_STATE_ON))
            {
                _ImmSimulateHotKey(g_hwndCurr, IME_KHOTKEY_ENGLISH);
            }
            break;
        case LANG_JAPANESE:
            _ImmSetOpenStatus(himc, bOn);
            break;
        }
        _ImmReleaseContext(g_hwndCurr, himc);
        CheckToggleState();
    }
}

DWORD ImeUi_GetState()
{
    if (!g_bInitialized)
        return IMEUI_STATE_OFF;
    CheckToggleState();
    return g_dwState;
}

void ImeUi_EnableIme(_In_ bool bEnable)
{
    if (!g_bInitialized || !g_hwndCurr)
        return;
    if (g_bDisableImeCompletely)
        bEnable = false;

    if (g_hwndCurr == g_hwndMain)
    {
        HIMC himcDbg;
        himcDbg = _ImmAssociateContext(g_hwndCurr, bEnable ? g_himcOrg : nullptr);
    }
    g_bImeEnabled = bEnable;
    if (bEnable)
    {
        CheckToggleState();
    }
    CTsfUiLessMode::EnableUiUpdates(bEnable);
}

bool ImeUi_IsEnabled()
{
    return g_bImeEnabled;
}

bool ImeUi_Initialize(_In_  HWND hwnd, _In_ bool bDisable)
{
    if (g_bInitialized)
    {
        return true;
    }
    g_hwndMain = hwnd;
    g_disableCicero.Initialize();

    g_hImmDll = LoadLibraryEx(L"imm32.dll", nullptr, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    g_bDisableImeCompletely = false;

    if (g_hImmDll)
    {
        _ImmLockIMC = reinterpret_cast<LPINPUTCONTEXT2(WINAPI*)(HIMC hIMC)>(reinterpret_cast<void*>(GetProcAddress(g_hImmDll, "ImmLockIMC")));
        _ImmUnlockIMC = reinterpret_cast<BOOL(WINAPI*)(HIMC hIMC)>(reinterpret_cast<void*>(GetProcAddress(g_hImmDll, "ImmUnlockIMC")));
        _ImmLockIMCC = reinterpret_cast<LPVOID(WINAPI*)(HIMCC hIMCC)>(reinterpret_cast<void*>(GetProcAddress(g_hImmDll, "ImmLockIMCC")));
        _ImmUnlockIMCC = reinterpret_cast<BOOL(WINAPI*)(HIMCC hIMCC)>(reinterpret_cast<void*>(GetProcAddress(g_hImmDll, "ImmUnlockIMCC")));
        BOOL(WINAPI* _ImmDisableTextFrameService)(DWORD) = reinterpret_cast<BOOL(WINAPI*)(DWORD)>(reinterpret_cast<void*>(GetProcAddress(g_hImmDll,
            "ImmDisableTextFrameService")));
        if (_ImmDisableTextFrameService)
        {
            _ImmDisableTextFrameService((DWORD)-1);
        }
    }
    else
    {
        g_bDisableImeCompletely = true;
        return false;
    }
    _ImmGetCompositionString = ImmGetCompositionStringW;
    _ImmGetCandidateList = ImmGetCandidateListW;
    _GetCandidateList = GetCandidateList;
    _SendCompString = SendCompString;
    _SendMessage = SendMessageW;

    // turn init flag on so that subsequent calls to ImeUi functions work. 
    g_bInitialized = true;

    ImeUi_SetWindow(g_hwndMain);
    g_himcOrg = _ImmGetContext(g_hwndMain);
    _ImmReleaseContext(g_hwndMain, g_himcOrg);

    if (!g_himcOrg)
    {
        bDisable = true;
    }

    // the following pointers to function has to be initialized before this function is called.
    if (bDisable ||
        !ImeUiCallback_Malloc ||
        !ImeUiCallback_Free
        )
    {
        g_bDisableImeCompletely = true;
        ImeUi_EnableIme(false);
        g_bInitialized = bDisable;
        return false;
    }

    CheckInputLocale();
    OnInputLangChangeWorker();
    ImeUi_SetSupportLevel(2);

    // SetupTSFSinks has to be called before CheckToggleState to make it work correctly.
    g_bUILessMode = CTsfUiLessMode::SetupSinks() != FALSE;
    CheckToggleState();
    if (g_bUILessMode)
    {
        g_bChineseIME = (GETPRIMLANG() == LANG_CHINESE) && CTsfUiLessMode::CurrentInputLocaleIsIme();
        CTsfUiLessMode::UpdateImeState();
    }
    ImeUi_EnableIme(false);

    return true;
}

void ImeUi_Uninitialize()
{
    if (!g_bInitialized)
    {
        return;
    }
    CTsfUiLessMode::ReleaseSinks();
    if (g_hwndMain)
    {
        ImmAssociateContext(g_hwndMain, g_himcOrg);
    }
    g_hwndMain = nullptr;
    g_himcOrg = nullptr;
    if (g_hImmDll)
    {
        FreeLibrary(g_hImmDll);
        g_hImmDll = nullptr;
    }
    g_disableCicero.Uninitialize();
    g_bInitialized = false;
}

//
//	GetImeId( UINT uIndex )
//		returns 
//	returned value:
//	0: In the following cases
//		- Non Chinese IME input locale
//		- Older Chinese IME
//		- Other error cases
//
//	Othewise:
//      When uIndex is 0 (default)
//			bit 31-24:	Major version
//			bit 23-16:	Minor version
//			bit 15-0:	Language ID
//		When uIndex is 1
//			pVerFixedInfo->dwFileVersionLS
//
//	Use IMEID_VER and IMEID_LANG macro to extract version and language information.
//	
static DWORD GetImeId(_In_ UINT uIndex)
{
    static HKL hklPrev = 0;
    static DWORD dwRet[2] =
    {
        0, 0
    };

    DWORD dwVerSize;
    DWORD dwVerHandle;
    LPVOID lpVerBuffer;
    LPVOID lpVerData;
    UINT cbVerData;
    char szTmp[1024];

    if (uIndex >= sizeof(dwRet) / sizeof(dwRet[0]))
        return 0;

    HKL kl = g_hklCurrent;
    if (hklPrev == kl)
    {
        return dwRet[uIndex];
    }
    hklPrev = kl;
    DWORD dwLang = (static_cast<DWORD>(reinterpret_cast<UINT_PTR>(kl)) & 0xffff);

    if (g_bUILessMode && GETLANG() == LANG_CHT)
    {
        // In case of Vista, artifitial value is returned so that it's not considered as older IME.
        dwRet[0] = IMEID_CHT_VER_VISTA;
        dwRet[1] = 0;
        return dwRet[0];
    }

    if (kl != _CHT_HKL_NEW_PHONETIC && kl != _CHT_HKL_NEW_CHANG_JIE
        && kl != _CHT_HKL_NEW_QUICK && kl != _CHT_HKL_HK_CANTONESE && kl != _CHS_HKL)
    {
        goto error;
    }

    if (_ImmGetIMEFileNameA(kl, szTmp, sizeof(szTmp) - 1) <= 0)
    {
        goto error;
    }

    if (!_GetReadingString)	// IME that doesn't implement private API
    {
#define LCID_INVARIANT MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT)
        if ((CompareStringA(LCID_INVARIANT, NORM_IGNORECASE, szTmp, -1, _CHT_IMEFILENAME, -1) != 2)
            && (CompareStringA(LCID_INVARIANT, NORM_IGNORECASE, szTmp, -1, _CHT_IMEFILENAME2, -1) != 2)
            && (CompareStringA(LCID_INVARIANT, NORM_IGNORECASE, szTmp, -1, _CHT_IMEFILENAME3, -1) != 2)
            && (CompareStringA(LCID_INVARIANT, NORM_IGNORECASE, szTmp, -1, _CHS_IMEFILENAME, -1) != 2)
            && (CompareStringA(LCID_INVARIANT, NORM_IGNORECASE, szTmp, -1, _CHS_IMEFILENAME2, -1) != 2)
            )
        {
            goto error;
        }
    }

    dwVerSize = GetFileVersionInfoSizeA(szTmp, &dwVerHandle);
    if (dwVerSize)
    {
        lpVerBuffer = (LPVOID)ImeUiCallback_Malloc(dwVerSize);
        if (lpVerBuffer)
        {
            if (GetFileVersionInfoA(szTmp, 0, dwVerSize, lpVerBuffer))
            {
                if (VerQueryValueA(lpVerBuffer, "\\", &lpVerData, &cbVerData))
                {
#define pVerFixedInfo ((VS_FIXEDFILEINFO FAR*)lpVerData)
                    DWORD dwVer = pVerFixedInfo->dwFileVersionMS;
                    dwVer = (dwVer & 0x00ff0000) << 8 | (dwVer & 0x000000ff) << 16;
                    if (_GetReadingString ||
                        dwLang == LANG_CHT && (
                            dwVer == MAKEIMEVERSION(4, 2) ||
                            dwVer == MAKEIMEVERSION(4, 3) ||
                            dwVer == MAKEIMEVERSION(4, 4) ||
                            dwVer == MAKEIMEVERSION(5, 0) ||
                            dwVer == MAKEIMEVERSION(5, 1) ||
                            dwVer == MAKEIMEVERSION(5, 2) ||
                            dwVer == MAKEIMEVERSION(6, 0))
                        ||
                        dwLang == LANG_CHS && (
                            dwVer == MAKEIMEVERSION(4, 1) ||
                            dwVer == MAKEIMEVERSION(4, 2) ||
                            dwVer == MAKEIMEVERSION(5, 3)))
                    {
                        dwRet[0] = dwVer | dwLang;
                        dwRet[1] = pVerFixedInfo->dwFileVersionLS;
                        ImeUiCallback_Free(lpVerBuffer);
                        return dwRet[0];
                    }
#undef pVerFixedInfo
                }
            }
            ImeUiCallback_Free(lpVerBuffer);
        }
    }
    // The flow comes here in the following conditions
    // - Non Chinese IME input locale
    // - Older Chinese IME
    // - Other error cases
error:
    dwRet[0] = dwRet[1] = 0;
    return dwRet[uIndex];
}

static void GetReadingString(_In_ HWND hWnd)
{
    if (g_bUILessMode)
    {
        return;
    }
    DWORD dwId = GetImeId();
    if (!dwId)
    {
        return;
    }

    HIMC himc;
    himc = _ImmGetContext(hWnd);
    if (!himc)
        return;

    DWORD dwlen = 0;
    DWORD dwerr = 0;
    WCHAR wzBuf[16];	// We believe 16 wchars are big enough to hold reading string after having discussion with CHT IME team.
    WCHAR* wstr = wzBuf;
    bool unicode = FALSE;
    LPINPUTCONTEXT2 lpIMC = nullptr;

    if (_GetReadingString)
    {
        BOOL bVertical;
        UINT uMaxUiLen;
        dwlen = _GetReadingString(himc, 0, nullptr, (PINT)&dwerr, &bVertical, &uMaxUiLen);
        if (dwlen)
        {
            if (dwlen > COUNTOF(wzBuf))
            {
                dwlen = COUNTOF(wzBuf);
            }
            dwlen = _GetReadingString(himc, dwlen, wstr, (PINT)&dwerr, &bVertical, &uMaxUiLen);
        }

        g_bHorizontalReading = bVertical == 0;
        unicode = true;
    }
    else //	IMEs that doesn't implement Reading String API
    {
        lpIMC = _ImmLockIMC(himc);

        // *** hacking code from Michael Yang ***

        LPBYTE p = 0;

        switch (dwId)
        {

        case IMEID_CHT_VER42: // New(Phonetic/ChanJie)IME98  : 4.2.x.x // Win98
        case IMEID_CHT_VER43: // New(Phonetic/ChanJie)IME98a : 4.3.x.x // WinMe, Win2k
        case IMEID_CHT_VER44: // New ChanJie IME98b          : 4.4.x.x // WinXP

            p = *(LPBYTE*)((LPBYTE)_ImmLockIMCC(lpIMC->hPrivate) + 24);
            if (!p) break;
            dwlen = *(DWORD*)(p + 7 * 4 + 32 * 4);	//m_dwInputReadStrLen
            dwerr = *(DWORD*)(p + 8 * 4 + 32 * 4);	//m_dwErrorReadStrStart
            wstr = (WCHAR*)(p + 56);
            unicode = TRUE;
            break;

        case IMEID_CHT_VER50: // 5.0.x.x // WinME

            p = *(LPBYTE*)((LPBYTE)_ImmLockIMCC(lpIMC->hPrivate) + 3 * 4); // PCKeyCtrlManager
            if (!p) break;
            p = *(LPBYTE*)((LPBYTE)p + 1 * 4 + 5 * 4 + 4 * 2); // = PCReading = &STypingInfo
            if (!p) break;
            dwlen = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16);		//m_dwDisplayStringLength;
            dwerr = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16 + 1 * 4);	//m_dwDisplayErrorStart;
            wstr = (WCHAR*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4);
            unicode = FALSE;
            break;

        case IMEID_CHT_VER51: // 5.1.x.x // IME2002(w/OfficeXP)
        case IMEID_CHT_VER52: // 5.2.x.x // (w/whistler)
        case IMEID_CHS_VER53: // 5.3.x.x // SCIME2k or MSPY3 (w/OfficeXP and Whistler)

            p = *(LPBYTE*)((LPBYTE)_ImmLockIMCC(lpIMC->hPrivate) + 4);   // PCKeyCtrlManager
            if (!p) break;
            p = *(LPBYTE*)((LPBYTE)p + 1 * 4 + 5 * 4);                       // = PCReading = &STypingInfo
            if (!p) break;
            dwlen = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16 * 2);		//m_dwDisplayStringLength;
            dwerr = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16 * 2 + 1 * 4);	//m_dwDisplayErrorStart;
            wstr = (WCHAR*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4);
            unicode = TRUE;
            break;

            // the code tested only with Win 98 SE (MSPY 1.5/ ver 4.1.0.21)
        case IMEID_CHS_VER41:
        {
            int offset;
            offset = (GetImeId(1) >= 0x00000002) ? 8 : 7;

            p = *(LPBYTE*)((LPBYTE)_ImmLockIMCC(lpIMC->hPrivate) + offset * 4);
            if (!p) break;
            dwlen = *(DWORD*)(p + 7 * 4 + 16 * 2 * 4);
            dwerr = *(DWORD*)(p + 8 * 4 + 16 * 2 * 4);
            dwerr = std::min(dwerr, dwlen);
            wstr = (WCHAR*)(p + 6 * 4 + 16 * 2 * 1);
            unicode = TRUE;
            break;
        }

        case IMEID_CHS_VER42: // 4.2.x.x // SCIME98 or MSPY2 (w/Office2k, Win2k, WinME, etc)
        {
            int nTcharSize = IsNT() ? sizeof(WCHAR) : sizeof(char);
            p = *(LPBYTE*)((LPBYTE)_ImmLockIMCC(lpIMC->hPrivate) + 1 * 4 + 1 * 4 + 6 * 4); // = PCReading = &STypintInfo
            if (!p) break;
            dwlen = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16 * nTcharSize);		//m_dwDisplayStringLength;
            dwerr = *(DWORD*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4 + 16 * nTcharSize + 1 * 4);	//m_dwDisplayErrorStart;
            wstr = (WCHAR*)(p + 1 * 4 + (16 * 2 + 2 * 4) + 5 * 4);                 //m_tszDisplayString
            unicode = IsNT() ? TRUE : FALSE;
        }
        }	// switch

        g_szCandidate[0][0] = 0;
        g_szCandidate[1][0] = 0;
        g_szCandidate[2][0] = 0;
        g_szCandidate[3][0] = 0;
    }
    g_dwCount = dwlen;
    g_dwSelection = (DWORD)-1; // do not select any char
    if (unicode)
    {
        int i;
        for (i = 0; (DWORD)i < dwlen; i++) // dwlen > 0, if known IME : yutakah
        {
            if (dwerr <= (DWORD)i && g_dwSelection == (DWORD)-1)
            { // select error char
                g_dwSelection = i;
            }
            g_szCandidate[i][0] = wstr[i];
            g_szCandidate[i][1] = 0;
        }
        g_szCandidate[i][0] = 0;
    }
    else
    {
        char* p = (char*)wstr;
        int i, j;
        for (i = 0, j = 0; (DWORD)i < dwlen; i++, j++) // dwlen > 0, if known IME : yutakah
        {
            if (dwerr <= (DWORD)i && g_dwSelection == (DWORD)-1)
            {
                g_dwSelection = (DWORD)j;
            }
            MultiByteToWideChar(g_uCodePage, 0, p + i, 1 + (_IsLeadByte(p[i]) ? 1 : 0),
                g_szCandidate[j], 1);
            if (_IsLeadByte(p[i]))
            {
                i++;
            }
        }
        g_szCandidate[j][0] = 0;
        g_dwCount = j;
    }
    if (!_GetReadingString)
    {
        _ImmUnlockIMCC(lpIMC->hPrivate);
        _ImmUnlockIMC(himc);
        GetReadingWindowOrientation(dwId);
    }
    _ImmReleaseContext(hWnd, himc);

    g_bReadingWindow = true;
    if (g_bHorizontalReading)
    {
        g_iReadingError = -1;
        g_szReadingString[0] = 0;
        for (UINT i = 0; i < g_dwCount; i++)
        {
            if (g_dwSelection == (DWORD)i)
                g_iReadingError = (int)wcslen(g_szReadingString);
            LPCTSTR pszTmp = g_szCandidate[i];
            wcscat_s(g_szReadingString, COUNTOF(g_szReadingString), pszTmp);
        }
    }
    g_uCandPageSize = MAX_CANDLIST;
}


static struct
{
    bool m_bCtrl;
    bool m_bShift;
    bool m_bAlt;
    UINT m_uVk;
}
aHotKeys[] =
{
    false,	false,	false,	VK_APPS,
    true,	false,	false,	'8',
    true,	false,	false,	'Y',
    true,	false,	false,	VK_DELETE,
    true,	false,	false,	VK_F7,
    true,	false,	false,	VK_F9,
    true,	false,	false,	VK_F10,
    true,	false,	false,	VK_F11,
    true,	false,	false,	VK_F12,
    false,	false,	false,	VK_F2,
    false,	false,	false,	VK_F3,
    false,	false,	false,	VK_F4,
    false,	false,	false,	VK_F5,
    false,	false,	false,	VK_F10,
    false,	true,	false,	VK_F6,
    false,	true,	false,	VK_F7,
    false,	true,	false,	VK_F8,
    true,	true,	false,	VK_F10,
    true,	true,	false,	VK_F11,
    true,	false,	false,	VK_CONVERT,
    true,	false,	false,	VK_SPACE,
    true,	false,	true,	0xbc,	// Alt + Ctrl + ',': SW keyboard for Trad. Chinese IME
    true,   false,  false,  VK_TAB, // ATOK2005's Ctrl+TAB
};

//
// Ignores specific keys when IME is on. Returns true if the message is a hot key to ignore.
// - Caller doesn't have to check whether IME is on.
// - This function must be called before TranslateMessage() is called.
//
bool ImeUi_IgnoreHotKey(_In_ const MSG* pmsg)
{
    if (!g_bInitialized || !pmsg)
        return false;

    if (pmsg->wParam == VK_PROCESSKEY && (pmsg->message == WM_KEYDOWN || pmsg->message == WM_SYSKEYDOWN))
    {
        bool bCtrl, bShift, bAlt;
        UINT uVkReal = _ImmGetVirtualKey(pmsg->hwnd);
        // special case #1 - VK_JUNJA toggles half/full width input mode in Korean IME.
        // This VK (sent by Alt+'=' combo) is ignored regardless of the modifier state.
        if (uVkReal == VK_JUNJA)
        {
            return true;
        }
        // special case #2 - disable right arrow key that switches the candidate list to expanded mode in CHT IME.
        if (uVkReal == VK_RIGHT && g_bCandList && GETLANG() == LANG_CHT)
        {
            return true;
        }
#ifndef ENABLE_HANJA_KEY
        // special case #3 - we disable VK_HANJA key because 1. some Korean fonts don't Hanja and 2. to reduce testing cost.
        if (uVkReal == VK_HANJA && GETPRIMLANG() == LANG_KOREAN)
        {
            return true;
        }
#endif
        bCtrl = (GetKeyState(VK_CONTROL) & 0x8000) ? true : false;
        bShift = (GetKeyState(VK_SHIFT) & 0x8000) ? true : false;
        bAlt = (GetKeyState(VK_MENU) & 0x8000) ? true : false;
        for (int i = 0; i < COUNTOF(aHotKeys); i++)
        {
            if (aHotKeys[i].m_bCtrl == bCtrl &&
                aHotKeys[i].m_bShift == bShift &&
                aHotKeys[i].m_bAlt == bAlt &&
                aHotKeys[i].m_uVk == uVkReal)
                return true;
        }
    }
    return false;
}

void ImeUi_FinalizeString(_In_ bool bSend)
{
    HIMC himc;
    static bool bProcessing = false; // to avoid infinite recursion
    if (!g_bInitialized || bProcessing)
        return;

    himc = _ImmGetContext(g_hwndCurr);
    if (!himc)
        return;
    bProcessing = true;

    if (g_dwIMELevel == 2 && bSend)
    {
        // Send composition string to app.
        LONG lRet = (int)wcslen(g_szCompositionString);
        assert(lRet >= 2);
        // In case of CHT IME, don't send the trailing double byte space, if it exists.
        if (GETLANG() == LANG_CHT && (lRet >= 1)
            && g_szCompositionString[lRet - 1] == 0x3000)
        {
            lRet--;
        }
        _SendCompString();
    }

    InitCompStringData();
    // clear composition string in IME
    _ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    if (g_bUILessMode)
    {
        // For some reason ImmNotifyIME doesn't work on DaYi and Array CHT IMEs. Cancel composition string by setting zero-length string.
        ImmSetCompositionString(himc, SCS_SETSTR, TEXT(""), sizeof(TCHAR), TEXT(""), sizeof(TCHAR));
    }
    // the following line is necessary as Korean IME doesn't close cand list when comp string is cancelled.
    _ImmNotifyIME(himc, NI_CLOSECANDIDATE, 0, 0);
    _ImmReleaseContext(g_hwndCurr, himc);
    // Zooty2 RAID #4759: Sometimes application doesn't receive IMN_CLOSECANDIDATE on Alt+Tab
    // So the same code for IMN_CLOSECANDIDATE is replicated here.
    CloseCandidateList();
    bProcessing = false;
    return;
}

static void SetSupportLevel(_In_ DWORD dwImeLevel)
{
    if (dwImeLevel < 2 || 3 < dwImeLevel)
        return;
    if (GETPRIMLANG() == LANG_KOREAN)
    {
        dwImeLevel = 3;
    }
    g_dwIMELevel = dwImeLevel;
    // cancel current composition string.
    ImeUi_FinalizeString();
}

void ImeUi_SetSupportLevel(_In_ DWORD dwImeLevel)
{
    if (!g_bInitialized)
        return;
    g_dwIMELevelSaved = dwImeLevel;
    SetSupportLevel(dwImeLevel);
}

static void CheckToggleState()
{
    CheckInputLocale();

    // In Vista, we have to use TSF since few IMM functions don't work as expected.
    // WARNING: Because of timing, g_dwState and g_bChineseIME may not be updated 
    // immediately after the change on IME states by user.
    if (g_bUILessMode)
    {
        return;
    }

    bool bIme = _ImmIsIME(g_hklCurrent) != 0
        && ((0xF0000000 & static_cast<DWORD>(reinterpret_cast<UINT_PTR>(g_hklCurrent))) == 0xE0000000); // Hack to detect IME correctly. When IME is running as TIP, ImmIsIME() returns true for CHT US keyboard.
    g_bChineseIME = (GETPRIMLANG() == LANG_CHINESE) && bIme;

    HIMC himc = _ImmGetContext(g_hwndCurr);
    if (himc)
    {
        if (g_bChineseIME)
        {
            DWORD dwConvMode, dwSentMode;
            _ImmGetConversionStatus(himc, &dwConvMode, &dwSentMode);
            g_dwState = (dwConvMode & IME_CMODE_NATIVE) ? IMEUI_STATE_ON : IMEUI_STATE_ENGLISH;
        }
        else
        {
            g_dwState = (bIme && _ImmGetOpenStatus(himc) != 0) ? IMEUI_STATE_ON : IMEUI_STATE_OFF;
        }
        _ImmReleaseContext(g_hwndCurr, himc);
    }
    else
        g_dwState = IMEUI_STATE_OFF;
}

void ImeUi_SetInsertMode(_In_ bool bInsert)
{
    if (!g_bInitialized)
        return;
    g_bInsertMode = bInsert;
}

bool ImeUi_GetCaretStatus()
{
    return !g_bInitialized || !g_szCompositionString[0];
}

// this function is used only in brief time in CHT IME handling, so accelerator isn't processed.
static void _PumpMessage()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE))
    {
        if (!GetMessageA(&msg, nullptr, 0, 0))
        {
            PostQuitMessage(msg.wParam);
            return;
        }
        //		if (0 == TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        //		}
    }
}

static void GetReadingWindowOrientation(_In_ DWORD dwId)
{
    g_bHorizontalReading = (g_hklCurrent == _CHS_HKL) || (g_hklCurrent == _CHT_HKL_NEW_CHANG_JIE) || (dwId == 0);
    if (!g_bHorizontalReading && IMEID_LANG(dwId) == LANG_CHT)
    {
        char szRegPath[MAX_PATH];
        HKEY hkey;
        DWORD dwVer = IMEID_VER(dwId);
        strcpy_s(szRegPath, COUNTOF(szRegPath), "software\\microsoft\\windows\\currentversion\\");
        strcat_s(szRegPath, COUNTOF(szRegPath), (dwVer >= MAKEIMEVERSION(5, 1)) ? "MSTCIPH" : "TINTLGNT");
        LONG lRc = RegOpenKeyExA(HKEY_CURRENT_USER, szRegPath, 0, KEY_READ, &hkey);
        if (lRc == ERROR_SUCCESS)
        {
            DWORD dwSize = sizeof(DWORD), dwMapping, dwType;
            lRc = RegQueryValueExA(hkey, "keyboard mapping", nullptr, &dwType, (PBYTE)&dwMapping, &dwSize);
            if (lRc == ERROR_SUCCESS)
            {
                if (
                    (dwVer <= MAKEIMEVERSION(5, 0) &&
                        ((BYTE)dwMapping == 0x22 || (BYTE)dwMapping == 0x23)
                        ) ||
                    ((dwVer == MAKEIMEVERSION(5, 1) || dwVer == MAKEIMEVERSION(5, 2)) &&
                        ((BYTE)dwMapping >= 0x22 && (BYTE)dwMapping <= 0x24)
                        )
                    )
                {
                    g_bHorizontalReading = true;
                }
            }
            RegCloseKey(hkey);
        }
    }
}

void ImeUi_ToggleLanguageBar(_In_ BOOL bRestore)
{
    static BOOL prevRestore = TRUE;
    bool bCheck = (prevRestore == TRUE || bRestore == TRUE);
    prevRestore = bRestore;
    if (!bCheck)
        return;

    static int iShowStatusWindow = -1;
    if (iShowStatusWindow == -1)
    {
        iShowStatusWindow = IsNT() && g_osi.dwMajorVersion >= 5 &&
            (g_osi.dwMinorVersion > 1 || (g_osi.dwMinorVersion == 1 && strlen(g_osi.szCSDVersion))) ? 1 : 0;
    }
    HWND hwndImeDef = _ImmGetDefaultIMEWnd(g_hwndCurr);
    if (hwndImeDef && bRestore && iShowStatusWindow)
        SendMessageA(hwndImeDef, WM_IME_CONTROL, IMC_OPENSTATUSWINDOW, 0);
    HRESULT hr;
    hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr))
    {
        ITfLangBarMgr* plbm = nullptr;
        hr = CoCreateInstance(CLSID_TF_LangBarMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfLangBarMgr),
            (void**)&plbm);
        if (SUCCEEDED(hr) && plbm)
        {
            DWORD dwCur;
            ULONG uRc;
            if (SUCCEEDED(hr))
            {
                if (bRestore)
                {
                    if (g_dwPrevFloat)
                        hr = plbm->ShowFloating(g_dwPrevFloat);
                }
                else
                {
                    hr = plbm->GetShowFloatingStatus(&dwCur);
                    if (SUCCEEDED(hr))
                        g_dwPrevFloat = dwCur;
                    if (!(g_dwPrevFloat & TF_SFT_DESKBAND))
                    {
                        hr = plbm->ShowFloating(TF_SFT_HIDDEN);
                    }
                }
            }
            uRc = plbm->Release();
        }
        CoUninitialize();
    }
    if (hwndImeDef && !bRestore)
    {
        // The following OPENSTATUSWINDOW is required to hide ATOK16 toolbar (FS9:#7546)
        SendMessageA(hwndImeDef, WM_IME_CONTROL, IMC_OPENSTATUSWINDOW, 0);
        SendMessageA(hwndImeDef, WM_IME_CONTROL, IMC_CLOSESTATUSWINDOW, 0);
    }
}

bool ImeUi_IsSendingKeyMessage()
{
    return bIsSendingKeyMessage;
}

static void OnInputLangChangeWorker()
{
    if (!g_bUILessMode)
    {
        g_iCandListIndexBase = (g_hklCurrent == _CHT_HKL_DAYI) ? 0 : 1;
    }
    SetImeApi();
}

static void OnInputLangChange()
{
    UINT uLang = GETPRIMLANG();
    CheckToggleState();
    OnInputLangChangeWorker();
    if (uLang != GETPRIMLANG())
    {
        // Korean IME always uses level 3 support.
        // Other languages use the level that is specified by ImeUi_SetSupportLevel()
        SetSupportLevel((GETPRIMLANG() == LANG_KOREAN) ? 3 : g_dwIMELevelSaved);
    }
    HWND hwndImeDef = _ImmGetDefaultIMEWnd(g_hwndCurr);
    if (hwndImeDef)
    {
        // Fix for Zooty #3995: prevent CHT IME toobar from showing up
        SendMessageA(hwndImeDef, WM_IME_CONTROL, IMC_OPENSTATUSWINDOW, 0);
        SendMessageA(hwndImeDef, WM_IME_CONTROL, IMC_CLOSESTATUSWINDOW, 0);
    }
}

static void SetImeApi()
{
    _GetReadingString = nullptr;
    _ShowReadingWindow = nullptr;
    if (g_bUILessMode)
        return;

    char szImeFile[MAX_PATH + 1];
    HKL kl = g_hklCurrent;
    if (_ImmGetIMEFileNameA(kl, szImeFile, sizeof(szImeFile) - 1) <= 0)
        return;
    HMODULE hIme = LoadLibraryExA(szImeFile, nullptr, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    if (!hIme)
        return;
    _GetReadingString = reinterpret_cast<UINT(WINAPI*)(HIMC, UINT, LPWSTR, PINT, BOOL*, PUINT)>(reinterpret_cast<void*>(GetProcAddress(hIme, "GetReadingString")));
    _ShowReadingWindow = reinterpret_cast<BOOL(WINAPI*)(HIMC himc, BOOL)>(reinterpret_cast<void*>(GetProcAddress(hIme, "ShowReadingWindow")));
    if (_ShowReadingWindow)
    {
        HIMC himc = _ImmGetContext(g_hwndCurr);
        if (himc)
        {
            _ShowReadingWindow(himc, false);
            _ImmReleaseContext(g_hwndCurr, himc);
        }
    }
}

static void CheckInputLocale()
{
    static HKL hklPrev = 0;
    g_hklCurrent = GetKeyboardLayout(0);
    if (hklPrev == g_hklCurrent)
    {
        return;
    }
    hklPrev = g_hklCurrent;
    switch (GETPRIMLANG())
    {
        // Simplified Chinese
    case LANG_CHINESE:
        g_bVerticalCand = true;
        switch (GETSUBLANG())
        {
        case SUBLANG_CHINESE_SIMPLIFIED:
            g_pszIndicatior = g_aszIndicator[INDICATOR_CHS];
            //g_bVerticalCand = GetImeId() == 0;
            g_bVerticalCand = false;
            break;
        case SUBLANG_CHINESE_TRADITIONAL:
            g_pszIndicatior = g_aszIndicator[INDICATOR_CHT];
            break;
        default:	// unsupported sub-language
            g_pszIndicatior = g_aszIndicator[INDICATOR_NON_IME];
            break;
        }
        break;
        // Korean
    case LANG_KOREAN:
        g_pszIndicatior = g_aszIndicator[INDICATOR_KOREAN];
        g_bVerticalCand = false;
        break;
        // Japanese
    case LANG_JAPANESE:
        g_pszIndicatior = g_aszIndicator[INDICATOR_JAPANESE];
        g_bVerticalCand = true;
        break;
    default:
        g_pszIndicatior = g_aszIndicator[INDICATOR_NON_IME];
    }
    char szCodePage[8];
    (void)GetLocaleInfoA(MAKELCID(GETLANG(), SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE, szCodePage,
        COUNTOF(szCodePage));
    g_uCodePage = _strtoul(szCodePage, nullptr, 0);
    for (int i = 0; i < 256; i++)
    {
        LeadByteTable[i] = (BYTE)IsDBCSLeadByteEx(g_uCodePage, (BYTE)i);
    }
}

void ImeUi_SetWindow(_In_ HWND hwnd)
{
    g_hwndCurr = hwnd;
    g_disableCicero.DisableCiceroOnThisWnd(hwnd);
}

UINT ImeUi_GetInputCodePage()
{
    return g_uCodePage;
}

DWORD ImeUi_GetFlags()
{
    return g_dwImeUiFlags;
}

void ImeUi_SetFlags(_In_ DWORD dwFlags, _In_ bool bSet)
{
    if (bSet)
    {
        g_dwImeUiFlags |= dwFlags;
    }
    else
    {
        g_dwImeUiFlags &= ~dwFlags;
    }
}

///////////////////////////////////////////////////////////////////////////////
//
//	CTsfUiLessMode methods
//
///////////////////////////////////////////////////////////////////////////////

//
//	SetupSinks()
//	Set up sinks. A sink is used to receive a Text Service Framework event.
//  CUIElementSink implements multiple sink interfaces to receive few different TSF events.
//
BOOL CTsfUiLessMode::SetupSinks()
{
    // ITfThreadMgrEx is available on Vista or later.
    HRESULT hr;
    hr = CoCreateInstance(CLSID_TF_ThreadMgr,
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(ITfThreadMgrEx),
        (void**)&m_tm);

    if (hr != S_OK)
    {
        return FALSE;
    }

    // ready to start interacting
    TfClientId cid;	// not used
    if (FAILED(m_tm->ActivateEx(&cid, TF_TMAE_UIELEMENTENABLEDONLY)))
    {
        return FALSE;
    }

    // Setup sinks
    BOOL bRc = FALSE;
    m_TsfSink = new (std::nothrow) CUIElementSink();
    if (m_TsfSink)
    {
        ITfSource* srcTm;
        if (SUCCEEDED(hr = m_tm->QueryInterface(__uuidof(ITfSource), (void**)&srcTm)))
        {
            // Sink for reading window change
            if (SUCCEEDED(hr = srcTm->AdviseSink(__uuidof(ITfUIElementSink), (ITfUIElementSink*)m_TsfSink,
                &m_dwUIElementSinkCookie)))
            {
                // Sink for input locale change
                if (SUCCEEDED(hr = srcTm->AdviseSink(__uuidof(ITfInputProcessorProfileActivationSink),
                    (ITfInputProcessorProfileActivationSink*)m_TsfSink,
                    &m_dwAlpnSinkCookie)))
                {
                    if (SetupCompartmentSinks())	// Setup compartment sinks for the first time
                    {
                        bRc = TRUE;
                    }
                }
            }
            srcTm->Release();
        }
    }
    return bRc;
}

void CTsfUiLessMode::ReleaseSinks()
{
    HRESULT hr;
    ITfSource* source;

    // Remove all sinks
    if (m_tm && SUCCEEDED(m_tm->QueryInterface(__uuidof(ITfSource), (void**)&source)))
    {
        hr = source->UnadviseSink(m_dwUIElementSinkCookie);
        hr = source->UnadviseSink(m_dwAlpnSinkCookie);
        source->Release();
        SetupCompartmentSinks(TRUE);	// Remove all compartment sinks
        m_tm->Deactivate();
        SAFE_RELEASE(m_tm);
        SAFE_RELEASE(m_TsfSink);
    }
}

CTsfUiLessMode::CUIElementSink::CUIElementSink()
{
    _cRef = 1;
}


CTsfUiLessMode::CUIElementSink::~CUIElementSink()
{
}

STDAPI CTsfUiLessMode::CUIElementSink::QueryInterface(_In_ REFIID riid, _COM_Outptr_ void** ppvObj)
{
    if (!ppvObj)
        return E_INVALIDARG;

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown))
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ITfUIElementSink*>(this));
    }
    else if (IsEqualIID(riid, __uuidof(ITfUIElementSink)))
    {
        *ppvObj = (ITfUIElementSink*)this;
    }
    else if (IsEqualIID(riid, __uuidof(ITfInputProcessorProfileActivationSink)))
    {
        *ppvObj = (ITfInputProcessorProfileActivationSink*)this;
    }
    else if (IsEqualIID(riid, __uuidof(ITfCompartmentEventSink)))
    {
        *ppvObj = (ITfCompartmentEventSink*)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDAPI_(ULONG)
CTsfUiLessMode::CUIElementSink::AddRef()
{
    return ++_cRef;
}

STDAPI_(ULONG)
CTsfUiLessMode::CUIElementSink::Release()
{
    LONG cr = --_cRef;

    if (_cRef == 0)
    {
        delete this;
    }

    return cr;
}

STDAPI CTsfUiLessMode::CUIElementSink::BeginUIElement(DWORD dwUIElementId, BOOL* pbShow)
{
    Autolock _lock(ImeUiSyncRoot);

    auto pElement = GetUIElement(dwUIElementId);
    if (!pElement)
        return E_INVALIDARG;

    ITfReadingInformationUIElement* preading = nullptr;
    ITfCandidateListUIElement* pcandidate = nullptr;
    *pbShow = FALSE;
    if (!g_bCandList && SUCCEEDED(pElement->QueryInterface(__uuidof(ITfReadingInformationUIElement),
        (void**)&preading)))
    {
        MakeReadingInformationString(preading);
        preading->Release();
    }
    else if (SUCCEEDED(pElement->QueryInterface(__uuidof(ITfCandidateListUIElement),
        (void**)&pcandidate)))
    {
        m_nCandidateRefCount++;
        MakeCandidateStrings(pcandidate);
        pcandidate->Release();
    }

    pElement->Release();
    return S_OK;
}

STDAPI CTsfUiLessMode::CUIElementSink::UpdateUIElement(DWORD dwUIElementId)
{
    Autolock _lock(ImeUiSyncRoot);

    auto pElement = GetUIElement(dwUIElementId);
    if (!pElement)
        return E_INVALIDARG;

    ITfReadingInformationUIElement* preading = nullptr;
    ITfCandidateListUIElement* pcandidate = nullptr;
    if (!g_bCandList && SUCCEEDED(pElement->QueryInterface(__uuidof(ITfReadingInformationUIElement),
        (void**)&preading)))
    {
        MakeReadingInformationString(preading);
        preading->Release();
    }
    else if (SUCCEEDED(pElement->QueryInterface(__uuidof(ITfCandidateListUIElement),
        (void**)&pcandidate)))
    {
        MakeCandidateStrings(pcandidate);
        pcandidate->Release();
    }

    pElement->Release();
    return S_OK;
}

STDAPI CTsfUiLessMode::CUIElementSink::EndUIElement(DWORD dwUIElementId)
{
    Autolock _lock(ImeUiSyncRoot);

    auto pElement = GetUIElement(dwUIElementId);
    if (!pElement)
        return E_INVALIDARG;

    ITfReadingInformationUIElement* preading = nullptr;
    if (!g_bCandList && SUCCEEDED(pElement->QueryInterface(__uuidof(ITfReadingInformationUIElement),
        (void**)&preading)))
    {
        g_dwCount = 0;
        preading->Release();
    }

    ITfCandidateListUIElement* pcandidate = nullptr;
    if (SUCCEEDED(pElement->QueryInterface(__uuidof(ITfCandidateListUIElement),
        (void**)&pcandidate)))
    {
        m_nCandidateRefCount--;
        if (m_nCandidateRefCount == 0)
            CloseCandidateList();
        pcandidate->Release();
    }

    pElement->Release();
    return S_OK;
}

void CTsfUiLessMode::UpdateImeState(BOOL bResetCompartmentEventSink)
{
    ITfCompartmentMgr* pcm;
    ITfCompartment* pTfOpenMode = nullptr;
    ITfCompartment* pTfConvMode = nullptr;
    if (GetCompartments(&pcm, &pTfOpenMode, &pTfConvMode))
    {
        VARIANT valOpenMode;
        if (SUCCEEDED(pTfOpenMode->GetValue(&valOpenMode)))
        {
            VARIANT valConvMode;
            if (SUCCEEDED(pTfConvMode->GetValue(&valConvMode)))
            {
                if (valOpenMode.vt == VT_I4)
                {
                    if (g_bChineseIME)
                    {
                        g_dwState = valOpenMode.lVal != 0 && valConvMode.lVal != 0 ? IMEUI_STATE_ON : IMEUI_STATE_ENGLISH;
                    }
                    else
                    {
                        g_dwState = valOpenMode.lVal != 0 ? IMEUI_STATE_ON : IMEUI_STATE_OFF;
                    }
                }
                VariantClear(&valConvMode);
            }
            VariantClear(&valOpenMode);
        }

        if (bResetCompartmentEventSink)
        {
            SetupCompartmentSinks(FALSE, pTfOpenMode, pTfConvMode);	// Reset compartment sinks
        }
        pTfOpenMode->Release();
        pTfConvMode->Release();
        pcm->Release();
    }
}

STDAPI CTsfUiLessMode::CUIElementSink::OnActivated(DWORD dwProfileType, LANGID langid, _In_ REFCLSID clsid, _In_ REFGUID catid,
    _In_ REFGUID guidProfile, HKL hkl, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(clsid);
    UNREFERENCED_PARAMETER(hkl);

    Autolock _lock(ImeUiSyncRoot);

    static GUID s_TF_PROFILE_DAYI =
    {
        0x037B2C25, 0x480C, 0x4D7F, 0xB0, 0x27, 0xD6, 0xCA, 0x6B, 0x69, 0x78, 0x8A
    };
    g_iCandListIndexBase = IsEqualGUID(s_TF_PROFILE_DAYI, guidProfile) ? 0 : 1;
    if (IsEqualIID(catid, GUID_TFCAT_TIP_KEYBOARD) && (dwFlags & TF_IPSINK_FLAG_ACTIVE))
    {
        g_bChineseIME = (dwProfileType & TF_PROFILETYPE_INPUTPROCESSOR) && langid == LANG_CHT;
        if (dwProfileType & TF_PROFILETYPE_INPUTPROCESSOR)
        {
            UpdateImeState(TRUE);
        }
        else
            g_dwState = IMEUI_STATE_OFF;
        OnInputLangChange();
    }
    return S_OK;
}

STDAPI CTsfUiLessMode::CUIElementSink::OnChange(_In_ REFGUID rguid)
{
    UNREFERENCED_PARAMETER(rguid);

    Autolock _lock(ImeUiSyncRoot);

    UpdateImeState();
    return S_OK;
}

void CTsfUiLessMode::MakeReadingInformationString(ITfReadingInformationUIElement* preading)
{
    UINT cchMax;
    UINT uErrorIndex = 0;
    BOOL fVertical;
    DWORD dwFlags;

    preading->GetUpdatedFlags(&dwFlags);
    preading->GetMaxReadingStringLength(&cchMax);
    preading->GetErrorIndex(&uErrorIndex);	// errorIndex is zero-based
    preading->IsVerticalOrderPreferred(&fVertical);
    g_iReadingError = (int)uErrorIndex;
    g_bHorizontalReading = !fVertical;
    g_bReadingWindow = true;
    g_uCandPageSize = MAX_CANDLIST;
    g_dwSelection = g_iReadingError ? g_iReadingError - 1 : (DWORD)-1;
    g_iReadingError--;	// g_iReadingError is used only in horizontal window, and has to be -1 if there's no error.

    BSTR bstr;
    if (SUCCEEDED(preading->GetString(&bstr)))
    {
        if (bstr)
        {
            wcscpy_s(g_szReadingString, COUNTOF(g_szReadingString), bstr);
            g_dwCount = cchMax;
            LPCTSTR pszSource = g_szReadingString;
            if (fVertical)
            {
                // for vertical reading window, copy each character to g_szCandidate array.
                for (UINT i = 0; i < cchMax; i++)
                {
                    LPTSTR pszDest = g_szCandidate[i];
                    if (*pszSource)
                    {
                        LPTSTR pszNextSrc = CharNext(pszSource);
                        SIZE_T size = (LPSTR)pszNextSrc - (LPSTR)pszSource;
                        memcpy(pszDest, pszSource, size);
                        pszSource = pszNextSrc;
                        pszDest += size;
                    }
                    *pszDest = 0;
                }
            }
            else
            {
                g_szCandidate[0][0] = TEXT(' ');	// hack to make rendering happen
            }
            SysFreeString(bstr);
        }
    }
}

void CTsfUiLessMode::MakeCandidateStrings(ITfCandidateListUIElement* pcandidate)
{
    UINT uIndex = 0;
    UINT uCount = 0;
    UINT uCurrentPage = 0;
    UINT* IndexList = nullptr;
    UINT uPageCnt = 0;
    DWORD dwPageStart = 0;
    DWORD dwPageSize = 0;
    BSTR bstr;

    pcandidate->GetSelection(&uIndex);
    pcandidate->GetCount(&uCount);
    pcandidate->GetCurrentPage(&uCurrentPage);
    g_dwSelection = (DWORD)uIndex;
    g_dwCount = (DWORD)uCount;
    g_bCandList = true;
    g_bReadingWindow = false;

    pcandidate->GetPageIndex(nullptr, 0, &uPageCnt);
    if (uPageCnt > 0)
    {
        IndexList = (UINT*)ImeUiCallback_Malloc(sizeof(UINT) * uPageCnt);
        if (IndexList)
        {
            pcandidate->GetPageIndex(IndexList, uPageCnt, &uPageCnt);
            dwPageStart = IndexList[uCurrentPage];
            dwPageSize = (uCurrentPage < uPageCnt - 1) ?
                std::min(uCount, IndexList[uCurrentPage + 1]) - dwPageStart :
                uCount - dwPageStart;
        }
    }

    g_uCandPageSize = std::min<UINT>(dwPageSize, MAX_CANDLIST);
    g_dwSelection = g_dwSelection - dwPageStart;

    memset(&g_szCandidate, 0, sizeof(g_szCandidate));
    for (UINT i = dwPageStart, j = 0; (DWORD)i < g_dwCount && j < g_uCandPageSize; i++, j++)
    {
        if (SUCCEEDED(pcandidate->GetString(i, &bstr)))
        {
            if (bstr)
            {
                ComposeCandidateLine(j, bstr);
                SysFreeString(bstr);
            }
        }
    }

    if (GETPRIMLANG() == LANG_KOREAN)
    {
        g_dwSelection = (DWORD)-1;
    }

    if (IndexList)
    {
        ImeUiCallback_Free(IndexList);
    }
}

ITfUIElement* CTsfUiLessMode::GetUIElement(DWORD dwUIElementId)
{
    ITfUIElementMgr* puiem;
    ITfUIElement* pElement = nullptr;

    if (SUCCEEDED(m_tm->QueryInterface(__uuidof(ITfUIElementMgr), (void**)&puiem)))
    {
        puiem->GetUIElement(dwUIElementId, &pElement);
        puiem->Release();
    }

    return pElement;
}

BOOL CTsfUiLessMode::CurrentInputLocaleIsIme()
{
    BOOL ret = FALSE;
    HRESULT hr;

    ITfInputProcessorProfiles* pProfiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(ITfInputProcessorProfiles), (LPVOID*)&pProfiles);
    if (SUCCEEDED(hr))
    {
        ITfInputProcessorProfileMgr* pProfileMgr;
        hr = pProfiles->QueryInterface(__uuidof(ITfInputProcessorProfileMgr), (LPVOID*)&pProfileMgr);
        if (SUCCEEDED(hr))
        {
            TF_INPUTPROCESSORPROFILE tip;
            hr = pProfileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &tip);
            if (SUCCEEDED(hr))
            {
                ret = (tip.dwProfileType & TF_PROFILETYPE_INPUTPROCESSOR) != 0;
            }
            pProfileMgr->Release();
        }
        pProfiles->Release();
    }
    return ret;
}

// Sets up or removes sink for UI element. 
// UI element sink should be removed when IME is disabled,
// otherwise the sink can be triggered when a game has multiple instances of IME UI library.
void CTsfUiLessMode::EnableUiUpdates(bool bEnable)
{
    if (!m_tm ||
        (bEnable && m_dwUIElementSinkCookie != TF_INVALID_COOKIE) ||
        (!bEnable && m_dwUIElementSinkCookie == TF_INVALID_COOKIE))
    {
        return;
    }
    ITfSource* srcTm = nullptr;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(hr = m_tm->QueryInterface(__uuidof(ITfSource), (void**)&srcTm)))
    {
        if (bEnable)
        {
            hr = srcTm->AdviseSink(__uuidof(ITfUIElementSink), (ITfUIElementSink*)m_TsfSink,
                &m_dwUIElementSinkCookie);
        }
        else
        {
            hr = srcTm->UnadviseSink(m_dwUIElementSinkCookie);
            m_dwUIElementSinkCookie = TF_INVALID_COOKIE;
        }
        srcTm->Release();
    }
}

// Returns open mode compartments and compartment manager.
// Function fails if it fails to acquire any of the objects to be returned.
BOOL CTsfUiLessMode::GetCompartments(ITfCompartmentMgr** ppcm, ITfCompartment** ppTfOpenMode,
    ITfCompartment** ppTfConvMode)
{
    ITfCompartmentMgr* pcm = nullptr;
    ITfCompartment* pTfOpenMode = nullptr;
    ITfCompartment* pTfConvMode = nullptr;

    static GUID _GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION =
    {
        0xCCF05DD8, 0x4A87, 0x11D7, 0xA6, 0xE2, 0x00, 0x06, 0x5B, 0x84, 0x43, 0x5C
    };

    HRESULT hr;
    if (SUCCEEDED(hr = m_tm->QueryInterface(IID_ITfCompartmentMgr, (void**)&pcm)))
    {
        if (SUCCEEDED(hr = pcm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &pTfOpenMode)))
        {
            if (SUCCEEDED(hr = pcm->GetCompartment(_GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                &pTfConvMode)))
            {
                *ppcm = pcm;
                *ppTfOpenMode = pTfOpenMode;
                *ppTfConvMode = pTfConvMode;
                return TRUE;
            }
            pTfOpenMode->Release();
        }
        pcm->Release();
    }
    return FALSE;
}

// There are three ways to call this function:
// SetupCompartmentSinks() : initialization
// SetupCompartmentSinks(FALSE, openmode, convmode) : Resetting sinks. This is necessary as DaYi and Array IME resets compartment on switching input locale
// SetupCompartmentSinks(TRUE) : clean up sinks
BOOL CTsfUiLessMode::SetupCompartmentSinks(BOOL bRemoveOnly, ITfCompartment* pTfOpenMode,
    ITfCompartment* pTfConvMode)
{
    bool bLocalCompartments = false;
    ITfCompartmentMgr* pcm = nullptr;
    BOOL bRc = FALSE;
    HRESULT hr = E_FAIL;

    if (!pTfOpenMode && !pTfConvMode)
    {
        bLocalCompartments = true;
        GetCompartments(&pcm, &pTfOpenMode, &pTfConvMode);
    }
    if (!(pTfOpenMode && pTfConvMode))
    {
        // Invalid parameters or GetCompartments() has failed.
        return FALSE;
    }
    ITfSource* srcOpenMode = nullptr;
    if (SUCCEEDED(hr = pTfOpenMode->QueryInterface(IID_ITfSource, (void**)&srcOpenMode)))
    {
        // Remove existing sink for open mode
        if (m_dwOpenModeSinkCookie != TF_INVALID_COOKIE)
        {
            srcOpenMode->UnadviseSink(m_dwOpenModeSinkCookie);
            m_dwOpenModeSinkCookie = TF_INVALID_COOKIE;
        }
        // Setup sink for open mode (toggle state) change
        if (bRemoveOnly || SUCCEEDED(hr = srcOpenMode->AdviseSink(IID_ITfCompartmentEventSink,
            (ITfCompartmentEventSink*)m_TsfSink,
            &m_dwOpenModeSinkCookie)))
        {
            ITfSource* srcConvMode = nullptr;
            if (SUCCEEDED(hr = pTfConvMode->QueryInterface(IID_ITfSource, (void**)&srcConvMode)))
            {
                // Remove existing sink for open mode
                if (m_dwConvModeSinkCookie != TF_INVALID_COOKIE)
                {
                    srcConvMode->UnadviseSink(m_dwConvModeSinkCookie);
                    m_dwConvModeSinkCookie = TF_INVALID_COOKIE;
                }
                // Setup sink for open mode (toggle state) change
                if (bRemoveOnly || SUCCEEDED(hr = srcConvMode->AdviseSink(IID_ITfCompartmentEventSink,
                    (ITfCompartmentEventSink*)m_TsfSink,
                    &m_dwConvModeSinkCookie)))
                {
                    bRc = TRUE;
                }
                srcConvMode->Release();
            }
        }
        srcOpenMode->Release();
    }
    if (bLocalCompartments)
    {
        pTfOpenMode->Release();
        pTfConvMode->Release();
        pcm->Release();
    }
    return bRc;
}


WORD ImeUi_GetPrimaryLanguage()
{
    return GETPRIMLANG();
};

DWORD ImeUi_GetImeId(_In_ UINT uIndex)
{
    return GetImeId(uIndex);
};

WORD ImeUi_GetLanguage()
{
    return GETLANG();
};

PTSTR ImeUi_GetIndicatior()
{
    return g_pszIndicatior;
};


bool ImeUi_IsShowReadingWindow()
{
    return g_bReadingWindow;
};

bool ImeUi_IsShowCandListWindow()
{
    return g_bCandList;
};

bool ImeUi_IsVerticalCand()
{
    return g_bVerticalCand;
};

bool ImeUi_IsHorizontalReading()
{
    return g_bHorizontalReading;
};

TCHAR* ImeUi_GetCandidate(_In_ UINT idx)
{
    if (idx < MAX_CANDLIST)
        return g_szCandidate[idx];
    else
        return g_szCandidate[0];
}

DWORD ImeUi_GetCandidateSelection()
{
    return g_dwSelection;
}

DWORD ImeUi_GetCandidateCount()
{
    return g_dwCount;
}

TCHAR* ImeUi_GetCompositionString()
{
    return g_szCompositionString;
}

BYTE* ImeUi_GetCompStringAttr()
{
    return  g_szCompAttrString;
}

DWORD ImeUi_GetImeCursorChars()
{
    return g_IMECursorChars;
}
