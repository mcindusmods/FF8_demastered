// stand-in for the MSVC-only StackWalker.h, only for syntax checks with MinGW
#pragma once
#include <windows.h>
class StackWalker
{
public:
	StackWalker() {}
	virtual ~StackWalker() {}
	BOOL ShowCallstack(HANDLE = nullptr, const CONTEXT* = nullptr) { return TRUE; }
protected:
	virtual void OnDbgHelpErr(LPCSTR, DWORD, DWORD64) {}
	virtual void OnOutput(LPCSTR) {}
};
