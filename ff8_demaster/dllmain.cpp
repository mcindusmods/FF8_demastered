// ReSharper disable CppZeroConstantCanBeReplacedWithNullptr
#include "coreHeader.h"
#include <GL/GL.h>
#include "minhook/include/MinHook.h"
#include <chrono>
#include <filesystem>
#include "texture.h"
#include "config.h"
#include "opengl.h"
#include "hext.h"
#include "zzz.h"
#include "opengl.h"


//DO NOT DELETE- it acts as an anchor for EFIGS.dll import
EXPORT void InitTest() { OutputDebug("DEMASTER ENGINE LOADED!\n"); }

#ifndef JAPANESE_PATCH
#define JAPANESE_PATCH 0
#endif

const char* windowTitle = "FINAL FANTASY VIII Remastered - Demaster patch by Maki [1.3]";

//The game language is detected at runtime from whichever game DLL is loaded, so the same ff8_demaster.dll
//works for EFIGS and JP. Releases 1.3.4+ were EFIGS-only builds and logged "IMAGE_BASE at: 0" on JP (#87).
//JAPANESE_PATCH=1 only changes which module is probed first.
static bool DetectGameModule()
{
	struct Candidate { const char* module; GameLanguage language; };
	constexpr Candidate efigs{ "FFVIII_EFIGS", GameLanguage::EFIGS };
	constexpr Candidate japanese{ "FFVIII_JP", GameLanguage::Japanese };
	const Candidate order[2] = { JAPANESE_PATCH ? japanese : efigs, JAPANESE_PATCH ? efigs : japanese };
	for (const Candidate& candidate : order)
	{
		if (const HMODULE module = GetModuleHandleA(candidate.module))
		{
			IMAGE_BASE = reinterpret_cast<DWORD>(module);
			g_gameLanguage = candidate.language;
			OutputDebug("Detected game module %s.dll\n", candidate.module);
			return true;
		}
	}
	return false;
}

//Hooks below are found relative to other addresses (WINDOWTITLE+0x12 and so on). Those offsets were measured on
//FFVIII_EFIGS 1.0.3.0; check the opcode before patching so a different build or language skips the hook instead
//of writing over unrelated code.
static bool ExpectOpcode(const DWORD address, const BYTE opcode, const char* what)
{
	if (address == 0 || *reinterpret_cast<const BYTE*>(address) != opcode)
	{
		OutputDebug("Skipping %s: expected opcode %02X at %08X\n", what, opcode, address);
		return false;
	}
	return true;
}

static bool ExpectCallOrJmp(const DWORD address, const char* what)
{
	if (address == 0)
		return false;
	const BYTE opcode = *reinterpret_cast<const BYTE*>(address);
	if (opcode != 0xE8 && opcode != 0xE9)
	{
		OutputDebug("Skipping %s: expected CALL/JMP at %08X, found %02X\n", what, address, opcode);
		return false;
	}
	return true;
}



DWORD dllmainBackAddr;
__declspec(naked) void AsmReplaceWindowTitle()
{
	__asm
	{
		PUSH windowTitle
		JMP dllmainBackAddr
	}
}

//	SetResolution_102190D0+E3   FF 76 04                       push    dword ptr [esi+4] ; x
//	SetResolution_102190D0+E6   68 D8 89 CA 01                 push    1CA89D8h <-- here, we inject
//	The pushed value differs per build (0x21AE970 in FFVIII_JP.dll 1.0.3.0), so it is read from the game before patching
DWORD setResolutionPushedValue = 0x1CA89D8;
__declspec(naked) void AsmSetResolution()
{
	__asm
	{
		POP EAX //Due to where we hook-in, we need to pop to EAX (it's safe reg), and then re-push, but change the esi
		MOV EAX, SetResolutionX
		MOV [esi+4], EAX
		
		MOV EAX, SetResolutionY
		MOV [esi+0x28], EAX
		
		MOV EAX, SetResolutionWidth
		MOV [esi+0x34], EAX
		
		MOV EAX, SetResolutionHeight
		MOV [esi+0x30], EAX
		
		PUSH dword ptr [esi+4]
		PUSH setResolutionPushedValue
		
		JMP oSetResolution
	}
}

void glfwErrorCallback(const int error, const char* description)
{
	OutputDebug("GLFW error: %s (%d)\n", description, error);
}

inline void* oGlfwErrorCallback;
void * HookGlfwErrorCallback([[maybe_unused]] int error_callback)
{
	return static_cast<void* (*)(void*)>(oGlfwErrorCallback)(glfwErrorCallback);
}

inline void* oGlfwSetWindowHint;
void*  HookGlfwSetWindowHint(int hint, int value)
{
	OutputDebug("GlfwSetWindowHint hooked: hint %d, value %d\n", hint, value);
	int newHint = hint;
	int newValue = value;
	
	if(WINDOW_OVERWRITE)
	{
		if(hint == GLFW_DECORATED)
			newValue = GLFW_TRUE;
		//static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(GLFW_CONTEXT_VERSION_MAJOR, 3);
		//static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(GLFW_CONTEXT_VERSION_MINOR, 3);
		//static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		//static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
		//static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(GLFW_CONTEXT_NO_ERROR, GLFW_TRUE);

		
		// ModPage(0x1156E0E5, 1);
		// *reinterpret_cast<BYTE*>(0x1156E0E5) = 0xEB;
		
	}
	return static_cast<void* (*)(GLenum, GLuint)>(oGlfwSetWindowHint)(newHint, newValue);
}

void GetWindow()
{
	if (!HasAddress(WINDOWTITLE))
		return;
	const DWORD windowTitleAddress = IMAGE_BASE + GetAddress(WINDOWTITLE);
	if (ExpectOpcode(windowTitleAddress, 0x68, "window title (PUSH imm32)"))
		dllmainBackAddr = reinterpret_cast<DWORD>(InjectJMP(windowTitleAddress,
			reinterpret_cast<DWORD>(AsmReplaceWindowTitle), 5));

	if (ExpectOpcode(windowTitleAddress + 0x11, 0xE8, "glfwCreateWindow hook"))
	{
		const unsigned int wndGlfw = windowTitleAddress + 0x12
		+ *reinterpret_cast<DWORD*>(windowTitleAddress + 0x12) + 4;
		MH_CreateHook(reinterpret_cast<LPVOID>(wndGlfw), HookGlfwWindow, &glfwWindowTrampoline);
	}
	if (ExpectOpcode(windowTitleAddress - 0xA3, 0xE8, "glfwInit hook"))
	{
		const unsigned int wndGlfwInit = windowTitleAddress - 0xA2
		 + *reinterpret_cast<DWORD*>(windowTitleAddress - 0xA2) + 4;
		MH_CreateHook(reinterpret_cast<LPVOID>(wndGlfwInit), HookGlfwInit, &glfwInitTrampoline);
	}

	const unsigned int wndGlfwSetInputMode = windowTitleAddress + 0x162; //PUSH GLFW_HIDDEN_CURSOR
	if (ExpectOpcode(wndGlfwSetInputMode, 0x68, "cursor mode patch"))
		InjectDWORD(wndGlfwSetInputMode+1, GLFW_CURSOR_NORMAL);

	const unsigned int wndGlfwSetWindowHint = windowTitleAddress-0x5D;
	if (ExpectCallOrJmp(wndGlfwSetWindowHint - 2, "glfwWindowHint hook"))
		MH_CreateHook(reinterpret_cast<LPVOID>(GetRelativeCall(wndGlfwSetWindowHint, -2)),
			HookGlfwSetWindowHint, &oGlfwSetWindowHint);

	const unsigned int wndGlfwErrorCallback = windowTitleAddress - 0xAB;
	if (ExpectCallOrJmp(wndGlfwErrorCallback, "glfwSetErrorCallback hook"))
		MH_CreateHook(reinterpret_cast<LPVOID>(GetRelativeCall(wndGlfwErrorCallback, 0)),
			HookGlfwErrorCallback, &oGlfwErrorCallback);
}




//force GL_LINEAR with non-fixed UV was a d*k move ... ~Maki
void ApplyFilteringPatch()
{
	InjectDWORD(IMAGE_BASE + GetAddress(FILTERPATCH1) + 1, GL_NEAREST);
	//FILTERPATCH2 is the "push GL_TEXTURE_MIN_FILTER" of that call (see mymemory.h), so this write has always made the
	//call invalid and left the minification filter alone. LINEAR_PATCH_MIN_FILTER=1 sets it to nearest as intended.
	if (LINEAR_PATCH_MIN_FILTER && HasAddress(FILTERPATCH2_VALUE)
		&& *reinterpret_cast<DWORD*>(IMAGE_BASE + GetAddress(FILTERPATCH2_VALUE) + 1) == GL_LINEAR)
		InjectDWORD(IMAGE_BASE + GetAddress(FILTERPATCH2_VALUE) + 1, GL_NEAREST);
	else
		InjectDWORD(IMAGE_BASE + GetAddress(FILTERPATCH2) + 1, GL_NEAREST);
	InjectDWORD(IMAGE_BASE + GetAddress(FILTERPATCH3) + 1, GL_NEAREST);
	InjectDWORD(IMAGE_BASE + GetAddress(FILTERPATCH4) + 1, GL_NEAREST);
}

/* This is some fake-ass shit from porting company (or something I don't understand). The real modes are:
	FF8_MODE_0 = 0,
	FF8_MODE_1,
	FF8_MODE_WORLDMAP,
	FF8_MODE_SWIRL,
	FF8_MODE_4,
	FF8_MODE_5,
	FF8_MODE_MENU,
	FF8_MODE_7,
	FF8_MODE_CARDGAME,
	FF8_MODE_9,
	FF8_MODE_10,
	FF8_MODE_11,
	FF8_MODE_INTRO,
	FF8_MODE_100 = 100,
*/
const char * GetModeName(const int mode)
{
	const char *result; // eax
	currentMode = mode;
	switch ( mode )
	{
		case 0:
			result = "MODE_NONE";
			break;
		case 2:
			result = "MODE_FIELD";
			break;
		case 3:
			result = "MODE_MENU";
			break;
		case 4:
			result = "MODE_MENU2";
			break;
		case 5:
			result = "MODE_WORLDMAP";
			break;
		case 6:
			result = "MODE_CARDGAME";
			break;
		case 7:
			result = "MODE_BATTLE";
			break;
		case 8:
			result = "MODE_BATTLESWIRL";
			break;
		case 9:
			result = "MODE_INTRO";
			break;
		case 10:
			result = "MODE_CD_CHANGE";
			break;
		case 11:
			result = "MODE_CHANGE_TO_MAIN";
			break;
		case 12:
			result = "MODE_MAIN";
			break;
		case 13:
			result = "MODE_TITLE";
			break;
		default:
			result = "MODE_UNKNOWN";
			break;
	}
	currentModeStr = result;
	OutputDebug("Mode changed to %s\n", result);
	return result;
}


void __cdecl MoreDebugLog(DWORD* a1, const char* pFormat, ...)
{
	va_list args;
	va_start(args, pFormat);
	va_list args_copy;
	va_copy(args_copy, args);
	int length = vsnprintf(nullptr, 0, pFormat, args_copy);
	va_end(args_copy);
	if (length > 0)
	{
		std::vector<char> buffer(length + 1);
		vsnprintf(buffer.data(), buffer.size(), pFormat, args);
		OutputDebug("%d %s", a1, buffer.data());
	}
	va_end(args);
}

void ZzzUnpack()
{
	bool shouldUnpack = true;
	std::string en_exe_dat_path = std::string(DIRECT_IO_EXPORT_DIR) + "\\ff8_en.exe.dat";
	std::string en_lock_file = std::string(DIRECT_IO_EXPORT_DIR) + "\\unpacked.lock";
	if(!std::filesystem::exists(DIRECT_IO_EXPORT_DIR))
	{
		shouldUnpack = true;
		std::filesystem::create_directory(DIRECT_IO_EXPORT_DIR);
	}
	else
	{
		if (std::filesystem::exists(en_exe_dat_path) && std::filesystem::exists(en_lock_file))
			shouldUnpack = false;
	}
	if (!std::filesystem::exists(zzz::zzzMain) || !std::filesystem::exists(zzz::zzzOther))
	{
		OutputDebug("main.zzz and/or other.zzz not found. Aborting unpack...\n");
		shouldUnpack = false;
	}
	if (!shouldUnpack)
		return;
	
	zzz::UnpackZzz(zzz::zzzMain, DIRECT_IO_EXPORT_DIR);
	zzz::UnpackZzz(zzz::zzzOther, DIRECT_IO_EXPORT_DIR);
	
	std::ofstream lockFile(en_lock_file);
    lockFile.close();
}

void HookSetResolution()
{
	
}

BOOL WINAPI DllMain(

	HINSTANCE hinstDLL, // handle to DLL module
	DWORD fdwReason, // reason for calling function
	LPVOID lpReserved) // reserved
{
	if (fdwReason != DLL_PROCESS_ATTACH) //fail if not on app-init. Attaching is not recommended, should be loaded at startup by import
		return 0;

	SetUnhandledExceptionFilter(ExceptionHandler);

	
	

	AllocConsole();
	(void)freopen("CONOUT$", "w", stdout);
	(void)freopen("CON", "r", stdin);
	InitTest();
	ReadConfigFile();
	if (LOG) logFile = decltype(logFile){ fopen("demasterlog.txt", "wb"), fclose };
	
	

	serverInst.WriteLog(std::string("Server initialized"));
	
	if (AUTOUNPACK)
		ZzzUnpack();
	
	const bool bGameModuleFound = DetectGameModule();
	OPENGL_HANDLE = reinterpret_cast<DWORD>(GetModuleHandleA("OPENGL32"));

	OutputDebug("IMAGE_BASE at: %lX; OPENGL at: %lX\n", IMAGE_BASE, OPENGL_HANDLE);
	if (!bGameModuleFound)
	{
		//Patching IMAGE_BASE 0 is what produced "ModPage error: 00000057" and the crash in #87
		OutputDebug("Neither FFVIII_EFIGS.dll nor FFVIII_JP.dll is loaded. Demaster patches are not applied.\n");
		return 1;
	}

	MH_Initialize();
	GetWindow();
	MH_CreateHookApi(L"OPENGL32", "glViewport", HookGlViewport, &oglViewport);

	//uint32_t getModeName = get_relative_call(GetAddress(CHANGEMODE), 0x1FC);
	if (HasAddress(CHANGEMODE) && ExpectCallOrJmp(GetAddressBase(CHANGEMODE)+0x1FC, "mode name hook"))
		ReplaceCallFunction(GetAddressBase(CHANGEMODE)+0x1FC, GetModeName);

	//LET'S GET THE HACKING DONE
	if (DIRECT_IO)
		ApplyDirectIO();
	if (UVPATCH)
		ApplyUVPatch();
	if (TEXTURE_PATCH && DIRECT_IO)
		ReplaceTextureFunction();
	if (DEBUG_PATCH)
		InjectJMP(GetAddressBase(NULLSUB_DEBUG), reinterpret_cast<DWORD>(OutputDebug));
	if (LINEAR_PATCH)
		ApplyFilteringPatch();
	ApplyAnalogMovementPatch(); //ANALOG_MOVEMENT

    hextPatcher.ApplyAll(std::string());

	if (SKIP_SPLASH && HasAddress(SKIP_SLASHSCREEN))
	{
		OutputDebug("Skipping splash screen\n");
		InjectBYTE(GetAddressBase(SKIP_SLASHSCREEN), 0x01);
	}

	//MH_CreateHook(reinterpret_cast<LPVOID>(GetAddressBase(SET_RESOLUTION)),&HookSetResolution, &oSetResolution);
	//Before the HasAddress guard, the missing JP entry threw std::out_of_range here on every JP launch
	//Maki's widescreen test (0107523) was applied unconditionally: it replaces the game's draw area on every call, in every
	//mode, and turned off scissor clipping. Off unless WIDESCREEN_EXPERIMENTAL=1.
	bForceScissors = WIDESCREEN_EXPERIMENTAL != 0;
	if (!WIDESCREEN_EXPERIMENTAL)
		OutputDebug("WIDESCREEN_EXPERIMENTAL=0: game draw area and scissor left as they are\n");
	else if (HasAddress(SET_RESOLUTION) && ExpectOpcode(GetAddressBase(SET_RESOLUTION)+0xE3, 0xFF, "SetResolution hook")
		&& ExpectOpcode(GetAddressBase(SET_RESOLUTION)+0xE6, 0x68, "SetResolution hook"))
	{
		setResolutionPushedValue = *reinterpret_cast<DWORD*>(GetAddressBase(SET_RESOLUTION)+0xE7);
		oSetResolution = reinterpret_cast<DWORD>(InjectJMP(GetAddressBase(SET_RESOLUTION)+0xE6,
		reinterpret_cast<DWORD>(AsmSetResolution), 5));
		HookSetResolution();
	}

	//InjectJMP(GetAddressBase(MORE_DEBUG_LOG), reinterpret_cast<DWORD>(&MoreDebugLog));
	//MH_CreateHook(reinterpret_cast<LPVOID>(GetAddressBase(MORE_DEBUG_LOG)), &MoreDebugLog, reinterpret_cast<LPVOID*>(&oLogFunction));
	MH_EnableHook(MH_ALL_HOOKS);
	return 1; //all success
}
#undef CRASHLOG