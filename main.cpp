#define PLUGIN_VERSION "3.25.3"

#define WACUP_BUILD
//#define USE_GDIPLUS
#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#ifdef USE_GDIPLUS
#pragma warning(push)
#pragma warning(disable:4458) // declaration of 'xxx' hides class member
#include <gdiplus.h>
#pragma warning(pop)
#pragma comment(lib, "gdiplus.lib")
#endif
#include <string>
#include <map>
#define _USE_MATH_DEFINES
#include <math.h>
#include <winamp/in2.H>
#include <winamp/gen.h>
#ifdef WACUP_BUILD
#include <winamp/wa_cup.h>
#else
#include <winamp/wa_ipc.h>
#endif
#include <winamp/ipc_pe.h>
#include <gen_ml/ml.h>
#include <gen_ml/ml_ipc_0313.h>
#include "resource.h"
#include "api.h"
#include "gen_waveseek.h"
#include "embedwnd.h"
#ifdef WACUP_BUILD
#include <loader/loader/paths.h>
#include <loader/loader/utils.h>
#include <loader/loader/ini.h>
#include <loader/loader/runtime_helper.h>
#include <loader/hook/plugins.h>
#include <nu/ServiceBuilder.h>
#endif

//#define WA_DLG_IMPLEMENT
#define WA_DLG_IMPORTS
#include <winamp/wa_dlg.h>
#include <strsafe.h>

#ifndef ID_PE_SCUP
#define ID_PE_SCUP   40289
#endif

#ifndef ID_PE_SCDOWN
#define ID_PE_SCDOWN 40290
#endif

// this is used to identify the skinned frame to allow for embedding/control by modern skins if needed
#ifdef WACUP_BUILD
// {E124F4D6-AA3E-4f3d-A813-C2A8CD6501E5}
static const GUID embed_guid = 
{ 0xe124f4d6, 0xaa3e, 0x4f3d, { 0xa8, 0x13, 0xc2, 0xa8, 0xcd, 0x65, 0x1, 0xe5 } };
#else
// {1C2F2C09-4F43-4CFF-9DE7-32E014638DFC}
static const GUID embed_guid = 
{ 0x1c2f2c09, 0x4f43, 0x4cff, { 0x9d, 0xe7, 0x32, 0xe0, 0x14, 0x63, 0x8d, 0xfc } };
#endif

HWND hWndWaveseek = NULL, hWndToolTip = NULL, hWndInner = NULL;
static ATOM wndclass = 0;
WNDPROC oldPlaylistWndProc = NULL;
bool paint_allowed = false;
HDC cacheDC = NULL;
HBITMAP cacheBMP = NULL;
HRGN waveformRemainingRgn = NULL, waveformRgn = NULL;
RECT lastWnd = { 0 };
embedWindowState embed = { 0 };
TOOLINFO ti = { 0 };
int clickTrack = 0, showCuePoints = 0,
#ifndef _WIN64
	legacy = 0,
#endif
	audioOnly = 1, hideTooltip = 0, debug = 0,
	kill_threads = 0, lowerpriority = 0, clearOnExit = 0;
UINT WINAMP_WAVEFORM_SEEK_MENUID = 0xa1bb;
UINT_PTR timer_id = 0;
LPPOINT wave_remaining_points = NULL, wave_points = NULL;

api_service *WASABI_API_SVC = NULL;
api_decodefile2 *WASABI_API_DECODEFILE2 = NULL;
api_application *WASABI_API_APP = NULL;

SETUP_API_LNG_VARS;

void DummySAVSAInit(const int maxlatency_in_ms, const int srate) {}
void DummySAVSADeInit() {}
void DummySAAddPCMData(const void *PCMData, const int nch, const int bps, const int timestamp) {}
int DummySAGetMode() { return 0; }
int DummySAAdd(const void *data, const int timestamp, const int csa) { return 0; }
void DummyVSAAddPCMData(const void *PCMData, const int nch, const int bps, const int timestamp) {}
int DummyVSAGetMode(int *specNch, int *waveNch) { return 0; }
int DummyVSAAdd(const void *data, const int timestamp) { return 0; }
void DummyVSASetInfo(const int srate, const int nch) {}
void DummySetInfo(const int bitrate, const int srate, const int stereo, const int synched) {}

#ifndef _WIN64
LRESULT CALLBACK EmdedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
							  UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
#endif

void FinishProcessingFile(LPCWSTR szCacheFile, unsigned short *pBuffer);
void ProcessStop(const bool is_closing = false);

typedef In_Module *(*PluginGetter)();

#define TIMER_ID 31337
#define TIMER_FREQ 125		// ~8fps
#define TIMER_LIVE_FREQ 33	// ~30fps

wchar_t
#ifndef WACUP_BUILD
		*szDLLPath = 0, *ini_file = 0,
#endif
		szFilename[MAX_PATH] = { 0 },
		szWaveCacheFile[MAX_PATH] = { 0 },
#ifndef _WIN64
		szTempDLLDestination[MAX_PATH] = { 0 },
#endif
		szUnavailable[128] = { 0 },
		szBadPlugin[128] = { 0 },
		szStreamsNotSupported[128] = { 0 },
		szLegacy[128] = { 0 };

#ifndef _WIN64
In_Module * pModule = NULL;
#endif
LRESULT delay_load = -1;
size_t szFilenameLen = 0;
int nLengthInMS = 0, no_uninstall = 1, bUnsupported = 0;
bool bIsCurrent = false, bIsProcessing = false, bIsLoaded = false;
DWORD delay_ipc = (DWORD)-1;

std::map<std::wstring, HANDLE> processing_list;
CRITICAL_SECTION processing_cs = { 0 };
HBRUSH waveformRemaining = NULL,
	   waveformPlayed = NULL;
COLORREF clrWaveform = RGB(0, 255, 0),
		 clrBackground = RGB(0, 0, 0),
		 clrCuePoint = RGB(117, 116, 139),
		 clrWaveformPlayed = RGB(0, 128, 0),
		 clrGeneratingText = RGB(0, 128, 0),
		 clrWaveformFailed = RGB(0, 96, 0);

void PluginConfig();

wchar_t* GetFilePaths(void)
{
	static wchar_t szWaveCacheDir[MAX_PATH] = { 0 };
	if (!szWaveCacheDir[0])
	{
		// find the winamp.ini for the Winamp install being used
#ifdef WACUP_BUILD
		//ini_file = (wchar_t *)GetPaths()->winamp_ini_file;
		CombinePath(szWaveCacheDir, GetPaths()->settings_sub_dir, L"wavecache");
#else
		ini_file = (wchar_t *)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_GETINIFILEW);
		(void)StringCchCopy(szWaveCacheDir, ARRAYSIZE(szWaveCacheDir), (wchar_t *)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_GETINIDIRECTORYW));
		AppendOnPath(szWaveCacheDir, L"Plugins\\wavecache");
#endif

		// make the cache folder in the user's settings folder e.g. %APPDATA%\Winamp\Plugins\wavecache
		// which will better ensure that the cache will be correctly generated though it will fallback
		// to %PROGRAMFILES(x86)%\Winamp\Plugins\wavecache or %PROGRAMFILES%\Winamp\Plugins\wavecache
		// as applicable to the Windows and Winamp version being used (more so with pre v5.11 clients)
		CreateDirectory(szWaveCacheDir, NULL);

#ifndef WACUP_BUILD
		// find the correct Winamp\Plugins folder (using native api before making a good guess at it)
		szDLLPath = (wchar_t *)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_GETPLUGINDIRECTORYW);
#endif
	}
	return szWaveCacheDir;
}

#ifndef _WIN64
int GetFileInfo(const bool unicode, char* szFn, char szFile[MAX_PATH])
{
	wchar_t szTitle[GETFILEINFO_TITLE_LENGTH] = { 0 };
	int lengthInMS = -1;
	if (!unicode)
	{
		ConvertPathToA(szFilename, szFile, MAX_PATH, CP_ACP);
	}

	pModule->GetFileInfo((unicode ? (char*)szFn : szFile), (char*)szTitle, &lengthInMS);
	return lengthInMS;
}

void MPG123HotPatch(HINSTANCE module)
{
	const patch_block blocks[] =
	{
		{0x7740, 2, 0, "\x75\x05\xE8\x89\xFD\xFF\xFF\x6A", "\xEB\x00"}, // 109.2 SSE2
		{0x76CB, 2, 0, "\x75\x05\xE8\x8E\xFD\xFF\xFF\x6A", "\xEB\x00"}, // 109.2 normal
		{0x7290, 2, 0, "\x75\x05\xE8\x89\xFE\xFF\xFF\x53", "\xEB\x00"},	// 112.1 SSE2
		{0x725B, 2, 0, "\x75\x05\xE8\x8E\xFE\xFF\xFF\x53", "\xEB\x00"}, // 112.1 normal
	};

	ApplyPatch(module, blocks, ARRAYSIZE(blocks));
}
#endif

unsigned long AddThreadSample(LPCWSTR szFn, unsigned short *pBuffer, const unsigned int nSample,
							  const unsigned int nFramePerWindow, const unsigned int nNumChannels,
							  unsigned long nAmplitude, unsigned long &nSampleCount,
							  unsigned int &nBufferPointer, uint64_t &nTotalSampleCount)
{
	if (!kill_threads)
	{
		nAmplitude = max(nAmplitude, nSample);
		++nSampleCount;
		++nTotalSampleCount;

		if (((nSampleCount / nNumChannels) == nFramePerWindow) &&
			(nBufferPointer < SAMPLE_BUFFER_SIZE))
		{
			pBuffer[nBufferPointer++] = (unsigned short)nAmplitude;
			nSampleCount = 0;
			nAmplitude = 0;

			if (wcsistr(szFilename, szFn))
			{
				// since the playlist selection can have
				// changed, it's simpler to just re-copy
				// the whole buffer (small penalty) than
				// trying to just update it on per-byte.
				memcpy(&pSampleBuffer, pBuffer, SAMPLE_BUFFER_SIZE * sizeof(unsigned short));
			}
		}
	}

	return nAmplitude;
}

void abort_processing_thread(const HANDLE handle)
{
	if (handle != NULL)
	{
		if (WaitForSingleObjectEx(handle, 3000/*/INFINITE/**/, TRUE) == WAIT_TIMEOUT)
		{
			CloseHandle(handle);
		}
	}
}

void ClearProcessingHandles(void)
{
	ProcessStop((kill_threads == 2));

	EnterCriticalSection(&processing_cs);

	std::map<std::wstring, HANDLE>::iterator itr = processing_list.begin();
	while (itr != processing_list.end())
	{
		abort_processing_thread((*itr).second);

		itr = processing_list.erase(itr);
	}

	// make sure we're good
	processing_list.clear();
}

typedef struct
{
	LPCWSTR filename;
	INT_PTR db_error;
	AudioParameters parameters;
} CalcThreadParams;

DWORD WINAPI CalcWaveformThread(LPVOID lp)
{
//#define USE_PROFILING
#ifdef USE_PROFILING
	LARGE_INTEGER starttime = { 0 }, endtime = { 0 };
	QueryPerformanceCounter(&starttime);
#endif

	(void)CreateCOM();

	CalcThreadParams *item = (CalcThreadParams *)lp;
	ifc_audiostream* decoder = NULL;
	unsigned int nFramePerWindow = 0;
	int lengthMS = -1;
	void* token = NULL;
	bool reentrant = false;
	wchar_t buf[16] = { 0 };
	extendedFileInfoStructW efis = { item->filename, (audioOnly ? L"type" :
										  L"length"), buf, ARRAYSIZE(buf) };

	// if enabled then only process audio only files
	// as processing video can cause some problems...
	if (!kill_threads && audioOnly)
	{
		if (GetFileInfoHookable((WPARAM)&efis, TRUE, &token, &reentrant,
									  &item->db_error) && !!WStr2I(buf))
		{
			plugin.metadata->FreeExtendedFileInfoToken(&token);
			goto abort;
		}

		efis.metadata = L"length";
	}
	buf[0] = 0;

	// try to get the appropriate value which will
	// usually be pulled in from the local library
	// database before trying the file itself. but
	// that can sometimes fail due to file locking
	// so it'll if needed fallback to the playlist
	// information that's been stored for the item
	// as something to work with to check it's ok.
	if (!kill_threads)
	{
		if (GetFileInfoHookable((WPARAM)&efis, TRUE, &token, &reentrant,
										     &item->db_error) && buf[0])
		{
			lengthMS = WStr2I(buf);
		}
		else
		{
			basicFileInfoStructW bfiW = { item->filename, 0, -1, NULL, 0 };
			if (GetBasicFileInfo(&bfiW, TRUE, TRUE))
			{
				lengthMS = (bfiW.length * 1000);
			}
		}
	}

	plugin.metadata->FreeExtendedFileInfoToken(&token);

	// without a valid length we've not got much
	// chance of reliably processing this file
	if (!kill_threads && (lengthMS > 0))
	{
		decoder = (WASABI_API_DECODEFILE2 ? WASABI_API_DECODEFILE2->OpenAudioBackground(item->filename, &item->parameters) : NULL);

		if (decoder)
		{
			nFramePerWindow = MulDiv(lengthMS, item->parameters.sampleRate,
											   SAMPLE_BUFFER_SIZE * 1000) + 1;
		}
	}

	if (decoder)
	{
		wchar_t szThreadWaveCacheFile[MAX_PATH] = { 0 };
		unsigned long nAmplitude = 0, nSampleCount = 0;
		unsigned int nBufferPointer = 0;
		uint64_t nTotalSampleCount = 0;
		unsigned short pThreadSampleBuffer[SAMPLE_BUFFER_SIZE] = { 0 };

		memset(pSampleBuffer, 0, SAMPLE_BUFFER_SIZE * sizeof(unsigned short));
		(void)StringCchCopy(szThreadWaveCacheFile, ARRAYSIZE(szThreadWaveCacheFile), szWaveCacheFile);

		// ensure that the expected count will work with vary large files where there's
		// millions & billions of samples otherwise it'll abort the rendering too soon!
		const uint64_t nExpectedTotalSampleCount = (nFramePerWindow * SAMPLE_BUFFER_SIZE
													* 1ULL * item->parameters.channels);
		const int padded_bits = (((item->parameters.bitsPerSample + 7) & (~7)) / 8);
		const size_t buffer_size = (1152 * item->parameters.channels * padded_bits);
		char* data = (char*)plugin.memmgr->sysMalloc(buffer_size * 2);	// *2 deals with bad calls
		if (!data)
		{
			goto abort;
		}

		// to avoid some visual issues especially if there's a
		// delay to get the metadata from the local library we
		// only flag this once we know we're going to read it.
		bIsProcessing = true;

		while (!kill_threads)
		{
			int error = 0;
			const size_t bytesRead = decoder->ReadAudio((void *)data, buffer_size,
															&kill_threads, &error);
			if (error || kill_threads)
			{
				break;
			}

			if (!(bytesRead ? (bytesRead / padded_bits) : 0) || kill_threads)
			{
				break;
			}

			// cppcheck-suppress knownConditionTrueFalse
			if ((nFramePerWindow == 0) || kill_threads)
			{
				break;
			}

			if (item->parameters.bitsPerSample == 16)
			{
				const short *p = (short *)data;
				for (size_t i = 0; i < (bytesRead / 2) && !kill_threads; i++)
				{
					const unsigned int nSample = abs(*(p++));
					nAmplitude = AddThreadSample(item->filename, &pThreadSampleBuffer[0],
												 nSample, nFramePerWindow, item->parameters.channels,
												 nAmplitude, nSampleCount, nBufferPointer, nTotalSampleCount);
				}
			}
			else if (item->parameters.bitsPerSample == 24)
			{
				const char *p = (char *)data;
				for (size_t i = 0; i < (bytesRead / 3) && !kill_threads; i++)
				{
					const unsigned int nSample = abs((((0xFF & *(p + 2)) << 24) |
													 ((0xFF & *(p + 1)) << 16) |
													 ((0xFF & *(p)) << 8)) >> 16);
					p += 3;
					nAmplitude = AddThreadSample(item->filename, &pThreadSampleBuffer[0], nSample,
												 nFramePerWindow, item->parameters.channels,
												 nAmplitude, nSampleCount, nBufferPointer, nTotalSampleCount);
				}
			}
			else
			{
				// if we don't support it then we need to flag it
				// so that a message is provided to the user else
				// it can cause confusion due to looking broken.
				bUnsupported = 1;
				break;
			}

			// if we're beyond the maximum expected samples then
			// we should just abort as it's likely to be related
			// to a file format that does looping or it doesn't
			// report the length correctly vs just spinning here
			if (nTotalSampleCount > nExpectedTotalSampleCount)
			{
				break;
			}
		}

		plugin.memmgr->sysFree(data);

		if (WASABI_API_DECODEFILE2)
		{
			WASABI_API_DECODEFILE2->CloseAudio(decoder);
		}

		if (!kill_threads)
		{
			FinishProcessingFile(szThreadWaveCacheFile, &pThreadSampleBuffer[0]);

			// when we have finished, if we are still the
			// current playlist item (playing / selected)
			// then we will just copy over the buffer so
			// that we don't need to do more processing.
			if (wcsistr(szFilename, item->filename))
			{
				memcpy(&pSampleBuffer, &pThreadSampleBuffer, SAMPLE_BUFFER_SIZE * sizeof(unsigned short));
			}
		}
	}
	else
	{
		// if we don't support it then we need to flag it
		// so that a message is provided to the user else
		// it can cause confusion due to looking broken.
		bUnsupported = 1;
	}

abort:
	if (!kill_threads)
	{
		EnterCriticalSection(&processing_cs);

		std::map<std::wstring, HANDLE>::iterator itr = processing_list.begin();
		while (itr != processing_list.end())
		{
			// found it so no need to re-add
			// as that's just going to cause
			// more processing / duplication
			if (wcsistr(item->filename, (*itr).first.c_str()))
			{
				CloseHandle((*itr).second);

				processing_list.erase(itr);
				break;
			}

			++itr;
		}

		const bool is_empty = processing_list.empty();

		LeaveCriticalSection(&processing_cs);

		if (is_empty)
		{
			// start or stop the timer
			// as needed based on the
			// current playback state
			ProcessStop();

			bIsProcessing = false;
		}
	}

#ifdef USE_PROFILING
	QueryPerformanceCounter(&endtime);
	const float ms = ((endtime.QuadPart - starttime.QuadPart) * 1000.0f / PerfFreq().QuadPart);
	if (ms > /*0/*/0.10f/**/)
	{
		wchar_t profile[128] = { 0 };
		StringCchPrintf(profile, ARRAYSIZE(profile), L"%.3fms", ms);
		MessageBox(plugin.hwndParent, profile, 0, 0);
	}
#endif
	plugin.memmgr->sysFree((LPVOID)item->filename);
	plugin.memmgr->sysFree((LPVOID)item);

	CloseCOM();
	return 0;
}

void start_live_timer(void)
{
	if (IsWindow(hWndInner))
	{
		timer_id = SetTimer(hWndInner, TIMER_ID, TIMER_LIVE_FREQ, NULL);
	}
}

int calc_thread_started_callback(HANDLE thread, LPVOID parameter)
{
	if (thread != NULL)
	{
		start_live_timer();
	}
	else if (parameter)
	{
		CalcThreadParams* item = (CalcThreadParams*)parameter;
		plugin.memmgr->sysFree((LPVOID)item->filename);
		plugin.memmgr->sysFree((LPVOID)item);
	}
	return 1;
}

HANDLE StartProcessingFile(const wchar_t *szFn, const wchar_t *szArchiveFn,
						   const bool start_playing, const INT_PTR db_error)
{
#ifndef _WIN64
	pModule = NULL;
#endif
	// first we try to use the decoder api which saves having to do any
	// of the plug-in copying which is better and works with most of the
	// popular formats (as long as the input plug-in has support for it)
	// though only doing it if the window is open otherwise it's a waste
	// to do the processing if it's not going to be seen & resolves some
	// user complaints related to the background processing hammering it
	const bool has_inner_wnd = IsWindow(hWndInner);
	if (!kill_threads && (visible || !has_inner_wnd/*BBM skin embedded*/) &&
		WASABI_API_DECODEFILE2 && WASABI_API_DECODEFILE2->DecoderExists(szFn))
	{
		if (!visible && has_inner_wnd)
		{
			visible = TRUE;
		}

		CalcThreadParams *item = (CalcThreadParams *)plugin.memmgr->sysMalloc(sizeof(CalcThreadParams));
		if (item)
		{
			item->db_error = db_error;
			item->parameters.flags = AUDIOPARAMETERS_MAXCHANNELS | AUDIOPARAMETERS_MAXSAMPLERATE | AUDIOPARAMETERS_NO_RESAMPLE;
			item->parameters.channels = 2;
			item->parameters.bitsPerSample = 24;
			item->parameters.sampleRate = 44100;
			item->filename = plugin.memmgr->sysDupStr((wchar_t*)szFn);

			const HANDLE CalcThread = StartThread(CalcWaveformThread, item, (!lowerpriority ? THREAD_PRIORITY_HIGHEST :
															 THREAD_PRIORITY_LOWEST), 0, calc_thread_started_callback);
			if (CalcThread)
			{
				return CalcThread;
			}
		}
	}

#ifndef _WIN64
	if (legacy)
	{
		HMODULE hDLL = NULL;
		// we use Winamp's own checking to more reliably ensure that we'll
		// get which plug-in is actually responsible for the file handling
		In_Module *in_mod = InputPluginFindPlugin(szFn, 0)/*(In_Module*)SendMessage(plugin.hwndParent,
															WM_WA_IPC, (WPARAM)szFn, IPC_CANPLAY)/**/;
		if (in_mod && (in_mod != (In_Module*)1))
		{
			wchar_t szSource[MAX_PATH] = { 0 };
			GetModuleFileName(in_mod->hDllInstance, szSource, ARRAYSIZE(szSource));

			// we got a valid In_Module * so make a temp copy
			// which we'll then be calling for the processing
			wchar_t *filename = FindPathFileName(szSource);
			if (!filename || !*filename)
			{
				bUnsupported = 2;
				return NULL;
			}

			// prime the plug-in just in-case it supports this
			// & the plug-in author forgets to do it as needed
			// or it's a wacup plug-in build so skip it anyway
			if ((((in_mod->version & ~IN_UNICODE) &
				~IN_INIT_RET) == IN_VER_REVISION) ||
				wcsistr(filename, L"in_cdda.dll") ||
				wcsistr(filename, L"in_flac.dll") ||
				wcsistr(filename, L"in_midi.dll") ||
				wcsistr(filename, L"in_mod.dll") ||
				wcsistr(filename, L"in_mp3.dll") ||
				wcsistr(filename, L"in_mp4.dll") ||
				wcsistr(filename, L"in_snes.dll") ||
				wcsistr(filename, L"in_snes.trb") ||
				wcsistr(filename, L"in__snesamp_wrapper.dll") ||
				wcsistr(filename, L"in_txt.dll") ||
				wcsistr(filename, L"in_url.dll") ||
				wcsistr(filename, L"in_vgmstream.dll") ||
				wcsistr(filename, L"in_vorbis.dll") ||
				wcsistr(filename, L"in_wave.dll") ||
				wcsistr(filename, L"in_wm.dll") ||
				wcsistr(filename, L"in_yansf.dll"))
			{
				// these native / nullsoft plug-ins really
				// really really doesn't like to work as a
				// multi-instance plug-in so best to just
				// not attempt to use it for processing :(
				//
				// somehow leveraging the transcoding api
				// might be enough to re-enable in_wm.dll
				//
				// the in_mp4 check is there since nullsoft
				// or wacup provided versions also don't
				// play well when copied & the only reason
				// for trying to use in_mp4 here is if its
				// from a video only mp4 file (as happens)
				bUnsupported = 2;
				return NULL;
			}

			if (wcsistr(filename, L"in_bpxfade.dll"))
			{
				// unfortunately this plug-in seems to have issues due to
				// it not fully supporting the Winamp 5.66+ API with the
				// api_service member of the input plug-in structure so
				// it's best to block it being used to prevent crashing.
				// it acts as a middle-ware between Winamp and plug-ins
				// which without that api_service member being populated
				// and a few other assumptions it makes for trouble :(
				bUnsupported = 2;
				return NULL;
			}

			if (wcsistr(filename, L"in_bpopus.dll"))
			{
				// this (another thinktink plug-in like in_bpxfade.dll)
				// doesn't like being duplicated which will cause a crash
				bUnsupported = 2;
				return NULL;
			}

			if (wcsistr(filename, L"in_zip.dll"))
			{
				// this is to avoid issues if the WACUP version of the
				// plug-in contains files that require the legacy mode
				// as it otherwise tries to work with ifc_audiostream
				bUnsupported = 2;
				return NULL;
			}

			CombinePath(szTempDLLDestination, GetFilePaths(), L"waveseek_");
			AddExtension(szTempDLLDestination, filename);

			// if not there then copy it
			if (!FileExists(szTempDLLDestination))
			{
				CopyFile(szSource, szTempDLLDestination, FALSE);
			}
			// otherwise check there's not a difference in the
			// file times which means if the original plug-in
			// gets updated then our in-cache copy gets updated
			// and this way we're only copying the plug-in once
			else
			{
				HANDLE sourceFile = CreateFile(szSource, GENERIC_READ, FILE_SHARE_READ, NULL,
											   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL),
					   destFile = CreateFile(szTempDLLDestination, GENERIC_READ, FILE_SHARE_READ,
											 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

				FILETIME sourceTime = { 0 }, destTime = { 0 };
				GetFileTime(sourceFile, NULL, NULL, &sourceTime);
				GetFileTime(destFile, NULL, NULL, &destTime);

				CloseHandle(sourceFile);
				CloseHandle(destFile);

				// there's a difference so we will re-copy
				if (CompareFileTime(&sourceTime, &destTime))
				{
					CopyFile(szSource, szTempDLLDestination, FALSE);
				}
			}

			if (FileExists(szTempDLLDestination))
			{
				hDLL = GetOrLoadDll(szTempDLLDestination, NULL);
				if (!hDLL)
				{
					return NULL;
				}

#ifndef _WIN64
				if (wcsistr(filename, L"mpg123"))
				{
					// this allows us to patch the mpg123 based input plug-in
					// (different versions) so we can correct an issue it has
					// with the use of the fake output not being called once
					// the first file has been processed which was ok when we
					// unloaded the input plug-in after use (which isn't good
					// for most other plug-ins out there i.e. hang on close!)
					MPG123HotPatch(hDLL);
				}
#endif

				__try
				{
					PluginGetter pluginGetter = (PluginGetter)GetProcAddress(hDLL, "winampGetInModule2");
					if (!pluginGetter)
					{
						return NULL;
					}

					pModule = pluginGetter();
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					return NULL;
				}
			}
		}

		if (pModule)
		{
			pModule->hMainWindow = hWndWaveseek;
			pModule->hDllInstance = hDLL;

			pModule->dsp_isactive = DummyDSPIsActive;
			pModule->dsp_dosamples = DummyDSPDoSamples;
			pModule->SAVSAInit = DummySAVSAInit;
			pModule->SAVSADeInit = DummySAVSADeInit;
			pModule->SAAddPCMData = DummySAAddPCMData;
			pModule->SAGetMode = DummySAGetMode;
			pModule->SAAdd = DummySAAdd;
			pModule->VSAAddPCMData = DummyVSAAddPCMData;
			pModule->VSAGetMode = DummyVSAGetMode;
			pModule->VSAAdd = DummyVSAAdd;
			pModule->VSASetInfo = DummyVSASetInfo;
			pModule->SetInfo = DummySetInfo;

			// if a v5.66x+ input plug-in then fill the 'service' member
			if (((pModule->version & ~IN_UNICODE) & ~IN_INIT_RET) == 0x101)
			{
				pModule->service = WASABI_API_SVC;
			}

			const bool has_ret = !!(pModule->version & IN_INIT_RET);
			const int ret = pModule->Init();
			if ((has_ret && (ret == IN_INIT_SUCCESS)) || !has_ret)
			{
				if (pModule->UsesOutputPlug & IN_MODULE_FLAG_USES_OUTPUT_PLUGIN)
				{
					char szFile[MAX_PATH] = { 0 };
					const bool unicode = !!(pModule->version & IN_UNICODE);
					nLengthInMS = GetFileInfo(unicode, (char*)szFn, szFile);
					if (nLengthInMS <= 0)
					{
						return NULL;
					}

					pModule->outMod = CreateOutput(hWndWaveseek, hDLL);
					if (pModule->Play((unicode ? (char*)szFn : szFile)) != 0)
					{
						return NULL;
					}

					start_live_timer();
				}
			}
		}
	}
	else
#endif
	{
		// give the user a hint if all else fails
		bUnsupported = 3;
	}
	return NULL;
}

void ProcessStop(const bool is_closing)
{
#ifndef _WIN64
	if (pModule)
	{
		if (bIsProcessing && pModule->Stop)
		{
			__try
			{
				pModule->Stop();
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		if (pModule->Quit)
		{
			__try
			{
				pModule->Quit();
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		pModule = NULL;
	}
#endif

	if (!is_closing && !!GetPlayingState())
	{
		if (IsWindow(hWndInner))
		{
			timer_id = SetTimer(hWndInner, TIMER_ID, TIMER_FREQ, NULL);
		}
	}
	else if (IsWindow(hWndInner))
	{
		KillTimer(hWndInner, TIMER_ID);

		if (is_closing)
		{
			KillTimer(hWndInner, 8889);
		}

		if (timer_id)
		{
			if (IsWindow(hWndInner))
			{
				InvalidateRect(hWndInner, NULL, TRUE);
			}
			timer_id = 0;
		}
	}
}

void FinishProcessingFile(LPCWSTR szCacheFile, unsigned short *pBuffer)
{
	HANDLE h = CreateFile(szCacheFile, GENERIC_WRITE, NULL, NULL, CREATE_ALWAYS, NULL, NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD dw = 0;
		WriteFile(h, (pBuffer ? pBuffer : pSampleBuffer), SAMPLE_BUFFER_SIZE * sizeof(unsigned short), &dw, NULL);
		CloseHandle(h);

		if (dw > 0)
		{
			bIsLoaded = true;
		}
	}

	ProcessStop();
}

typedef struct {
	wchar_t szPerformer[256];
	wchar_t szTitle[256];
	unsigned int nMillisec;
	int nDrawnPos;
	bool bDrawn;
} CUETRACK;

int nCueTracks = 0;
CUETRACK pCueTracks[256] = { 0 };

void LoadCUE(wchar_t * szFn)
{
	if (showCuePoints)
	{
		wchar_t szCue[MAX_PATH] = { 0 };
		(void)StringCchCopy(szCue, ARRAYSIZE(szCue), szFn);
		RenameExtension(szCue, L".cue");

		nCueTracks = 0;

		// TODO this is causing things to hang with broken network shares
		//		since it's getting a valid response due to how things now
		//		react to network / unc paths to avoid hangs vs checking &
		//		that will incorrectly allow the action to be processed...
		FILE* f = (FilePathExists(szCue, NULL) ? _wfopen(szCue, L"rt,ccs=UTF-8") : NULL);
		if (!f)
		{
			return;
		}

		int nCurrentTrack = 0;
		wchar_t strs[256] = { 0 };
		while (fgetws(strs, ARRAYSIZE(strs), f))
		{
			wchar_t* str = strs;
			while ((*str == L' ') || (*str == L'\r') || (*str == L'\n'))
			{
				++str;
			}

			if (wcsstr(str, L"TRACK") == str)
			{
				(void)swscanf(str, L"TRACK %d AUDIO", &nCurrentTrack);
				nCurrentTrack = min(nCurrentTrack, 255);
				nCueTracks = max(nCueTracks, nCurrentTrack);
				if (nCurrentTrack > 1)
				{
					pCueTracks[nCurrentTrack - 1].szPerformer[0] = 0;
					pCueTracks[nCurrentTrack - 1].szTitle[0] = 0;
				}
			}

			if (nCurrentTrack > 0)
			{
				CUETRACK& track = pCueTracks[nCurrentTrack - 1];
				if (wcsstr(str, L"PERFORMER") == str)
				{
					(void)swscanf(str, L"PERFORMER \"%[^\"]\"", track.szPerformer);
				}

				if (wcsstr(str, L"TITLE") == str)
				{
					(void)swscanf(str, L"TITLE \"%[^\"]\"", track.szTitle);
				}

				if (wcsstr(str, L"INDEX") == str)
				{
					int m = 0, s = 0, _f = 0;
					(void)swscanf(str, L"INDEX %*d %d:%d:%d", &m, &s, &_f);
					track.nMillisec = m * 60 * 1000 + s * 1000 + (_f * 1000) / 75;
				}
			}
		}

		fclose(f);
	}
}

LPWSTR GetTooltipText(HWND hWnd, int pos, int lengthInMS)
{
	static wchar_t coords[256] = { 0 };
	RECT rc = { 0 };
	GetClientRect(hWnd, &rc);

	// adjust width down by 1px so that the tooltip should then
	// appear when the mouse is at the far right of the window.
	unsigned int cur_ms = MulDiv(pos, lengthInMS, (rc.right - rc.left) - 1),
				 sec = (cur_ms > 0 ? (cur_ms / 1000) : 0),
				 total_sec = (lengthInMS > 0 ? (lengthInMS / 1000) : 0);

	int nTrack = -1;
	for (int i = 0; i < nCueTracks; i++)
	{
		if (i < nCueTracks - 1)
		{
			if (pCueTracks[i].nMillisec <= cur_ms && cur_ms < pCueTracks[i + 1].nMillisec)
			{
				nTrack = i;
			}
		}
		else
		{
			if (pCueTracks[i].nMillisec <= cur_ms)
			{
				nTrack = i;
			}
		}
	}

	wchar_t position[32] = { 0 }, total[32] = { 0 };
	plugin.language->FormattedTimeString(position, ARRAYSIZE(position), sec, 0, NULL);
	plugin.language->FormattedTimeString(total, ARRAYSIZE(total), total_sec, 0, NULL);

	if (nTrack >= 0)
	{
		if (pCueTracks[nTrack].szPerformer[0])
		{
			StringCchPrintf(coords, ARRAYSIZE(coords), L"%s - %s [%s / %s]",
							pCueTracks[nTrack].szPerformer,
							pCueTracks[nTrack].szTitle, position, total);
		}
		else
		{
			StringCchPrintf(coords, ARRAYSIZE(coords), L"%s [%s / %s]",
							pCueTracks[nTrack].szTitle, position, total);
		}
	}
	else
	{
		StringCchPrintf(coords, ARRAYSIZE(coords), L"%s / %s", position, total);
	}

	return coords;
}

int GetFileLengthMilliseconds(void)
{
	basicFileInfoStructW bfiW = { szFilename, 0, -1, NULL, 0 };
	return (GetBasicFileInfo(&bfiW, TRUE, TRUE) ? (bfiW.length * 1000) : -1000);
}

BOOL AllowedFile(const wchar_t * szFn)
{
	LPCWSTR extension = FindPathExtension(szFn);
	if (extension)
	{
		// for extensions that we know are going to trigger plug-ins that are
		// known to not be thread safe then we'll crudely add them here so as
		// avoid processing until (hopefully) those plug-ins can be improved!
		if (SameStr(extension, L"2sf") || SameStr(extension, L"mini2sf") ||
			SameStr(extension, L"gsf") || SameStr(extension, L"minigsf") ||
			SameStr(extension, L"ncsf") || SameStr(extension, L"minincsf") ||
			SameStr(extension, L"qsf") || SameStr(extension, L"minisqsf") ||
			SameStr(extension, L"snsf") || SameStr(extension, L"minisnsf") ||
			SameStr(extension, L"spc") || plugin.albumart->CanLoad(szFn))
		{
			return FALSE;
		}
	}
	return TRUE;
}

void RefreshInnerWindow(void)
{
	// deal with no playback to
	// ensure the window update
	if (!timer_id && IsWindow(hWndInner))
	{
		InvalidateRect(hWndInner, NULL, TRUE);
	}
}

const bool ProcessFilePlayback(const wchar_t *szFn, const bool start_playing,
							   const wchar_t *szArchiveFn, const bool only_check_exists)
{
	if (szFn && *szFn)
	{
		// this is used to try to deal with relative paths
		// where it can otherwise cause re-processing when
		// there's no need to actually do that when it has
		// already been processed & all of that fun stuff!
		wchar_t usable_path[MAX_PATH] = { 0 },
				archive_override[FILENAME_SIZE] = { 0 };
		const bool archive = (szArchiveFn && *szArchiveFn);
		ProcessPath((archive ? szArchiveFn : szFn), usable_path,
						   ARRAYSIZE(usable_path), FALSE, NULL);

		// we can sometimes see this message before the following
		// delay load message so we need to ensure that the paths
		// are setup correctly otherwise we get wrongly located
		// waveseek_in_*.dll in the music folders loaded from :(
		GetFilePaths();

		if (wcsistr(szFilename, usable_path) && bIsProcessing)
		{
			// if we're already processing and we're asked
			// to re-process (e.g. multiple clicks in the
			// main playlist editor) then we try to filter
			// out and keep going if it's the same file.
			bIsCurrent = SameStr(usable_path, GetPlayingFilename(0, archive_override));
			if (archive_override[0] && SameStr((archive ? szArchiveFn :
									   usable_path), archive_override))
			{
				bIsCurrent = true;
			}

			RefreshInnerWindow();
			return true;
		}

		ProcessStop();

		bIsLoaded = bUnsupported = false;
		bIsCurrent = SameStr((archive ? szArchiveFn : usable_path),
						  GetPlayingFilename(0, archive_override));
		if (archive_override[0] && SameStr((archive ? szArchiveFn :
								   usable_path), archive_override))
		{
			bIsCurrent = true;
		}

		size_t filename_remaining = ARRAYSIZE(szFilename);
		(void)StringCchCopyEx(szFilename, ARRAYSIZE(szFilename),
				  usable_path, NULL, &filename_remaining, NULL);
		szFilenameLen = (ARRAYSIZE(szFilename) - filename_remaining);

		nCueTracks = 0;

		// make sure that it's valid and something we can process
		if ((!IsPathURL(usable_path) && !IsCDEntry(usable_path) &&
			FilePathExists(usable_path, NULL) && AllowedFile(usable_path)) ||
			IsYTUrl(usable_path) || (IsZipEntry(usable_path) &&
			AllowedFile(usable_path)))
		{
			const bool zip_entry = (bIsCurrent && (IsZipEntry(usable_path) ||
											  IsZipEntry(archive_override)));
			LoadCUE(usable_path);

#ifdef WACUP_BUILD
			// for a smoother upgrade we'll still look
			// for the original cache file but a newer
			// file will be made with the better name.
			CombinePath(szWaveCacheFile, GetFilePaths(), FindPathFileName((zip_entry &&
							   archive_override[0] ? archive_override : usable_path)));

			size_t remaining = ARRAYSIZE(szWaveCacheFile);
			StringCchCatEx(szWaveCacheFile, ARRAYSIZE(szWaveCacheFile),
									L".cache", NULL, &remaining, NULL);

			if (!FileExists(szWaveCacheFile))
			{
				wchar_t cacheFile[61] = { 0 };
				if (GetFilenameHash((zip_entry && archive_override[0] ? archive_override :
									usable_path), (zip_entry && archive_override[0] ?
									wcslen(archive_override ): szFilenameLen), cacheFile))
				{
					StringCchCat(cacheFile, ARRAYSIZE(cacheFile), L".cache");
					CombinePath(szWaveCacheFile, GetFilePaths(), cacheFile);
				}
			}
#else
			// TODO apply the above to a non-WACUP install
			CombinePath(szWaveCacheFile, GetFilePaths(),
						FindPathFileName(usable_path));
			StringCchCat(szWaveCacheFile, ARRAYSIZE(szWaveCacheFile), L".cache");
#endif

			if (!FileExists(szWaveCacheFile))
			{
				if (only_check_exists)
				{
					return false;
				}

				EnterCriticalSection(&processing_cs);

				std::map<std::wstring, HANDLE>::const_iterator itr = processing_list.begin();
				while (itr != processing_list.end())
				{
					// found it so no need to re-add
					// as that's just going to cause
					// more processing / duplication
					if (wcsistr((zip_entry && archive && archive_override[0] ?
						archive_override : usable_path), (*itr).first.c_str()))
					{
						LeaveCriticalSection(&processing_cs);

						start_live_timer();

						RefreshInnerWindow();
						return true;
					}

					++itr;
				}

				// try to limit the number of files
				// being processed at the same time
				// to avoid causing some setups to
				// hang due to quick 'next' actions
				INT_PTR db_error = FALSE;
				static const int cpu_count = GetCpuProcs();
				const int count = static_cast<int>(processing_list.size());
				const bool can_process = (count < cpu_count);
				if (can_process)
				{
					const HANDLE thread = StartProcessingFile(usable_path, szArchiveFn,
															  start_playing, db_error);
					if (thread != NULL)
					{
						processing_list[std::wstring(usable_path)] = thread;
					}
				}

				LeaveCriticalSection(&processing_cs);
				
				start_live_timer();
			}
			else
			{
				const HANDLE h = CreateFile(szWaveCacheFile, GENERIC_READ, NULL, NULL,
											OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
				if (h != INVALID_HANDLE_VALUE)
				{
					DWORD dw = 0;
					(void)ReadFile(h, pSampleBuffer, SAMPLE_BUFFER_SIZE * sizeof(unsigned short), &dw, NULL);
					CloseHandle(h);
					if (dw > 0)
					{
						bIsLoaded = true;

						RefreshInnerWindow();
						return true;
					}
				}
			}
		}
	}

	// if the tooltip is being shown then we need to try
	// & update it so that it reflects the current view.
	if (!hideTooltip && IsWindowVisible(hWndToolTip))
	{
		const int lengthInMS = (bIsCurrent ? GetCurrentTrackLengthMilliSeconds() : GetFileLengthMilliseconds());
		POINT pt = { 0 };
		GetClientCursorPos(hWndInner, &pt);
		ti.lpszText = GetTooltipText(hWndInner, pt.x, lengthInMS);
		PostMessage(hWndToolTip, TTM_SETTOOLINFO, 0, (LPARAM)&ti);
	}

	RefreshInnerWindow();

	return false;
}

void ProcessSkinChange(BOOL skip_refresh = FALSE)
{
	clrBackground = WADlg_getColor(WADLG_ITEMBG);
	clrCuePoint = WADlg_getColor(WADLG_HILITE);
	clrGeneratingText = WADlg_getPlaylistSelectionTextColor();

	// get the current skin and use that as a
	// means to control the colouring used
	wchar_t szBuffer[MAX_PATH] = { 0 };
	GetCurrentSkin(szBuffer, ARRAYSIZE(szBuffer));

	if (szBuffer[0])
	{
		// use the skin provided value
		clrWaveformPlayed = clrGeneratingText;
		// give us something that should be ok
		clrWaveformFailed = WADlg_BlendColors(clrBackground, clrWaveformPlayed, (COLORREF)128);
	}
	else
	{
		// use a default value as the default
		// colour from the classic base skin
		// is almost white and looks wrong.
		clrWaveformPlayed = Beris(RGB(0, 178, 0));
		clrWaveformFailed = Beris(RGB(0, 96, 0));
	}

	clrWaveform = WADlg_getColor(WADLG_ITEMFG);

	// try to ensure that things can be seen in-case of
	// close colouring by the skin (matches what the ML
	// tries to do to ensure some form of visibility).
	if (abs(WADlg_GetColorDistance(clrWaveformPlayed, clrWaveform)) < 70)
	{ 
		clrWaveformPlayed = WADlg_BlendColors(clrBackground, clrWaveform, (COLORREF)77);
	}

	// attempt to now use the skin override options
	// which are provided in a waveseek.txt within
	// the root of the skin (folder or archive).
	if (szBuffer[0])
	{
		// look for the file that classic skins could provide
		AppendOnPath(szBuffer, L"waveseek.txt");
		if (FileExists(szBuffer))
		{
			clrBackground = GetPrivateProfileHex(L"colours", L"background", clrBackground, szBuffer);
			clrCuePoint = GetPrivateProfileHex(L"colours", L"cue_point", clrCuePoint, szBuffer);
			clrGeneratingText = GetPrivateProfileHex(L"colours", L"status_text", clrGeneratingText, szBuffer);
			clrWaveform = GetPrivateProfileHex(L"colours", L"wave_normal", clrWaveform, szBuffer);
			clrWaveformPlayed = GetPrivateProfileHex(L"colours", L"wave_playing", clrWaveformPlayed, szBuffer);
			clrWaveformFailed = GetPrivateProfileHex(L"colours", L"wave_failed", clrWaveformFailed, szBuffer);
		}
#ifndef _WIN64
		else
		{
			// otherwise look for (if loaded) anything within the
			// modern skin configuration for it's override colours
			clrBackground = GetFFSkinColour(L"plugin.waveseeker.background", clrBackground, true);
			clrCuePoint = GetFFSkinColour(L"plugin.waveseeker.cue_point", clrCuePoint, false);
			clrGeneratingText = GetFFSkinColour(L"plugin.waveseeker.status_text", clrGeneratingText, false);
			clrWaveform = GetFFSkinColour(L"plugin.waveseeker.wave_normal", clrWaveform, false);
			clrWaveformPlayed = GetFFSkinColour(L"plugin.waveseeker.wave_playing", clrWaveformPlayed, false);
			clrWaveformFailed = GetFFSkinColour(L"plugin.waveseeker.wave_failed", clrWaveformFailed, false);
		}
#endif
	}

	if (waveformRemaining != NULL)
	{
		DeleteObject(waveformRemaining);
	}
	waveformRemaining = CreateSolidBrush(clrWaveform);

	if (waveformPlayed != NULL)
	{
		DeleteObject(waveformPlayed);
	}
	waveformPlayed = CreateSolidBrush(clrWaveformPlayed);

	if (!skip_refresh)
	{
		InvalidateRect(hWndInner, NULL, TRUE);
	}

	MLSkinnedWnd_SkinChanged(hWndToolTip, FALSE, FALSE);
}

void ClearCacheFolder(const bool mode)
{
	for (int i = 0; i < 1 + !!mode; i++)
	{
		WIN32_FIND_DATA wfd = { 0 };
		wchar_t szFnFind[MAX_PATH] = { 0 };
		HANDLE hFind = SearchFolder(szFnFind, GetFilePaths(), (!i ? L"*.cache" : L"*.dll"), &wfd);
		if (hFind != INVALID_HANDLE_VALUE)
		{
			do
			{
				DeleteFile(CombinePath(szFnFind, GetFilePaths(), wfd.cFileName));
			} while (FindNextFile(hFind, &wfd));
			FindClose(hFind);
		}
	}
}

LPCWSTR GetSelectedFilePath(LPWSTR filepath)
{
	// update as needed to match the new setting
	// with fallback to the current playing if
	// there's no selection or it's been disabled
	int index = GetPlaylistPosition();
	if (clickTrack && GetSelectedCount())
	{
		const int sel = GetNextSelected((WPARAM)-1);
		if (sel != -1)
		{
			index = sel;
		}
	}

	return GetPlaylistItemFile(index, filepath);
}

void CALLBACK ProcessFileTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	KillTimer(hwnd, idEvent);

	PostMessage(plugin.hwndParent, WM_WA_IPC, (WPARAM)2, delay_load);
}

void CALLBACK CreateTooltipTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	KillTimer(hwnd, idEvent);

	if (!IsWindow(hWndToolTip))
	{
		hWndToolTip = CreateWindowEx(WS_EX_NOPARENTNOTIFY | WS_EX_TOPMOST,
									 TOOLTIPS_CLASS, NULL, TTS_NOPREFIX |
									 TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd,
									 NULL, plugin.hDllInstance, NULL);
	}

	if (IsWindow(hWndToolTip))
	{
		ti.cbSize = sizeof(TOOLINFO);
		ti.uFlags = TTF_TRACK | TTF_TRANSPARENT |
					TTF_ABSOLUTE | TTF_CENTERTIP;
		ti.hinst = plugin.hDllInstance;
		PostMessage(hWndToolTip, TTM_ADDTOOL, NULL, (LPARAM)&ti);

		SkinWindow(hWndToolTip, SKINNEDWND_TYPE_TOOLTIP,
				   SWS_COMMON_STYLE | SWTTIP_NOZORDERFIX, 0);
	}
}

bool ProcessMenuResult(const UINT command, HWND parent)
{
	switch (command)
	{
		case ID_SUBMENU_VIEWFILEINFO:
		{
			infoBoxParamW infoBoxW = {hWndWaveseek, szFilename};
			InfoBox(&infoBoxW, TRUE);
			break;
		}
		case ID_SUBMENU_CLEARWAVCACHEONEXIT:
		{
			clearOnExit = (!clearOnExit);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"clearOnExit",
										   (clearOnExit ? L"1" : NULL));
			break;
		}
		case ID_SUBMENU_CLEARWAVCACHE:
		{
			if (MessageBox(parent, WASABI_API_LNGSTRINGW(IDS_CLEAR_CACHE),
						   (LPWSTR)plugin.description, MB_YESNO | MB_ICONQUESTION) != IDYES)
			{
				break;
			}

			// trigger any in-progress processing to be cancelled
			// so nothing should be being re-saved out on clearing
			kill_threads = 1;

			ClearProcessingHandles();

			// remove all *.cache files in the users current cache folder.
			//
			// removing the waveseek_in_*.dll isn't covered as they are
			// still loaded and the unload / force delete will cause a
			// crash and so is not worth the hassle to do it. plus it
			// updates automatically as needed with the copy file checks :)
			ClearCacheFolder(0);

			// we will want to fall through so we
			// can then re-render the current file
		}
		[[fallthrough]];
		case ID_SUBMENU_RERENDER:
		{
			// support the older & newer cache filenames
			wchar_t filename[MAX_PATH] = { 0 };
			CombinePath(filename, GetFilePaths(),
						FindPathFileName(szFilename));

			size_t remaining = ARRAYSIZE(filename);
			StringCchCatEx(filename, ARRAYSIZE(filename), L".cache", NULL, &remaining, NULL);

			if (FileExists(filename))
			{
				DeleteFile(filename);
			}
			else
			{
				if (FileExists(szWaveCacheFile))
				{
					DeleteFile(szWaveCacheFile);
				}
				else
				{
					// thisis the final attempt to get a match which shouldn't
					// typically ever end up being called especially for zips!
					wchar_t cacheFile[61] = { 0 };
					if (GetFilenameHash(szFilename, szFilenameLen, cacheFile))
					{
						StringCchCat(cacheFile, ARRAYSIZE(cacheFile), L".cache");
						if (CheckForPath(filename, GetFilePaths(), cacheFile))
						{
							DeleteFile(filename);
						}
					}
				}
			}

			if (IsWindow(hWndInner))
			{
				timer_id = SetTimer(hWndInner, TIMER_ID, TIMER_FREQ, NULL);
			}
			bIsProcessing = false;

			ProcessStop();

			EnterCriticalSection(&processing_cs);

			std::map<std::wstring, HANDLE>::iterator itr = processing_list.begin();
			while (itr != processing_list.end())
			{
				// found it so no need to re-add
				// as that's just going to cause
				// more processing / duplication
				if (wcsistr(szFilename, (*itr).first.c_str()))
				{
					abort_processing_thread((*itr).second);

					processing_list.erase(itr);
					break;
				}

				++itr;
			}

			processing_list.clear();

			LeaveCriticalSection(&processing_cs);

			// wait a moment so if it was already
			// processing then that can clean-up
			// before we attempt a new processing
			Sleep(10);
			PostMessage(plugin.hwndParent, WM_WA_IPC, (WPARAM)1, delay_load);
			break;
		}
		case ID_CONTEXTMENU_CLICKTRACK:
		{
			clickTrack = (!clickTrack);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"clickTrack",
										   (clickTrack ? L"1" : NULL));

			// update as needed to match the new setting
			// with fallback to the current playing if
			// there's no selection or it's been disabled
			SetTimer(hWndInner, 8889, 50, ProcessFileTimerProc);
			break;
		}
		case ID_SUBMENU_SHOWCUEPOINTS:
		{
			showCuePoints = (!showCuePoints);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"showCuePoints",
										   (showCuePoints ? L"1" : NULL));

			// update as needed to match the new setting
			// with fallback to the current playing if
			// there's no selection or it's been disabled
			SetTimer(hWndInner, 8889, 50, ProcessFileTimerProc);
			break;
		}
		case ID_SUBMENU_HIDEWAVEFORMTOOLTIP:
		{
			hideTooltip = (!hideTooltip);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"hideTooltip",
										   (hideTooltip ? L"1" : NULL));

			if (!hideTooltip && IsWindow(hWndInner))
			{
				SetTimer(hWndInner, 8888, 333, CreateTooltipTimerProc);
			}
			break;
		}
		case ID_SUBMENU_RENDERWAVEFORMFORAUDIO:
		{
			audioOnly = (!audioOnly);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"audioOnly",
										   (audioOnly ? NULL : L"0"));
			break;
		}
		case ID_SUBMENU_RENDERWAVEFORMUSINGALOWERPRIORITY:
		{
			lowerpriority = (!lowerpriority);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"lowerpriority",
										   (lowerpriority ? L"1" : NULL));

			EnterCriticalSection(&processing_cs);

			// update the threads that are already running
			std::map<std::wstring, HANDLE>::iterator itr = processing_list.begin();
			while (itr != processing_list.end())
			{
				const HANDLE handle = (*itr).second;
				if (handle != NULL)
				{
					SuspendThread(handle);
					SetThreadPriority(handle, (!lowerpriority ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_LOWEST));
					ResumeThread(handle);
				}
			}

			LeaveCriticalSection(&processing_cs);
			break;
		}
#ifndef _WIN64
		case ID_SUBMENU_USELEGACYPROCESSINGMODE:
		{
			legacy = (!legacy);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"legacy",
										   (legacy ? L"1" : NULL));
			if (legacy)
			{
				Subclass(hWndWaveseek, EmdedWndProc);
			}
			else
			{
				UnSubclass(hWndWaveseek, EmdedWndProc);
			}
			break;
		}
#endif
		case ID_SUBMENU_SHOWDEBUGGINGMESSAGES:
		{
			debug = (!debug);
			SaveNativeIniString(WINAMP_INI, L"Waveseek", L"debug",
										   (debug ? L"1" : NULL));
			break;
		}
		case ID_SUBMENU_ABOUT:
		{
			wchar_t message[512] = { 0 };
			StringCchPrintf(message, ARRAYSIZE(message), WASABI_API_LNGSTRINGW(IDS_ABOUT_STRING), TEXT(__DATE__));
			//MessageBox(plugin.hwndParent, message, pluginTitleW, 0);
			AboutMessageBox(plugin.hwndParent, message, (LPWSTR)plugin.description);
			break;
		}
		default:
		{
			return false;
		}
	}
	return true;
}

void SetupConfigMenu(HMENU popup)
{
	CheckMenuItem(popup, ID_CONTEXTMENU_CLICKTRACK, MF_BYCOMMAND | (clickTrack ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(popup, ID_SUBMENU_CLEARWAVCACHEONEXIT, MF_BYCOMMAND | (clearOnExit ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(popup, ID_SUBMENU_SHOWCUEPOINTS, MF_BYCOMMAND | (showCuePoints ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(popup, ID_SUBMENU_HIDEWAVEFORMTOOLTIP, MF_BYCOMMAND | (hideTooltip ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(popup, ID_SUBMENU_RENDERWAVEFORMFORAUDIO, MF_BYCOMMAND | (audioOnly ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(popup, ID_SUBMENU_RENDERWAVEFORMUSINGALOWERPRIORITY, MF_BYCOMMAND | (lowerpriority ? MF_CHECKED : MF_UNCHECKED));
#ifndef _WIN64
	CheckMenuItem(popup, ID_SUBMENU_USELEGACYPROCESSINGMODE, MF_BYCOMMAND | (legacy ? MF_CHECKED : MF_UNCHECKED));
#endif
	CheckMenuItem(popup, ID_SUBMENU_SHOWDEBUGGINGMESSAGES, MF_BYCOMMAND | (debug ? MF_CHECKED : MF_UNCHECKED));
}

#ifndef _WIN64
LRESULT CALLBACK EmdedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
							  UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	switch (uMsg)
	{
		case WM_WA_MPEG_EOF:
		{
			FinishProcessingFile(szWaveCacheFile, NULL);
			break;
		}
		case WM_WA_IPC:
		{
			// this is used to pass on some of the messages but not all
			// since we use this window as a fake 'main' window so some
			// of the messages like WM_WA_MPEG_EOF are blocked from the
			// real main window when using the dll re-use hack in place
			return SendMessage(plugin.hwndParent, uMsg, wParam, lParam);
		}
	}

	return DefSubclass(hWnd, uMsg, wParam, lParam);
}
#endif

LRESULT CALLBACK InnerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// if you need to do other message handling then you can just place this first and
	// process the messages you need to afterwards. note this is passing the frame and
	// its id so if you have a few embedded windows you need to do this with each child
	if (HandleEmbeddedWindowChildMessages(hWndWaveseek, WINAMP_WAVEFORM_SEEK_MENUID,
										  hWnd, uMsg, wParam, lParam))
	{
		return 0;
	}

	bool bForceJump = false;
	switch (uMsg)
	{
		case WM_COMMAND:	// for what's handled from the accel table
		{
			if (ProcessMenuResult(LOWORD(wParam), hWnd))
			{
				break;
			}
		}
		[[fallthrough]];
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_SYSCHAR:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		{
			// this is needed to avoid it causing a beep
			// due to it not being reported as unhandled
			EatKeyPress();
		}
		[[fallthrough]];
		case WM_MOUSEWHEEL:
		{
			PostMessage(plugin.hwndParent, uMsg, wParam, lParam);
			break;
		}
		case WM_CREATE:
		{
			if (!hideTooltip)
			{
				SetTimer(hWnd, 8888, 333, CreateTooltipTimerProc);
			}

			// if we've been created but processing is already
			// underway then we need to get things going on to
			// ensure the processing will be shown. embedding
			// the window in the BBM skin can cause this issue
			if (bIsProcessing)
			{
				timer_id = SetTimer(hWnd, TIMER_ID, TIMER_LIVE_FREQ, NULL);
			}
			break;
		}
		case WM_TIMER:
		{
			if ((wParam == TIMER_ID) && IsWindowVisible(hWnd))
			{
				InvalidateRect(hWnd, NULL, TRUE);
			}
			break;
		}
		case WM_NCPAINT:
		{
			return 0;
		}
		case WM_SHOWWINDOW:
		{
			// we allow the window to draw but wait a bit
			// for the waveform to be allowed to render &
			// prevent it blocking loading for too long
			paint_allowed = !!wParam;
			break;
		}
		case WM_ERASEBKGND:
		{
			const HDC hdc = ((kill_threads != 2) ? (HDC)wParam : NULL);
			if (hdc)
			{
				// we get the client area instead of
				// using the paint area as it's not
				// the same if partially off-screen
				RECT wnd = { 0 };
				GetClientRect(hWnd, &wnd);

				const int nSongPos = (bIsCurrent ? GetCurrentTrackPos() : 0),
						  nSongLen = (bIsCurrent ? GetCurrentTrackLengthMilliSeconds() :
														   GetFileLengthMilliseconds()),
						  nBufPos = (nSongLen != -1 ? MulDiv(nSongPos, SAMPLE_BUFFER_SIZE, nSongLen) : 0),
						  h = (wnd.bottom - wnd.top);
				int w = (wnd.right - wnd.left);

				// it's a bit quicker to make use of a cached dc instead of making
				// one for every paint event so we'll check if our cached dc is ok
				// for the size of the window it needs to be painted or re-create
				if (!cacheDC || memcmp(&lastWnd, &wnd, sizeof(RECT)))
				{
					const HDC oldCacheDC = cacheDC;
					cacheDC = CreateCompatibleDC(hdc);

					if (cacheBMP)
					{
						DeleteObject(cacheBMP);
					}
					cacheBMP = CreateCompatibleBitmap(hdc, w, h);

					DeleteObject(SelectObject(cacheDC, cacheBMP));

					if (oldCacheDC)
					{
						DeleteDC(oldCacheDC);
					}

					lastWnd = wnd;

					const size_t wave_points_size = ((w + 4) * sizeof(POINT));

					if (wave_remaining_points)
					{
						plugin.memmgr->sysFree(wave_remaining_points);
					}
					wave_remaining_points = (LPPOINT)plugin.memmgr->sysMalloc(wave_points_size);

					if (wave_points)
					{
						plugin.memmgr->sysFree(wave_points);
					}
					wave_points = (LPPOINT)plugin.memmgr->sysMalloc(wave_points_size);
				}

				if (paint_allowed)
				{
					FillRectWithColour(cacheDC, &wnd, clrBackground, TRUE);

					// make the width a bit less so the right-edge
					// can allow for the end of the track to be
					// correctly selected or the tooltip show up
					--w;

					const int height = (h / 2);
					if ((bIsLoaded || bIsProcessing) && !bUnsupported)
					{
						SelectObject(cacheDC, GetStockObject(DC_PEN));

						int num_points = 1, num_remaining_points = 0;

						wave_points[0].x = 0;
						wave_points[0].y = height;

						const bool has_cue = (showCuePoints && (nCueTracks > 0));
						bool changed = false, different = false;
						for (int i = 0; i < w; i++)
						{
							const int nBufLoc0 = ((i * SAMPLE_BUFFER_SIZE) / w),
									  nBufLoc1 = (const int)min((((i + 1) * SAMPLE_BUFFER_SIZE) / w), SAMPLE_BUFFER_SIZE);

							// only update this when there's a need
							if (nSongPos && (nBufLoc0 >= nBufPos) && !changed)
							{
								changed = true;

								wave_remaining_points[num_remaining_points].x = i;
								wave_remaining_points[num_remaining_points].y = height;
								++num_remaining_points;
							}

							unsigned short nSample = 0;
							for (int j = nBufLoc0; j < nBufLoc1 && j < SAMPLE_BUFFER_SIZE; j++)
							{
								nSample = max(pSampleBuffer[j], nSample);
							}

							const unsigned short sh = static_cast<const unsigned short>((nSample * h) / 32767);
							const int y = ((h - sh) / 2);

							const int old_y = wave_points[num_points].y,
									  old_remaining_y = wave_remaining_points[num_remaining_points].y;
							if (!changed)
							{
								wave_points[num_points].x = num_points;
								wave_points[num_points].y = y;
							}
							else
							{
								wave_remaining_points[num_remaining_points].x = i;
								wave_remaining_points[num_remaining_points].y = y;
							}

							if (!changed)
							{
								++num_points;
							}
							else
							{
								++num_remaining_points;
							}

							if (!different && ((!changed ? (y != old_y) : (y != old_remaining_y))))
							{
								different = true;
							}
							/*/if (sh)
							{
								MoveToEx(cacheDC, i, y, NULL);
								LineTo(cacheDC, i, (y + sh));
							}/**/

							if (has_cue)
							{
								const unsigned int ms = MulDiv(i, nSongLen, w);
								if (ms > 0)
								{
									for (int k = 0; k <= nCueTracks; k++)
									{
										if (!pCueTracks[k].bDrawn &&
											(pCueTracks[k].nMillisec > 0) &&
											(pCueTracks[k].nMillisec < ms))
										{
											pCueTracks[k].bDrawn = true;
											pCueTracks[k].nDrawnPos = i;
											break;
										}
									}
								}
							}
						}

						if (num_points)
						{
							wave_points[num_points].x = num_points;
							wave_points[num_points].y = height;
							wave_points[++num_points].x = 0;
							wave_points[num_points].y = height;
							wave_points[++num_points].x = 0;
							wave_points[num_points].y = wave_points[0].y;
						}
						
						if (num_remaining_points)
						{
							wave_remaining_points[num_remaining_points].x = (num_points + num_remaining_points + 1);
							wave_remaining_points[num_remaining_points].y = height;
							++num_remaining_points;
							wave_remaining_points[num_remaining_points].x = wave_remaining_points[0].x;
							wave_remaining_points[num_remaining_points].y = height;
						}

						if (different)
						{
							if (waveformRemainingRgn)
							{
								DeleteObject(waveformRemainingRgn);
							}

							waveformRemainingRgn = CreatePolygonRgn(wave_remaining_points, num_remaining_points, ALTERNATE);

							if (waveformRgn)
							{
								DeleteObject(waveformRgn);
							}

							waveformRgn = CreatePolygonRgn(wave_points, num_points, ALTERNATE);
						}

						if (waveformRgn)
						{
							FillRgn(cacheDC, waveformRgn, (nSongPos ? waveformPlayed : waveformRemaining));/*/
							SetDCPenColor(cacheDC, (nSongPos ? clrWaveformPlayed : clrWaveform));
							PolylineTo(cacheDC, wave_points, num_points);/**/
						}

						if (waveformRemainingRgn)
						{
							FillRgn(cacheDC, waveformRemainingRgn, waveformRemaining);/*/
							SetDCPenColor(cacheDC, clrWaveform);
							PolylineTo(cacheDC, wave_remaining_points, num_remaining_points);/**/
						}

						if (has_cue)
						{
							for (int k = 0; k <= nCueTracks; k++)
							{
								const int i = pCueTracks[k].nDrawnPos;

								const COLORREF old_colour = SetDCPenColor(cacheDC, clrCuePoint);
								MoveToEx(cacheDC, i - 1, 0, NULL);
								LineTo(cacheDC, i - 1, height);
								MoveToEx(cacheDC, i, 0, NULL);
								LineTo(cacheDC, i, height);

								// need to reset or it'll go wonky
								SetDCPenColor(cacheDC, old_colour);

								pCueTracks[k].bDrawn = false;
							}
						}

						// and now restore so that we get drawn correctly
						++w;

						// copy the upper portion & then flip that to form the full image as that
						// under profiling is quicker compared to doing loads of move/line calls
						/*BitBlt(hdc, 0, 0, w, h, cacheDC, 0, 0, SRCCOPY);;
						/*/
						StretchBlt(hdc, 0, 0, w, height, cacheDC, 0, 0, w, height, SRCCOPY);
						StretchBlt(hdc, 0, h, w, -height - 2, cacheDC, 0, 0, w, height, SRCCOPY);/**/
					}
					else
					{
						if (debug)
						{
							const HFONT old_font = (HFONT)SelectObject(cacheDC, WADlg_getFont());

							SetBkColor(cacheDC, clrBackground);
							SetTextColor(cacheDC, clrGeneratingText);

							DrawText(cacheDC, (!IsPathURL(szFilename) && !IsZipEntry(szFilename) ?
									 ((bUnsupported == 2) ? szBadPlugin : ((bUnsupported == 3) ?
									 szLegacy : szUnavailable)) : szStreamsNotSupported), -1, &wnd,
									 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

							SelectObject(cacheDC, old_font);
						}
						else
						{
#ifdef USE_GDIPLUS
							const bool plain = false;
							if (plain)
							{
#endif
								const HDC hdcMem2 = CreateCompatibleDC(hdc);
								const HBITMAP hbmMem2 = CreateCompatibleBitmap(hdc, w, h),
											  hbmOld2 = (HBITMAP)SelectObject(hdcMem2, hbmMem2);

								FillRectWithColour(hdcMem2, &wnd, clrBackground, TRUE);

								// draw a sine wave to indicate we're still
								// there but that we've not got anything to
								// show either from being missing / invalid
								for (int k = 0; k < 2; k++)
								{
									const HDC thisdc = (!k ? cacheDC : hdcMem2);

									SelectObject(thisdc, GetStockObject(DC_PEN));

									// draw a base line in the middle of the window
									SetDCPenColor(thisdc, (!nBufPos ? clrWaveformFailed : (!k ?
															clrWaveform : clrWaveformPlayed)));
									MoveToEx(thisdc, 0, height, NULL);

									if (!k)
									{
										LineTo(thisdc, w, height);
										MoveToEx(thisdc, 0, height, NULL);
									}

									const bool playing = (k && (nBufPos > 0));
									for (int j = 0; j < ((w / 256) + 1); j++)
									{
#define num_points 4096
										LPPOINT points = (LPPOINT)plugin.memmgr->sysMalloc(num_points * sizeof(POINT));
										if (points)
										{
											for (int i = 0; i < num_points; i++)
											{
												points[i].x = (j * 256) + ((i * 256) / num_points);
												points[i].y = (int)((h / 2.0f) * (1 - sin((4.0f * M_PI) * i / num_points)));
											}

											PolylineTo(thisdc, points, num_points);

											// only fill in things when its needed
											const HRGN rgn = (playing ? CreatePolygonRgn(points, num_points, ALTERNATE) : NULL);
											if (rgn)
											{
												FillRgn(thisdc, rgn, waveformPlayed);
												DeleteObject(rgn);
											}
											plugin.memmgr->sysFree(points);
										}
									}

									// ensure the middle line will show in playing mode
									if (playing)
									{
										SetDCPenColor(thisdc, clrWaveform);
										MoveToEx(thisdc, 0, height, NULL);
										LineTo(thisdc, w, height);
									}
								}

								// only copy in things when its needed
								if (nBufPos > 0)
								{
									BitBlt(cacheDC, 0, 0, MulDiv(nSongPos, w,
										   nSongLen), h, hdcMem2, 0, 0, SRCCOPY);
								}

								SelectObject(hdcMem2, hbmOld2);
								DeleteObject(hbmMem2);
								DeleteDC(hdcMem2);
#ifdef USE_GDIPLUS
							}
							else
							{
								Gdiplus::Graphics graphics(cacheDC);
								Gdiplus::Pen* pen = new Gdiplus::Pen(Gdiplus::Color(GetRValue(clrWaveformFailed),
																					GetGValue(clrWaveformFailed),
																					GetBValue(clrWaveformFailed)));
								if (pen)
								{
									graphics.DrawLine(pen, 0, y, w, y);

									/*graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);/*/
									graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);/**/

									// draw a sine wave to indicate we're still
									// there but that we've not got anything to
									// show either from being missing / invalid
									for (int j = 0; j < (w / 256) + 1; j++)
									{
#define num_point 4096
										Gdiplus::PointF points[num_point];
										for (int i = 0; i < num_point; i++)
										{
											points[i].X = (j * 256) + ((i * 256) / num_point);
											points[i].Y = (int)((h / 2.0f) * (1 - sinf((4.0f * M_PI) * i / num_point)));
										}
										graphics.DrawLines(pen, points, num_point);
									}

									delete pen;
								}
							}
#endif
						}

						// and now restore so that we get drawn correctly
						++w;

						BitBlt(hdc, 0, 0, w, h, cacheDC, 0, 0, SRCCOPY);
					}
				}
			}
			return 1;
		}
		// make sure we catch all appropriate skin changes
		case WM_USER + 0x202:	// WM_DISPLAYCHANGE / IPC_SKIN_CHANGED_NEW
		{
			ProcessSkinChange();
			break;
		}
		case WM_USER + 0x98:
		//case WM_CONTEXTMENU:
		{
			int xPos = GET_X_LPARAM(lParam),
				yPos = GET_Y_LPARAM(lParam);

			HMENU hMenu = WASABI_API_LOADMENUW(IDR_CONTEXTMENU);
			HMENU popup = GetSubMenu(hMenu, 0);
			SetupConfigMenu(popup);

			// this will handle the menu being shown not via the mouse actions
			// so is positioned just below the header if no selection but there's a queue
			// or below the item selected (or the no files in queue entry)
			if ((xPos == -1) || (yPos == -1))
			{
				RECT rc = { 0 };
				GetWindowRect(hWnd, &rc);
				xPos = (short)rc.left;
				yPos = (short)rc.top;
			}

			ProcessMenuResult(TrackPopup(popup, TPM_RIGHTBUTTON | TPM_RETURNCMD,
										 xPos, yPos, hWnd), hWnd);

			DestroyMenu(hMenu);
			break;
		}
		case WM_LBUTTONDBLCLK:
		{
			// start playing current or selection as needed
			// this also jumps to the time point where the click
			// happened as a coincidence of the mesages received
			if (bIsCurrent)
			{
				if (!GetPlayingState())
				{
					PostMessage(plugin.hwndParent, WM_COMMAND,
								MAKEWPARAM(WINAMP_BUTTON2, 0), 0);
					bForceJump = true;
				}
				// we'll fall through to WM_LBUTTONUP the handling
				// so that we also do jump to the desired point of
				// the double-click
			}
			else
			{
				if (GetSelectedCount())
				{
					const int sel = GetNextSelected((WPARAM)-1);
					if (sel != -1)
					{
						// update the position and then fake hitting play
						SendMessage(plugin.hwndParent, WM_WA_IPC,
									sel, IPC_SETANDPLAYLISTPOS);
					}
				}
				break;
			}
		}
		[[fallthrough]];
		case WM_LBUTTONUP:
		{
			if (bIsCurrent)
			{
				const int nSongLen = GetCurrentTrackLengthMilliSeconds();
				if (nSongLen != -1 || bForceJump)
				{
					RECT rc = { 0 };
					GetClientRect(hWnd, &rc);
					const unsigned int ms = MulDiv(GET_X_LPARAM(lParam), nSongLen, (rc.right - rc.left));
					// if not forcing a jump then just send as-is
					if (!bForceJump)
					{
						JumpToTime(ms);
					}
					else
					{
						// but if we need to force due to a double-click start
						// then we need to wait a little bit before we can try
						// to seek else it'll fail as the file won't be in the
						// playing state so soon after sending start playback.
						Sleep(100);
						PostMessage(plugin.hwndParent, WM_WA_IPC, ms, IPC_JUMPTOTIME);
					}
				}
			}
			break;
		}
		case WM_MOUSEMOVE:
		{
			if (!hideTooltip)
			{
				static short xOldPos = 0; 
				static short yOldPos = 0; 
				short xPos = GET_X_LPARAM(lParam); 
				short yPos = GET_Y_LPARAM(lParam); 

				if (xPos == xOldPos && yPos == yOldPos)
				{
					break;
				}

				xOldPos = xPos;
				yOldPos = yPos;

				const int lengthInMS = (bIsCurrent ? GetCurrentTrackLengthMilliSeconds() : GetFileLengthMilliseconds());
				if (lengthInMS > 0)
				{
					// ensures we'll get a WM_MOUSELEAVE 
					TRACKMOUSEEVENT trackMouse = { 0 };
					trackMouse.cbSize = sizeof(trackMouse);
					trackMouse.dwFlags = TME_LEAVE;
					trackMouse.hwndTrack = hWnd;
					TrackMouseEvent(&trackMouse);

					POINT pt = { xPos, yPos };
					ClientToScreen(hWndWaveseek, &pt);
					ti.lpszText = GetTooltipText(hWnd, xPos, lengthInMS);

					RECT rt = { 0 };
					GetClientRect(hWndToolTip, &rt);

					PostMessage(hWndToolTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
					PostMessage(hWndToolTip, TTM_SETTOOLINFO, 0, (LPARAM)&ti);
					PostMessage(hWndToolTip, TTM_TRACKPOSITION, 0, (LPARAM)MAKELONG(
								pt.x + (WORD)((rt.right - rt.left) / 1.5f), pt.y));
				}
			}
			break;
		}
		case WM_MOUSELEAVE:
		{
			PostMessage(hWndToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
			break;
		}
		case WM_CLOSE:
		{
			// ths is needed to avoid the inner window being
			// destroyed when trying to close the outer one!
			return 0;
		}
		case WM_DESTROY:
		{
			if (IsWindow(hWndToolTip))
			{
				DestroyWindow(hWndToolTip);
				hWndToolTip = NULL;
			}

			if (WASABI_API_APP != NULL)
			{
				WASABI_API_APP->app_removeAccelerators(hWnd);
			}
			break;
		}
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void __cdecl MessageProc(HWND hWnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam)
{
	if (uMsg == WM_WA_IPC)
	{
		if ((lParam == IPC_PLAYING_FILEW) || ((lParam == IPC_CB_MISC) &&
			((wParam == IPC_CB_MISC_TITLE) || (wParam == IPC_CB_MISC_STATUS))) ||
			((lParam == IPC_PLITEM_SELECTED_CHANGED) && clickTrack))
		{
			// update as needed to match the new setting
			// with fallback to the current playing if
			// there's no selection or it's been disabled
			SetTimer(hWndInner, 8889, 50, ProcessFileTimerProc);
		}
		else if (lParam == IPC_STOPPLAYING)
		{
			// if we're doing a proper stop then we're best
			// turning off the timer as long as there's not
			// processing still happening to lower idle cpu
			if (timer_id && !bIsProcessing)
			{
				ProcessStop(((const stopPlayingInfoStructEx*)wParam)->is_closing);
			}
		}
		else if (lParam == IPC_GET_EMBEDIF_NEW_HWND)
		{
			if (((HWND)wParam == hWndWaveseek) && visible &&
				(InitialShowState() != SW_SHOWMINIMIZED))
			{
				// only show on startup if under a classic skin and was set
				ShowHideEmbeddedWindow(hWndWaveseek, TRUE, FALSE);
			}
		}
		else if (lParam == delay_load)
		{
			if (!wParam)
			{
				GetFilePaths();

				// just incase we need to handle a migration update, we check
				// for a Winamp\Plugins\wavecache folder and if found then we
				// move it into the correct settings folder (due to UAC, etc)
				/*if (!GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"Migrate", 0))
				{
					wchar_t szOldWaveCacheDir[MAX_PATH] = { 0 };
					if (CheckForPath(szOldWaveCacheDir, szDLLPath, L"wavecache"))
					{
						wchar_t szFnFind[MAX_PATH] = { 0 };
						CombinePath(szFnFind, szOldWaveCacheDir, L"*.cache");

						WIN32_FIND_DATA wfd = { 0 };
						HANDLE hFind = FindFirstFile(szFnFind, &wfd);
						if (hFind != INVALID_HANDLE_VALUE)
						{
							do
							{
								// if we found a *.cache file then move it over
								// as long as there's permission and the OS can
								wchar_t szFnMove[MAX_PATH] = { 0 };
								CombinePath(szFnFind, szOldWaveCacheDir, wfd.cFileName);
								CombinePath(szFnMove, GetFilePaths(), wfd.cFileName);
								MoveFileEx(szFnFind, szFnMove, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
							}
							while (FindNextFile(hFind,&wfd));
							CloseHandle(hFind);
						}
					}
					if (IsDirectoryEmpty(szOldWaveCacheDir))
					{
						RemoveDirectory(szOldWaveCacheDir);
					}
					SaveNativeIniString(WINAMP_INI, L"Waveseek", L"Migrate", L"1");
				}*/

				clearOnExit = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"clearOnExit", clearOnExit);
				clickTrack = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"clickTrack", clickTrack);
				showCuePoints = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"showCuePoints", showCuePoints);
				hideTooltip = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"hideTooltip", hideTooltip);
				audioOnly = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"audioOnly", audioOnly);
				lowerpriority = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"lowerpriority", lowerpriority);
#ifndef _WIN64
				legacy = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"legacy", legacy);
#endif
				debug = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"debug", debug);

				// just get the colours but no need to do
				// a refresh as that will cause a ui lag!
				ProcessSkinChange(TRUE);

				// for the purposes of this example we will manually create an accelerator table so
				// we can use IPC_REGISTER_LOWORD_COMMAND to get a unique id for the menu items we
				// will be adding into Winamp's menus. using this api will allocate an id which can
				// vary between Winamp revisions as it moves depending on the resources in Winamp.
				WINAMP_WAVEFORM_SEEK_MENUID = RegisterCommandID(0);

				// then we show the embedded window which will cause the child window to be
				// sized into the frame without having to do any thing ourselves. also this will
				// only show the window if Winamp was not minimised on close and the window was
				// open at the time otherwise it will remain hidden
				old_visible = visible = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"wnd_open", TRUE);

				// finally we add menu items to the main right-click menu and the views menu
				// with Modern skins which support showing the views menu for accessing windows
				wchar_t lang_string[32] = { 0 };
				AddEmbeddedWindowToMenus(WINAMP_WAVEFORM_SEEK_MENUID, WASABI_API_LNGSTRINGW_BUF(IDS_WAVEFORM_SEEKER_MENU,
																	  lang_string, ARRAYSIZE(lang_string)), visible, -1);

				// now we will attempt to create an embedded window which adds its own main menu entry
				// and related keyboard accelerator (like how the media library window is integrated)
				embed.flags |= EMBED_FLAGS_SCALEABLE_WND;	// double-size support!
				hWndWaveseek = CreateEmbeddedWindow(&embed, embed_guid, WASABI_API_LNGSTRINGW_BUF(IDS_WAVEFORM_SEEKER,
																				lang_string, ARRAYSIZE(lang_string)));

#ifndef _WIN64
				// there's no need to be subclassing the
				// window unless we're using legacy mode
				// TODO might it be simpler to just have
				//		it call this window instead... ?
				if (legacy)
				{
					Subclass(hWndWaveseek, EmdedWndProc);
				}
#endif

				WNDCLASSEX wcex = { 0 };
				wcex.cbSize = sizeof(WNDCLASSEX);
				wcex.lpszClassName = L"WaveseekWnd";
				wcex.hInstance = plugin.hDllInstance;
				wcex.lpfnWndProc = InnerWndProc;
				wcex.style = CS_DBLCLKS; // so we get double-clicks
				wcex.hCursor = GetArrowCursor(false);
				wndclass = RegisterClassEx(&wcex);
				if (wndclass)
				{
					hWndInner = CreateWindowEx(WS_EX_NOPARENTNOTIFY, (LPCTSTR)wndclass, 0, WS_CHILD |
											   WS_VISIBLE, 0, 0, 0, 0, hWndWaveseek, (HMENU)101,
											   plugin.hDllInstance, (LPVOID)hWndWaveseek);

					if (IsWindow(hWndInner))
					{
						// just to be certain if the skinned preferences support is installed
						// we want to ensure that we're not going to have it touch our window
						// as it can otherwise cause some occassional drawing issues/clashes.
						//SetProp(hWndInner, L"SKPrefs_Ignore", (HANDLE)1);

						HACCEL accel = WASABI_API_LOADACCELERATORSW(IDR_ACCELERATOR_WND);
						if (accel)
						{
							WASABI_API_APP->app_addAccelerators(hWndInner, &accel, 1, TRANSLATE_MODE_NORMAL);
						}
					}
				}

				WASABI_API_LNGSTRINGW_BUF(IDS_WAVEFORM_UNAVAILABLE, szUnavailable, ARRAYSIZE(szUnavailable));
				WASABI_API_LNGSTRINGW_BUF(IDS_WAVEFORM_UNAVAILABLE_BAD_PLUGIN, szBadPlugin, ARRAYSIZE(szBadPlugin));
				WASABI_API_LNGSTRINGW_BUF(IDS_STREAMS_NOT_SUPPORTED, szStreamsNotSupported, ARRAYSIZE(szStreamsNotSupported));
				WASABI_API_LNGSTRINGW_BUF(IDS_TRY_LEGACY_MODE, szLegacy, ARRAYSIZE(szLegacy));

				// Note: WASABI_API_APP->app_addAccelerators(..) requires Winamp 5.53 and higher
				//       otherwise if you want to support older clients then you could use the
				//       IPC_TRANSLATEACCELERATOR callback api which works for v5.0 upto v5.52
				ACCEL accel = { FVIRTKEY | FALT, 'R', (WORD)WINAMP_WAVEFORM_SEEK_MENUID };
				HACCEL hAccel = CreateAcceleratorTable(&accel, 1);
				if (hAccel)
				{
					plugin.app->app_addAccelerators(hWndInner, &hAccel, 1, TRANSLATE_MODE_GLOBAL);
				}

				// Winamp can report if it was started minimised which allows us to control our window
				// to not properly show on startup otherwise the window will appear incorrectly when it
				// is meant to remain hidden until Winamp is restored back into view correctly
				if ((InitialShowState() == SW_SHOWMINIMIZED))
				{
					SetEmbeddedWindowMinimisedMode(hWndWaveseek, MINIMISED_FLAG, TRUE);
				}
				/*else
				{
					// only show on startup if under a classic skin and was set
					if (visible)
					{
						ShowHideEmbeddedWindow(hWndWaveseek, TRUE, FALSE);
					}
				}*/
			}
			else if (wParam <= 2)
			{
				const bool check_only = (wParam == 1);

				// allow new threads to be spawned
				// after we've cleaned up existing
				// skipping the timer for now as a
				// brief appearance of the missing
				// waveform can show vs processed.
				if (check_only)
				{
					kill_threads = 0;
				}

				wchar_t filepath[FILENAME_SIZE] = { 0 };
				if (!ProcessFilePlayback(((wParam == 1) ? szFilename :
										 GetSelectedFilePath(filepath)),
										 check_only, NULL, check_only))
				{
					if (check_only)
					{
						SetTimer(hWndInner, 8889, 50, ProcessFileTimerProc);
					}
					else
					{
						RefreshInnerWindow();
					}
				}
				else
				{
					RefreshInnerWindow();
				}
			}
		}
		/*else if (lParam == IPC_WACUP_HAS_LOADED)
		{
			// used to help avoid some initial load painting issues
			// which is more of an issue with wine since wacup with
			// it's non-legacy mode has become the default instance
			if (IsWindow(hWndInner))
			{
				timer_id = SetTimer(hWndInner, TIMER_ID, TIMER_FREQ, NULL);
			}
		}*/
	}

	// this will handle the message needed to be caught before the original window
	// proceedure of the subclass can process it. with multiple windows then this
	// would need to be duplicated for the number of embedded windows your handling
	HandleEmbeddedWindowWinampWindowMessages(hWndWaveseek, WINAMP_WAVEFORM_SEEK_MENUID,
											 &embed, hWnd, uMsg, wParam, lParam);
}

int PluginInit(void) 
{
#ifdef WACUP_BUILD
	WASABI_API_SVC = plugin.service;
#else
	// load all of the required wasabi services from the winamp client
	WASABI_API_SVC = reinterpret_cast<api_service*>(SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_GET_API_SERVICE));
	if (WASABI_API_SVC == reinterpret_cast<api_service*>(1)) WASABI_API_SVC = NULL;
#endif
	if (WASABI_API_SVC != NULL)
	{
		ServiceBuild(WASABI_API_SVC, WASABI_API_DECODEFILE2, decodeFile2GUID);
#ifdef WACUP_BUILD
		WASABI_API_APP = plugin.app;

		WASABI_API_START_LANG_DESC(plugin.language, plugin.hDllInstance,
								   embed_guid, IDS_PLUGIN_NAME, TEXT(
								   PLUGIN_VERSION), &plugin.description);
#else
		ServiceBuild(WASABI_API_SVC, WASABI_API_APP, applicationApiServiceGuid);
		ServiceBuild(WASABI_API_SVC, WASABI_API_LNG, languageApiGUID);

		// TODO add to lang.h
		WASABI_API_START_LANG(plugin.hDllInstance, embed_guid);

		wchar_t	pluginTitleW[256] = { 0 };
		StringCchPrintf(pluginTitleW, ARRAYSIZE(pluginTitleW), WASABI_API_LNGSTRINGW(IDS_PLUGIN_NAME), TEXT(PLUGIN_VERSION));
		plugin.description = (char*)plugin.memmgr->sysDupStr(pluginTitleW);
#endif

		InitializeCriticalSectionEx(&processing_cs, 400, CRITICAL_SECTION_NO_DEBUG_INFO);

		// restore / process the current file so we're showing something on load
		// but we delay it a bit until Winamp is in a better state especially if
		// we then fire offa file processing action otherwise we slow down startup
		delay_load = RegisterIPC((WPARAM)&"wave_seeker");
		PostMessage(plugin.hwndParent, WM_WA_IPC, 0, delay_load);

		return GEN_INIT_SUCCESS;
	}
	return GEN_INIT_FAILURE;
}

void PluginConfig()
{
	HMENU hMenu = WASABI_API_LOADMENUW(IDR_CONTEXTMENU);
	HMENU popup = GetSubMenu(hMenu, 0);

	MENUITEMINFO i = { sizeof(i), MIIM_ID | MIIM_STATE | MIIM_TYPE,
					   MFT_STRING, MFS_UNCHECKED | MFS_DISABLED, 1 };
	i.dwTypeData = (LPWSTR)plugin.description;
	InsertMenuItem(popup, 0, TRUE, &i);

	// as we are re-using the same menu resource, we
	// need to remove the options that are not global
	DeleteMenu(popup, ID_SUBMENU_RERENDER, MF_BYCOMMAND);

	SetupConfigMenu(popup);

	POINT pt = { 0 };
	HWND list = GetPrefsListPos(&pt);
	ProcessMenuResult(TrackPopupMenu(popup, TPM_RETURNCMD, pt.x,
									 pt.y, 0, list, NULL), list);

	DestroyMenu(hMenu);
}

void PluginQuit()
{
	kill_threads = 2;

	ClearProcessingHandles();

	if (no_uninstall)
	{
		DestroyEmbeddedWindow(&embed);
	}

#ifndef _WIN64
	if (FileExists(szTempDLLDestination))
	{
		DeleteFile(szTempDLLDestination);
	}
#endif

	if (clearOnExit)
	{
		ClearCacheFolder(1);
	}

	ServiceRelease(WASABI_API_SVC, WASABI_API_DECODEFILE2, decodeFile2GUID);
#ifndef WACUP_BUILD
	ServiceRelease(WASABI_API_SVC, WASABI_API_LNG, languageApiGUID);
	ServiceRelease(WASABI_API_SVC, WASABI_API_APP, applicationApiServiceGuid);
#endif

	DeleteCriticalSection(&processing_cs);
}

void __cdecl MessageProc(HWND hWnd, const UINT uMsg, const
						 WPARAM wParam, const LPARAM lParam);

winampGeneralPurposePlugin plugin =
{
	GPPHDR_VER_WACUP,
	(char *)L"Waveform Seeker v" TEXT(PLUGIN_VERSION),
	PluginInit, PluginConfig, PluginQuit,
	GEN_INIT_WACUP_HAS_MESSAGES
};

extern "C" __declspec(dllexport) winampGeneralPurposePlugin * winampGetGeneralPurposePlugin()
{
	return &plugin;
}

extern "C" __declspec(dllexport) int winampUninstallPlugin(HINSTANCE hDllInst, HWND hwndDlg, int param)
{
	// prompt to remove our settings with default as no (just incase)
	if (plugin.language->UninstallSettingsPrompt(reinterpret_cast<const wchar_t*>(plugin.description)))
	{
		SaveNativeIniString(WINAMP_INI, L"Waveseek", 0, 0);
		no_uninstall = 0;
	}

	// as we're doing too much in subclasses, etc we cannot allow for on-the-fly removal so need to do a normal reboot
	return GEN_PLUGIN_UNINSTALL_REBOOT;
}

RUNTIME_LEN_HELPER_HANDLER