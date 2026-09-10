#include "tjsCommHead.h"
#include "vita_klog.h"

#include <algorithm>
#include <string>
#include <vector>
#include <assert.h>

#include "tjsError.h"
#include "tjsDebug.h"

#include "Application.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "ScriptMgnIntf.h"
#include "tjsError.h"
#include "PluginImpl.h"
#include "SystemIntf.h"

#include "Exception.h"
//#include "Resource.h"
#include "SystemControl.h"
//#include "MouseCursor.h"
#include "SystemImpl.h"
#include "WaveImpl.h"
#include "GraphicsLoadThread.h"
#include "Platform.h"
#include "EventIntf.h"
#include <thread>
#include <dirent.h>
#include "ConfigManager/LocaleConfigManager.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
extern "C" {
#include <libavutil/avstring.h>
}
#include "TVPColor.h"
#include "FontImpl.h"

//#include "resource.h"

//#pragma comment(lib,"dbghelp.lib")
/*
kernel32.lib;user32.lib;gdi32.lib;winspool.lib;comdlg32.lib;advapi32.lib;shell32.lib;ole32.lib;oleaut32.lib;uuid.lib;odbc32.lib;odbccp32.lib;winmm.lib;dsound.lib;version.lib;mpr.lib;shlwapi.lib;vfw32.lib;imm32.lib;zlib_d.lib;jpeg-6bx_d.lib;libpng_d.lib;onig_s_d.lib;freetype250MT_D.lib;tvpgl_ia32.lib;tvpsnd_ia32.lib;%(AdditionalDependencies)
kernel32.lib;user32.lib;gdi32.lib;winspool.lib;comdlg32.lib;advapi32.lib;shell32.lib;ole32.lib;oleaut32.lib;uuid.lib;odbc32.lib;odbccp32.lib;winmm.lib;dsound.lib;version.lib;mpr.lib;shlwapi.lib;vfw32.lib;imm32.lib;zlib.lib;jpeg-6bx.lib;libpng.lib;onig_s.lib;freetype250MT.lib;tvpgl_ia32.lib;tvpsnd_ia32.lib;%(AdditionalDependencies)
*/

tTVPApplication* Application = new tTVPApplication;
std::thread::id TVPMainThreadID;
static tTJSCriticalSection _NoMemCallBackCS;
static void *_reservedMem = malloc(1024 * 1024 * 4); // 4M reserved mem
static bool _project_startup = false;
tTJS *TVPAppScriptEngine;
#if !defined(__vita__)
#define HOOK_MALLOC
#endif

static void _do_compact() {
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
}

static void _no_memory_cb() {
	tTJSCSH lock(_NoMemCallBackCS);
	free(_reservedMem);
	if (TVPMainThreadID == std::this_thread::get_id()) {
		_do_compact();
	} else {
		Application->PostUserMessage(_do_compact);
	}
	_reservedMem = realloc(0, 1024 * 1024 * 4);
}

static std::string _title, _msg, _retry, _cancel;
static tTJSCriticalSection _cs;
typedef void* F_alloc_t(void*, size_t);
static void* __do_alloc_func(F_alloc_t *f, void *p, size_t c) {
	void *ptr = f(p, c);

	if (!ptr) {
		_no_memory_cb();
		ptr = f(p, c);
		if (!ptr) {
			tTJSCSH lock(_cs);
			const char *btns[2] = { _retry.c_str(), _cancel.c_str() };
			while (!ptr && TVPShowSimpleMessageBox(_msg.c_str(), _title.c_str(), 2, btns) == 0) {
				ptr = f(p, c);
			}
			//TVPExitApplication(-1);
		}
	}
	return ptr;
}

ttstr TVPGetErrorDialogTitle() {
	const ttstr &title = Application->GetTitle();
	if (title.IsEmpty()) {
		return TVPGetPackageVersionString() + " Error";
	} else {
		return ttstr(TVPGetPackageVersionString()) + " " + title;
	}
}

#ifdef HOOK_MALLOC
extern "C" {
	void* tc_malloc(size_t size);
	void tc_free(void* ptr);
	void* tc_realloc(void* ptr, size_t size);
	void* tc_calloc(size_t nmemb, size_t size);

	void *__real_malloc(size_t);
	void __real_free(void*);
	void* __real_realloc(void*, size_t);
	void* __real_calloc(size_t nmemb, size_t size);
#ifdef WIN32
	void* tc_malloc(size_t size) { return nullptr; }
	void tc_free(void* ptr) {}
	void* tc_realloc(void* ptr, size_t size){ return nullptr; }
	void* tc_calloc(size_t nmemb, size_t size){ return nullptr; }

	void *__real_malloc(size_t) { return nullptr; }
	void __real_free(void*) { return; }
	void* __real_realloc(void*, size_t) { return nullptr; }
	void* __real_calloc(size_t nmemb, size_t size){ return nullptr; }
#endif

	static void *__func_malloc(void *, size_t c) {
#ifdef TC_MALLOC
		int *ptr;
		if (tc_malloc_startup) {
			ptr = (int *)tc_malloc(c + sizeof(int));
			if (ptr) {
				*ptr++ = 1;
			}
		} else {
			ptr = (int *)__real_malloc(c + sizeof(int));
			if (ptr) {
				*ptr++ = 0;
			}
		}
		return ptr;
#else
		return __real_malloc(c);
#endif
	}

	void *__wrap_malloc(size_t c)
	{
#ifdef HOOK_MALLOC_FOR_OVERRUN
		try {
			return __real_malloc(c);
		}
		catch (...) {
			TVPExitApplication(-1);
		}
#else
		return __do_alloc_func(__func_malloc, nullptr, c);
#endif
	}

	static void *__func_realloc(void *p, size_t c) {
#ifdef TC_MALLOC
		if (!p) return __func_malloc(p, c);
		int *ptr = (int *)p;
		if (ptr[-1]) {
			ptr = (int *)tc_realloc(ptr - 1, c + sizeof(int));
		} else {
			ptr = (int *)__real_realloc(ptr - 1, c + sizeof(int));
		}
		return ptr;
#else
		return __real_realloc(p, c);
#endif
	}

	void *__wrap_realloc(void *p, size_t c) {
#ifdef HOOK_MALLOC_FOR_OVERRUN
		try {
			return __real_realloc(p, c);
		}
		catch (...) {
			TVPExitApplication(-1);
		}
#else
		return __do_alloc_func(__func_realloc, p, c);
#endif
	}

	static void *__func_calloc(void *p, size_t c) {
#ifdef TC_MALLOC
		int *ptr;
		if (tc_malloc_startup) {
			ptr = (int *)tc_calloc(c + sizeof(int), 1);
			if (ptr) {
				*ptr++ = 1;
			}
		} else {
			ptr = (int *)__real_calloc(c + sizeof(int), 1);
			if (ptr) {
				*ptr++ = 0;
			}
		}
		if (ptr) memset(ptr, 0, c);
		return ptr;
#else
		return __real_calloc(c, 1);
#endif
	}

	void *__wrap_calloc(size_t nmemb, size_t size) {
#ifdef HOOK_MALLOC_FOR_OVERRUN
		try {
			return __real_calloc(nmemb, size);
		}
		catch (...) {
			TVPExitApplication(-1);
		}
#else
		size *= nmemb;
		void* p = __do_alloc_func(__func_malloc, nullptr, size);
		if (p) memset(p, 0, size);
		return p;
#endif
	}

	void __wrap_free(void *p) {
#ifdef HOOK_MALLOC_FOR_OVERRUN
		try {
			return __real_free(p);
		}
		catch (...) {
			TVPExitApplication(-1);
		}
#elif defined(TC_MALLOC)
		int *ptr = (int *)p;
		if (ptr[-1] == 0) __real_free(ptr - 1);
		else tc_free(ptr - 1);
#else
		__real_free(p);
#endif
	}
}
#endif

#if 0
#ifdef TJS_64BIT_OS
extern void TVPHandleSEHException( int ErrorCode, EXCEPTION_RECORD *P, unsigned long long osEsp, PCONTEXT ctx);
#else
extern void TVPHandleSEHException( int ErrorCode, EXCEPTION_RECORD *P, unsigned long osEsp, PCONTEXT ctx);
#endif

// 傾僾儕働乕僔儑儞偺奐巒帪偵屇傇
inline void CheckMemoryLeaksStart()
{
#ifdef  _DEBUG
   _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
#endif  // _DEBUG
}

inline void DumpMemoryLeaks()
{
#ifdef  _DEBUG
	int is_leak = _CrtDumpMemoryLeaks();
	assert( !is_leak );
#endif  // _DEBUG
}
#endif

ttstr ExePath() {
	return TVPNativeProjectDir;
}

bool TVPCheckAbout();
bool TVPCheckPrintDataPath();
void TVPOnError();
void TVPLockSoundMixer();
void TVPUnlockSoundMixer();

static bool _warnLowMem = true;
void TVPCheckMemory() {
#if defined(_DEBUG)
	if (_warnLowMem) {
		tjs_int freeMem = TVPGetSystemFreeMemory();
		if (freeMem < 24) {
			char buf[256];
			sprintf(buf, "Insufficient memory (%dMB available)\nYou can diable this notice in global preference.", freeMem);
			const char *btn = "OK";
			TVPShowSimpleMessageBox(buf, "No Memory Warning", 1, &btn);
			_warnLowMem = false;
		}
	}
#endif
}

int TVPShowSimpleMessageBox(const ttstr & text, const ttstr & caption) {
	std::vector<ttstr> normal; normal.emplace_back(LocaleConfigManager::GetInstance()->GetText("msgbox_ok"));
	return TVPShowSimpleMessageBox(text, caption, normal);
}

int TVPShowSimpleMessageBoxYesNo(const ttstr & text, const ttstr & caption) {
	std::vector<ttstr> normal;
	LocaleConfigManager *mgr = LocaleConfigManager::GetInstance();
	normal.emplace_back(mgr->GetText("msgbox_yes"));
	normal.emplace_back(mgr->GetText("msgbox_no"));
	return TVPShowSimpleMessageBox(text, caption, normal);
}

ttstr TVPGetMessageByLocale(const std::string &key) {
	return LocaleConfigManager::GetInstance()->GetText(key);
}

int _argc;
char ** _argv;
#if 0
extern void TVPInitCompatibleNativeFunctions();
extern void TVPLoadMessage();
AcceleratorKeyTable::AcceleratorKeyTable() {
	// 僨僼僅儖僩傪撉傒崬傓
	hAccel_ = ::LoadAccelerators( (HINSTANCE)GetModuleHandle(0), MAKEINTRESOURCE(IDC_TVPWIN32));
}
AcceleratorKeyTable::~AcceleratorKeyTable() {
	std::map<HWND,AcceleratorKey*>::iterator i = keys_.begin();
	for( ; i != keys_.end(); i++ ) {
		delete (i->second);
	}
}
void AcceleratorKeyTable::AddKey( HWND hWnd, WORD id, WORD key, BYTE virt ) {
	std::map<HWND,AcceleratorKey*>::iterator i = keys_.find(hWnd);
	if( i != keys_.end() ) {
		i->second->AddKey(id,key,virt);
	} else {
		AcceleratorKey* acc = new AcceleratorKey();
		acc->AddKey( id, key, virt );
		keys_.insert( std::map<HWND, AcceleratorKey*>::value_type( hWnd, acc ) );
	}
}
void AcceleratorKeyTable::DelKey( HWND hWnd, WORD id ) {
	std::map<HWND,AcceleratorKey*>::iterator i = keys_.find(hWnd);
	if( i != keys_.end() ) {
		i->second->DelKey(id);
	}
}

void AcceleratorKeyTable::DelTable( HWND hWnd ) {
	std::map<HWND,AcceleratorKey*>::iterator i = keys_.find(hWnd);
	if( i != keys_.end() ) {
		delete (i->second);
		keys_.erase(i);
	}
}
AcceleratorKey::AcceleratorKey() : hAccel_(NULL), keys_(NULL), key_count_(0) {
}
AcceleratorKey::~AcceleratorKey() {
	if( hAccel_ != NULL ) ::DestroyAcceleratorTable( hAccel_ );
	delete[] keys_;
}
void AcceleratorKey::AddKey( WORD id, WORD key, BYTE virt ) {
	// 傑偢偼懚嵼偡傞偐僠僃僢僋偡傞
	bool found = false;
	int index = 0;
	for( int i = 0; i < key_count_; i++ ) {
		if( keys_[i].cmd == id ) {
			index = i;
			found = true;
			break;
		}
	}
	if( found ) {
		// 婛偵搊榐偝傟偰偄傞僐儅儞僪側偺偱僉乕忣曬偺峏怴傪峴偆
		if( keys_[index].key == key && keys_[index].fVirt == virt ) {
			// 曄峏偝傟偰偄側偄
			return;
		}
		keys_[index].key = key;
		keys_[index].fVirt = virt;
		HACCEL hAccel = ::CreateAcceleratorTable( keys_, key_count_ );
		if( hAccel_ != NULL ) ::DestroyAcceleratorTable( hAccel_ );
		hAccel_ = hAccel;
	} else {
		ACCEL* table = new ACCEL[key_count_+1];
		for( int i = 0; i < key_count_; i++ ) {
			table[i] = keys_[i];
		}
		table[key_count_].cmd = id;
		table[key_count_].key = key;
		table[key_count_].fVirt = virt;
		key_count_++;
		HACCEL hAccel = ::CreateAcceleratorTable( table, key_count_ );
		if( hAccel_ != NULL ) ::DestroyAcceleratorTable( hAccel_ );
		hAccel_ = hAccel;
		delete[] keys_;
		keys_ = table;
	}

}
void AcceleratorKey::DelKey( WORD id ) {
	// 傑偢偼懚嵼偡傞偐僠僃僢僋偡傞
	bool found = false;
	for( int i = 0; i < key_count_; i++ ) {
		if( keys_[i].cmd == id ) {
			found = true;
			break;
		}
	}
	if( found == false ) return;

	// 懚嵼偟偨応崌嶌傝捈偟
	ACCEL* table = new ACCEL[key_count_-1];
	int dest = 0;
	for( int i = 0; i < key_count_; i++ ) {
		if( keys_[i].cmd != id ) {
			table[dest] = keys_[i];
			dest++;
		}
	}
	key_count_--;
	HACCEL hAccel = ::CreateAcceleratorTable( table, key_count_ );
	if( hAccel_ != NULL ) ::DestroyAcceleratorTable( hAccel_ );
	hAccel_ = hAccel;
	delete[] keys_;
	keys_ = table;
}

int APIENTRY WinMain( _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow ) {
	try {
		CheckMemoryLeaksStart();
		// 僂僅僢僠偱 _crtBreakAlloc 偵僙僢僩偡傞

		// XP 傛傝屻偱巊偊傞API傪摦揑偵撉傒崬傫偱屳姺惈傪庢傞
		TVPInitCompatibleNativeFunctions();

		// 儊僢僙乕僕暥帤楍傪儕僜乕僗偐傜撉崬傒
		TVPLoadMessage();

		_argc = __argc;
		_argv = __argv;

		MouseCursor::Initialize();
		Application = new tTVPApplication();
		Application->StartApplication( __argc, __argv );
	
		// delete application and exit forcely
		// this prevents ugly exception message on exit
		// 傾僾儕働乕僔儑儞傪嶍彍偟嫮惂廔椆偝偣傞丅
		// 偙傟偼廔椆帪偺廥偄椺奜儊僢僙乕僕傪梷巭偡傞
		delete Application;

#ifndef _DEBUG
//		::ExitProcess(TVPTerminateCode); // 偙偙偱廔椆偝偣傞偲儊儌儕儕乕僋昞帵偑峴傢傟側偄
#endif
	} catch (...) {
		return 2;
	}
	return TVPTerminateCode;
}
#endif
tTVPApplication::tTVPApplication() : is_attach_console_(false), tarminate_(false), application_activating_(true)
	 , image_load_thread_(NULL), has_map_report_process_(false)
{
}
tTVPApplication::~tTVPApplication() {
// 	while( windows_list_.size() ) {
// 		std::vector<TTVPWindowForm*>::iterator i = windows_list_.begin();
// 		delete (*i);
// 		// TTVPWindowForm 偺僨僗僩儔僋僞撪偱儕僗僩偐傜嶍彍偝傟傞偼偢
// 	}
// 	windows_list_.clear();
	delete image_load_thread_;
}
#if 0
struct SEHException {
	unsigned int Code;
	_EXCEPTION_POINTERS* ExceptionPointers;
	SEHException( unsigned int code, _EXCEPTION_POINTERS* ep )
		: Code(code), ExceptionPointers(ep)
	{}
};

int TVPWriteHWEDumpFile( EXCEPTION_POINTERS* pExceptionPointers ) {
	BOOL bMiniDumpSuccessful;
	WCHAR szPath[MAX_PATH]; 
	WCHAR szFileName[MAX_PATH]; 
	const wchar_t* szAppName = TVPKirikiri;
	const wchar_t* szVersion = TVPGetVersionString().c_str();

	TVPEnsureDataPathDirectory();
	TJS_strcpy(szPath, TVPNativeDataPath.c_str());

	SYSTEMTIME stLocalTime;
	::GetLocalTime( &stLocalTime );
	StringCchPrintf( szFileName, MAX_PATH, L"%s%s%s-%04d%02d%02d-%02d%02d%02d-%ld-%ld.dmp",
				szPath, szAppName, szVersion,
				stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
				stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond,
				GetCurrentProcessId(), GetCurrentThreadId());
	HANDLE hDumpFile = ::CreateFile(szFileName, GENERIC_READ|GENERIC_WRITE,
				FILE_SHARE_WRITE|FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);

	MINIDUMP_EXCEPTION_INFORMATION ExpParam;
	ExpParam.ThreadId = ::GetCurrentThreadId();
	ExpParam.ExceptionPointers = pExceptionPointers;
	ExpParam.ClientPointers = TRUE;
	bMiniDumpSuccessful = MiniDumpWriteDump( ::GetCurrentProcess(), ::GetCurrentProcessId(), hDumpFile, MiniDumpWithDataSegs, &ExpParam, NULL, NULL);
	return EXCEPTION_EXECUTE_HANDLER;
}
static bool TVPIsHandledHWException = false;
void se_translator_function(unsigned int code, struct _EXCEPTION_POINTERS* ep) {
	if( !TVPIsHandledHWException ) {
		//ShowStackTrace( ep->ContextRecord );
		TVPWriteHWEDumpFile( ep );
#ifdef TJS_64BIT_OS
		TVPHandleSEHException( code, ep->ExceptionRecord, ep->ContextRecord->Rsp, ep->ContextRecord );
#else
		TVPHandleSEHException( code, ep->ExceptionRecord, ep->ContextRecord->Esp, ep->ContextRecord );
#endif
		TVPIsHandledHWException = true;
	}
	throw SEHException(code,ep);
}
const wchar_t* SECodeToMessage( unsigned int code ) {
	switch(code){
	case EXCEPTION_ACCESS_VIOLATION: return TVPExceptionAccessViolation;
	case EXCEPTION_BREAKPOINT: return TVPExceptionBreakpoint;
	case EXCEPTION_DATATYPE_MISALIGNMENT: return TVPExceptionDatatypeMisalignment;
	case EXCEPTION_SINGLE_STEP: return TVPExceptionSingleStep;
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return TVPExceptionArrayBoundsExceeded;
	case EXCEPTION_FLT_DENORMAL_OPERAND: return TVPExceptionFltDenormalOperand;
	case EXCEPTION_FLT_DIVIDE_BY_ZERO: return TVPExceptionFltDivideByZero;
	case EXCEPTION_FLT_INEXACT_RESULT: return TVPExceptionFltInexactResult;
	case EXCEPTION_FLT_INVALID_OPERATION: return TVPExceptionFltInvalidOperation;
	case EXCEPTION_FLT_OVERFLOW: return TVPExceptionFltOverflow;
	case EXCEPTION_FLT_STACK_CHECK: return TVPExceptionFltStackCheck;
	case EXCEPTION_FLT_UNDERFLOW: return TVPExceptionFltUnderflow;
	case EXCEPTION_INT_DIVIDE_BY_ZERO: return TVPExceptionIntDivideByZero;
	case EXCEPTION_INT_OVERFLOW: return TVPExceptionIntOverflow;
	case EXCEPTION_PRIV_INSTRUCTION: return TVPExceptionPrivInstruction;
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return TVPExceptionNoncontinuableException;
	case EXCEPTION_GUARD_PAGE: return TVPExceptionGuardPage;
	case EXCEPTION_ILLEGAL_INSTRUCTION: return TVPExceptionIllegalInstruction;
	case EXCEPTION_IN_PAGE_ERROR: return TVPExceptionInPageError;
	case EXCEPTION_INVALID_DISPOSITION: return TVPExceptionInvalidDisposition;
	case EXCEPTION_INVALID_HANDLE: return TVPExceptionInvalidHandle;
	case EXCEPTION_STACK_OVERFLOW: return TVPExceptionStackOverflow;
	case STATUS_UNWIND_CONSOLIDATE: return TVPExceptionUnwindCconsolidate;
	}
	return L"Unknown";
}
#endif
extern void TVPLoadPluigins(void);
bool tTVPApplication::StartApplication(ttstr path) {
//	_set_se_translator(se_translator_function);

	ArgC = 0;
	ArgV = nullptr;
#if 0
	for( int i = 0; i < argc; i++ ) {
		if(!strcmp(argv[i], "-@processohmlog")) {
			has_map_report_process_ = true;
		}
	}
#endif
	TVPTerminateCode = 0;
	LocaleConfigManager *mgr = LocaleConfigManager::GetInstance();
	_retry = mgr->GetText("retry");
	_cancel = mgr->GetText("cancel");
	_msg = mgr->GetText("err_no_memory");
	_title = mgr->GetText("err_occured");
	TVPNativeProjectDir = path;

	CheckConsole();

	// try starting the program!
	try {
//		if(TVPCheckProcessLog()) return true; // sub-process for processing object hash map log

		tjs_char last_ch = path.GetLastChar();
		if (last_ch != TVPArchiveDelimiter && last_ch != TJS_W('/') && last_ch != TJS_W('\\')) {
			if (TVPCheckExistentLocalFile(path)) {
				path += TVPArchiveDelimiter;
			} else {
				path += TJS_W("/");
			}
		}

		TVPProjectDir = TVPNormalizeStorageName(path);
		TVPSetCurrentDirectory(TVPProjectDir);
		KK4V_Log("[KK4V] StartApplication: project dir set");

		TVPInitScriptEngine();
		KK4V_Log("[KK4V] StartApplication: script engine initialized");
		TVPInitFontNames();
		KK4V_Log("[KK4V] StartApplication: font names initialized");

		// banner
		TVPAddImportantLog( TVPFormatMessage(TVPProgramStartedOn, TVPGetOSName(), TVPGetPlatformName()) );

		// TVPInitializeBaseSystems
		TVPInitializeBaseSystems();
		KK4V_Log("[KK4V] StartApplication: base systems initialized");

		Initialize();
		KK4V_Log("[KK4V] StartApplication: Initialize() done");

		// Auto-mount game directory and any archives (*.xp3, *.kxp) in it
		ttstr appDir = TVPGetAppPath();
		if (!appDir.IsEmpty()) {
			ttstr localAppDir = appDir;
			TVPGetLocalName(localAppDir);
			DIR *dir = opendir(localAppDir.AsNarrowStdString().c_str());
			if (dir) {
				struct dirent *ent;
				std::vector<ttstr> archives;
				while ((ent = readdir(dir)) != nullptr) {
					if (ent->d_name[0] == '.') continue;
					std::string fname = ent->d_name;
					std::string ext;
					size_t dot = fname.find_last_of('.');
					if (dot != std::string::npos) {
						ext = fname.substr(dot);
						for (auto &c : ext) c = tolower((unsigned char)c);
					}
					if (ext == ".xp3" || ext == ".kxp") {
						archives.push_back(appDir + ttstr(fname.c_str()) + TVPArchiveDelimiter);
					}
				}
				closedir(dir);
				// Sort so data.xp3 comes first, then patch.xp3, patch2.xp3, etc.
				std::sort(archives.begin(), archives.end());
				for (const auto &arc : archives) {
					TVPAddImportantLog(TJS_W("(info) Auto-registering archive: ") + arc);
					TVPAddAutoPath(arc);
				}
			}
			TVPAddAutoPath(appDir);
		}
		KK4V_Log("[KK4V] StartApplication: archives auto-mounted");

		if(TVPCheckPrintDataPath()) return true;
		if(TVPExecuteUserConfig()) return true;

		image_load_thread_ = new tTVPAsyncImageLoader();

		TVPLoadPluigins(); // load plugin module *.tpm
		KK4V_Log("[KK4V] StartApplication: plugins loaded");
		TVPSystemInit();
		KK4V_Log("[KK4V] StartApplication: TVPSystemInit() done");

		// TVPSystemInit() is the first point where the log directory
		// (TVPNativeDataPath) is known, so file logging can only be turned on
		// after this call returns (turning it on earlier silently and
		// permanently disables it for the whole run - see tTVPLogStreamHolder::Open).
		// Force it on unconditionally (not just on error) so a silent hang still
		// leaves a trail on disk: <gamefolder>/savedata/krkr.console.log
		TVPStartLogToFile(true);
		KK4V_Log("[KK4V] StartApplication: TVPStartLogToFile done (see savedata/krkr.console.log)");

		if(TVPCheckAbout()) return true; // version information dialog box;

		SetTitle(TVPKirikiri.operator const tjs_char *());
		TVPSystemControl = new tTVPSystemControl();
		// Check digitizer
		CheckDigitizer();
		KK4V_Log("[KK4V] StartApplication: SystemControl + digitizer ready");

		// start image load thread
		TVPAddImportantLog(TJS_W("(kk4v) resuming async image load thread"));
		image_load_thread_->Resume();
		KK4V_Log("[KK4V] StartApplication: image load thread resumed");

		TVPAddImportantLog(TJS_W("(kk4v) running startup script"));
		KK4V_Log("[KK4V] StartApplication: calling TVPInitializeStartupScript()...");
		/*if(TVPProjectDirSelected)*/ TVPInitializeStartupScript();
		KK4V_Log("[KK4V] StartApplication: TVPInitializeStartupScript() returned");
		TVPAddImportantLog(TJS_W("(kk4v) startup script finished"));
		_project_startup = true;
//		Run();
#if 0
		try {
			// image_load_thread_->ExitRequest();
			delete image_load_thread_;
			image_load_thread_ = NULL;
		} catch(...) {
			// ignore errors
		}
		try {
			TVPSystemUninit();
		} catch(...) {
			// ignore errors
		}
#endif
	} catch( const EAbort & ) {
		KK4V_Log("[KK4V] StartApplication: caught EAbort");
		// nothing to do
#if !(defined(_MSC_VER) && defined(_DEBUG))
	} catch (const Exception &exception) {
		KK4V_Log("[KK4V] StartApplication: caught Exception");
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		KK4V_Log("[KK4V] StartApplication: caught eTJSScriptError");
		TVPOnError();
		if (!TVPSystemUninitCalled) {
			ttstr msg;
			if (!title_.IsEmpty()) {
				msg += title_;
				msg += "\n";
			}
			msg += e.GetMessage();
			const tjs_char *pszBlockName = e.GetBlockName();
			if (pszBlockName && *pszBlockName) {
				msg += TJS_W("\n@line(");
				tjs_char tmp[34];
				msg += TJS_int_to_str(e.GetSourceLine(), tmp);
				msg += TJS_W(") ");
				msg += pszBlockName;
			}
			msg += TJS_W("\n");
			msg += e.GetTrace();
			ShowException(msg);
		}
	} catch( const TJS::eTJS &e) {
		KK4V_Log("[KK4V] StartApplication: caught eTJS");
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage() );
	} catch( const std::exception &e ) {
		KK4V_Log("[KK4V] StartApplication: caught std::exception");
		ShowException( e.what() );
	} catch( const char* e ) {
		KK4V_Log("[KK4V] StartApplication: caught const char*");
		ShowException( e );
	} catch( const tjs_char* e ) {
		KK4V_Log("[KK4V] StartApplication: caught const tjs_char*");
		ShowException( e );
#if 0
	} catch( const SEHException& e ) {
		PEXCEPTION_RECORD rec = e.ExceptionPointers->ExceptionRecord;
		std::wstring text(SECodeToMessage(e.Code));
		ttstr result = TJSGetStackTraceString( 10 );
		PrintConsole( result.c_str(), result.length(), true );

		TVPDumpHWException();
		ShowException( text.c_str() );
#endif
	} catch(...) {
		ShowException( (const tjs_char*)TVPUnknownError );
#endif
	}

	return false;
}
/**
 * 僐儞僜乕儖偐傜偺婲摦偐妋擣偟丄僐儞僜乕儖偐傜偺婲摦偺応崌偼丄昗弨弌椡傪妱傝摉偰傞
 */
void tTVPApplication::CheckConsole() {
#ifdef TVP_LOG_TO_COMMANDLINE_CONSOLE
	if( has_map_report_process_ ) return; // 彂偒弌偟梡巕僾儘僙僗偟偰婲摦偝傟偰偄偨帪偼僐儞僜乕儖愙懕偟側偄
	HANDLE hin  = ::GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hout = ::GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE herr = ::GetStdHandle(STD_ERROR_HANDLE);

	DWORD curProcId = ::GetCurrentProcessId();
	DWORD processList[256];
	DWORD count = ::GetConsoleProcessList( processList, 256 );
	bool thisProcHasConsole = false;
	for( DWORD i = 0; i < count; i++ ) {
		if( processList[i] == curProcId ) {
			thisProcHasConsole = true;
			break;
		}
	}
	bool attachedConsole = true;
	if( thisProcHasConsole == false ) {
		attachedConsole = ::AttachConsole(ATTACH_PARENT_PROCESS) != 0;
	}

	if( (hin==0||hout==0||herr==0) && attachedConsole ) {
		wchar_t console[256];
		::GetConsoleTitle( console, 256 );
		console_title_ = std::wstring( console );
		// 尦偺僴儞僪儖傪嵞妱傝摉偰
		if (hin)  ::SetStdHandle(STD_INPUT_HANDLE, hin);
		if (hout) ::SetStdHandle(STD_OUTPUT_HANDLE, hout);
		if (herr) ::SetStdHandle(STD_ERROR_HANDLE, herr);
	}
	is_attach_console_ = attachedConsole;
#endif
}

void tTVPApplication::CloseConsole() {
#if 0
	wchar_t buf[100];
	DWORD len = TJS_snprintf(buf, 100, TVPExitCode, TVPTerminateCode);
	PrintConsole(buf, len);
	if( is_attach_console_ ) {
		::SetConsoleTitle( console_title_.c_str() );
		::FreeConsole();
		is_attach_console_ = false;
	}
#endif
}
void TVPConsoleLog(const ttstr &mes, bool important);
void tTVPApplication::PrintConsole(const ttstr &mes, bool important) {
	TVPConsoleLog(mes, important);
}
#if 0
HWND tTVPApplication::GetHandle() {
	if( windows_list_.size() > 0 ) {
		return windows_list_[0]->GetHandle();
	} else {
		return INVALID_HANDLE_VALUE;
	}
}
void tTVPApplication::Minimize() {
	size_t size = windows_list_.size();
	for( size_t i = 0; i < size; i++ ) {
		if( windows_list_[i]->GetVisible() ) {
			::ShowWindow( windows_list_[i]->GetHandle(), SW_MINIMIZE );
		}
	}
}
void tTVPApplication::Restore() {
	size_t size = windows_list_.size();
	for( size_t i = 0; i < size; i++ ) {
		if( windows_list_[i]->GetVisible() ) {
			::ShowWindow( windows_list_[i]->GetHandle(), SW_RESTORE );
		}
	}
}
void tTVPApplication::BringToFront() {
	size_t size = windows_list_.size();
	for( size_t i = 0; i < size; i++ ) {
		windows_list_[i]->BringToFront();
	}
}
#endif
void tTVPApplication::ShowException(const ttstr& e) {
	TVPShowSimpleMessageBox(e, TVPGetErrorDialogTitle());
	TVPSystemUninit();
	TVPExitApplication(0);
}
void tTVPApplication::Run() {
	try {
		if (TVPTerminated) {
			TVPSystemUninit();
			TVPExitApplication(TVPTerminateCode);
		}
	//	TVPBreathe();
		ProcessMessages();
		if (TVPSystemControl) TVPSystemControl->SystemWatchTimerTimer();
//		TVPDeliverWindowUpdateEvents(); // from SystemWatchTimerTimer
	} catch (const EAbort &) {
		// nothing to do
#if !(defined(_MSC_VER) && defined(_DEBUG))
	} catch (const Exception &exception) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		TVPOnError();
		if (!TVPSystemUninitCalled) {
			ttstr msg;
			if (!title_.IsEmpty()) {
				msg += title_;
				msg += "\n";
			}
			msg += e.GetMessage();
			const tjs_char *pszBlockName = e.GetBlockName();
			if (pszBlockName && *pszBlockName) {
				msg += TJS_W("\n@line(");
				tjs_char tmp[34];
				msg += TJS_int_to_str(e.GetSourceLine(), tmp);
				msg += TJS_W(") ");
				msg += pszBlockName;
			}
			msg += TJS_W("\n");
			msg += e.GetTrace();
			ShowException(msg);
		}
	} catch( const TJS::eTJS &e) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage() );
	} catch( const std::exception &e ) {
		ShowException( e.what() );
	} catch( const char* e ) {
		ShowException( e );
	} catch( const tjs_char* e ) {
		ShowException( e );
	} catch (...) {
		ShowException((const tjs_char*)TVPUnknownError);
#endif
	}
}

void tTVPApplication::ProcessMessages()
{
	std::vector<std::tuple<void*, int, tMsg> > lstUserMsg;
	{
		std::lock_guard<std::mutex> cs(m_msgQueueLock);
		m_lstUserMsg.swap(lstUserMsg);
	}
	for (std::tuple<void*, int, tMsg>& it : lstUserMsg) {
		std::get<2>(it)();
	}
	TVPTimer::ProgressAllTimer();
}

#if 0
bool tTVPApplication::ProcessMessage( MSG &msg ) {
	bool result = false;
	if( ::PeekMessage( &msg, NULL, 0, 0, PM_NOREMOVE) ) {
		BOOL msgExists = ::PeekMessage( &msg, NULL, 0, 0, PM_REMOVE);
		if( msgExists == 0 ) {
			return result;
		}
		result = true;
		if( msg.message != WM_QUIT ) {
			HACCEL hAccelTable = accel_key_.GetHandle(msg.hwnd);
			if( !TranslateAccelerator(msg.hwnd, hAccelTable, &msg) ) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		} else {
			TVPTerminateCode = (int)msg.wParam;
			tarminate_ = true;
		}
	}
	return result;
}
void tTVPApplication::ProcessMessages() {
	MSG msg = {0};
	while(ProcessMessage(msg));
}
void tTVPApplication::HandleMessage() {
	MSG msg = {0};
	if( !ProcessMessage(msg) ) {
		HandleIdle(msg);
	}
}
void tTVPApplication::HandleIdle(MSG &) {
	bool done = true;
	if( TVPSystemControl ) {
		done = TVPSystemControl->ApplicationIdle();
	}
	if( done ) ::WaitMessage();
}
#endif
void tTVPApplication::SetTitle(const ttstr& caption) {
	title_ = caption;
#if 0
	if( windows_list_.size() > 0 ) {
		windows_list_[0]->SetCaption( caption );
	}
	if( is_attach_console_ ) {
		::SetConsoleTitle( caption.c_str() );
	}
#endif
}

void tTVPApplication::Terminate()
{
	//::PostQuitMessage(0);
	tarminate_ = true;
	TVPTerminated = true;
}
#if 0
HWND tTVPApplication::GetMainWindowHandle() const {
	if( windows_list_.size() > 0 ) {
		return windows_list_[0]->GetHandle();
	}
	return INVALID_HANDLE_VALUE;
}

void tTVPApplication::RemoveWindow( TTVPWindowForm* win ) {
	std::vector<class TTVPWindowForm*>::iterator it = std::remove( windows_list_.begin(), windows_list_.end(), win );
	if( it != windows_list_.end() ) {
		windows_list_.erase( it, windows_list_.end() );
	}
}
void tTVPApplication::PostMessageToMainWindow(UINT message, WPARAM wParam, LPARAM lParam) {
	if( windows_list_.size() > 0 ) {
		::PostMessage( windows_list_[0]->GetHandle(), message, wParam, lParam );
	}
}
void tTVPApplication::GetDisableWindowList( std::vector<class TTVPWindowForm*>& win ) {
	size_t count = windows_list_.size();
	for( size_t i = 0; i < count; i++ ) {
		if( windows_list_[i]->GetEnable() == false ) {
			win.push_back( windows_list_[i] );
		}
	}
}
void tTVPApplication::GetEnableWindowList( std::vector<class TTVPWindowForm*>& win, class TTVPWindowForm* activeWindow ) {
	size_t count = windows_list_.size();
	for( size_t i = 0; i < count; i++ ) {
		if( activeWindow != windows_list_[i] && windows_list_[i]->GetEnable() ) {
			win.push_back( windows_list_[i] );
		}
	}
}

void tTVPApplication::DisableWindows() {
	size_t count = windows_list_.size();
	for( size_t i = 0; i < count; i++ ) {
		windows_list_[i]->SetEnable( false );
	}
}
void tTVPApplication::EnableWindows( const std::vector<TTVPWindowForm*>& win ) {
	size_t count = win.size();
	for( size_t i = 0; i < count; i++ ) {
		win[i]->SetEnable( true );
	}
	/*
	size_t count = windows_list_.size();
	for( size_t i = 0; i < count; i++ ) {
		TTVPWindowForm* win = windows_list_[i];
		std::vector<TTVPWindowForm*>::const_iterator f = std::find( ignores.begin(), ignores.end(), win );
		if( f == ignores.end() ) {
			windows_list_[i]->SetEnable( true );
		}
	}
	*/
}
void tTVPApplication::FreeDirectInputDeviceForWindows() {
	size_t count = windows_list_.size();
	for( size_t i = 0; i < count; i++ ) {
		windows_list_[i]->FreeDirectInputDevice();
	}
}

void tTVPApplication::RegisterAcceleratorKey(HWND hWnd, char virt, short key, short cmd) {
	accel_key_.AddKey( hWnd, cmd, key, virt );
}
void tTVPApplication::UnregisterAcceleratorKey(HWND hWnd, short cmd) {
	accel_key_.DelKey( hWnd, cmd );
}
void tTVPApplication::DeleteAcceleratorKeyTable( HWND hWnd ) {
	accel_key_.DelTable( hWnd );
}
#endif
void tTVPApplication::CheckDigitizer() {
	// Windows 7 埲崀偱偺傒桳岠
#if 0
	OSVERSIONINFOEX ovi;
	ovi.dwOSVersionInfoSize = sizeof(ovi);
	::GetVersionEx((OSVERSIONINFO*)&ovi);
	if( ovi.dwPlatformId == VER_PLATFORM_WIN32_NT &&
		ovi.dwMajorVersion >= 6 && ovi.dwMinorVersion >= 1 ) {

		int value = ::GetSystemMetrics(SM_DIGITIZER);
		if( value == 0 ) return;

		TVPAddLog( (const tjs_char*)TVPEnableDigitizer );
		if( value & NID_INTEGRATED_TOUCH ) {
			TVPAddLog( (const tjs_char*)TVPTouchIntegratedTouch );
		}
		if( value & NID_EXTERNAL_TOUCH ) {
			TVPAddLog( (const tjs_char*)TVPTouchExternalTouch );
		}
		if( value & NID_INTEGRATED_PEN ) {
			TVPAddLog( (const tjs_char*)TVPTouchIntegratedPen );
		}
		if( value & NID_EXTERNAL_PEN ) {
			TVPAddLog( (const tjs_char*)TVPTouchExternalPen );
		}
		if( value & NID_MULTI_INPUT ) {
			TVPAddLog( (const tjs_char*)TVPTouchMultiInput );
		}
		if( value & NID_READY ) {
			TVPAddLog( (const tjs_char*)TVPTouchReady );
		}
	}
#endif
}

void tTVPApplication::PostUserMessage(const std::function<void()> &func, void* host, int msg)
{
	std::lock_guard<std::mutex> cs(m_msgQueueLock);
	m_lstUserMsg.emplace_back(host, msg, func);
}

void tTVPApplication::FilterUserMessage(const std::function<void(std::vector<std::tuple<void*, int, tMsg> > &)> &func)
{
	std::lock_guard<std::mutex> cs(m_msgQueueLock);
	func(m_lstUserMsg);
}

void tTVPApplication::OnActivate()
{
	application_activating_ = true;
	if (!_project_startup) return;

//	TVPRestoreFullScreenWindowAtActivation();
	TVPResetVolumeToAllSoundBuffer();
	TVPUnlockSoundMixer();

	// trigger System.onActivate event
	TVPPostApplicationActivateEvent();
	for (auto & it : m_activeEvents) {
		it.second(it.first, eTVPActiveEvent::onActive);
	}
}
void tTVPApplication::OnDeactivate(  )
{
	application_activating_ = false;
	if (!_project_startup) return;

//	TVPMinimizeFullScreenWindowAtInactivation();
	
	// fire compact event
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_DEACTIVATE);

	// set sound volume
	TVPResetVolumeToAllSoundBuffer();
	TVPLockSoundMixer();

	// trigger System.onDeactivate event
	TVPPostApplicationDeactivateEvent();
	for (auto & it : m_activeEvents) {
		it.second(it.first, eTVPActiveEvent::onDeactive);
	}
}

void tTVPApplication::OnExit()
{
	TVPUninitScriptEngine();

	if (TVPSystemControl) delete TVPSystemControl;
	TVPSystemControl = NULL;

	CloseConsole();
}

void tTVPApplication::OnLowMemory()
{
	if (!_project_startup) return;
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
}

bool tTVPApplication::GetNotMinimizing() const
{
	return !application_activating_;
#if 0
	HWND hWnd = GetMainWindowHandle();
	if( hWnd != INVALID_HANDLE_VALUE && hWnd != NULL ) {
		return ::IsIconic( hWnd ) == 0;
	}
	return true; // 儊僀儞偑側偄帪偼嵟彫壔偝傟偰偄傞偲傒側偡
#endif
}
#if 0
void tTVPApplication::OnActiveAnyWindow() {
	if( modal_window_stack_.empty() != true ) {
		tTVPWindow* win = modal_window_stack_.top();
		if( win->GetVisible() && win->GetEnable() ) {
			win->BringToFront();
		}
	}
}
void tTVPApplication::ModalFinished() {
	modal_window_stack_.pop();
	if( modal_window_stack_.empty() != true ) {
		tTVPWindow* win = modal_window_stack_.top();
		if( win->GetVisible() && win->GetEnable() ) {
			win->BringToFront();
		}
	}
}
#endif
void tTVPApplication::LoadImageRequest( class iTJSDispatch2 *owner, class tTJSNI_Bitmap* bmp, const ttstr &name ) {
	if( image_load_thread_ ) {
		image_load_thread_->LoadRequest( owner, bmp, name );
	}
}

void tTVPApplication::RegisterActiveEvent(void *host, const std::function<void(void*, eTVPActiveEvent)>& func/*empty = unregister*/)
{
	if (func)
		m_activeEvents.emplace(host, func);
	else
		m_activeEvents.erase(host);
}

#if 0
std::vector<std::string>* LoadLinesFromFile( const std::wstring& path ) {
	FILE *fp = NULL;
	_wfopen_s( &fp, path.c_str(), L"r");
    if( fp == NULL ) {
		return NULL;
    }
	char buff[1024];
	std::vector<std::string>* ret = new std::vector<std::string>();
    while( fgets(buff, 1024, fp) != NULL ) {
		ret->push_back( std::string(buff) );
    }
    fclose(fp);
	return ret;
}

void TVPRegisterAcceleratorKey(HWND hWnd, char virt, short key, short cmd) {
	if( Application ) Application->RegisterAcceleratorKey( hWnd, virt, key, cmd );
}
void TVPUnregisterAcceleratorKey(HWND hWnd, short cmd) {
	if( Application ) Application->UnregisterAcceleratorKey( hWnd, cmd );
}
void TVPDeleteAcceleratorKeyTable( HWND hWnd ) {
	if( Application ) Application->DeleteAcceleratorKeyTable( hWnd );
}
#endif

void TVPInitWindowOptions() {
	;
}

extern "C" char *TVPLocalDirname(char *path);
std::string ExtractFileDir(const std::string & FileName) {
	return TVPLocalDirname((char*)FileName.c_str());
}

unsigned long ColorToRGB(unsigned int col)
{
	// 0xBBGGRR
	switch (col) {
	case clScrollBar:
		return 0xc8c8c8;
	case clBackground:
		return 0;
	case clActiveCaption:
		return 0xd1b499;
	case clInactiveCaption:
		return 0xdbcdbf;
	case clMenu:
		return 0xf0f0f0;
	case clWindow:
		return 0xffffff;
	case clWindowFrame:
		return 0x646464;
	case clMenuText:
		return 0;
	case clWindowText:
		return 0;
	case clCaptionText:
		return 0;
	case clActiveBorder:
		return 0xb4b4b4;
	case clInactiveBorder:
		return 0xfcf7f4;
	case clAppWorkSpace:
		return 0xababab;
	case clHighlight:
		return 0xff9933;
	case clHighlightText:
		return 0xffffff;
	case clBtnFace:
		return 0xf0f0f0;
	case clBtnShadow:
		return 0xa0a0a0;
	case clGrayText:
		return 0x6d6d6d;
	case clBtnText:
		return 0;
	case clInactiveCaptionText:
		return 0x544e43;
	case clBtnHighlight:
		return 0xffffff;
	case cl3DDkShadow:
		return 0x696969;
	case cl3DLight:
		return 0xe3e3e3;
	case clInfoText:
		return 0;
	case clInfoBk:
		return 0xe1ffff;
	case clUnknown:
		return 0;
	case clHotLight:
		return 0xcc6600;
	case clGradientActiveCaption:
		return 0xead1b9;
	case clGradientInactiveCaption:
		return 0xf2e4d7;
	case clMenuLight:
		return 0xff9933;
	case clMenuBar:
		return 0xf0f0f0;
	default:
		return col & 0xFFFFFF;
	}
}
