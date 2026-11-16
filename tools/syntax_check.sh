#!/bin/bash
# Checks ff8_demaster's own source files for mistakes a compiler catches (names that don't exist, wrong arguments, wrong
# types) without Visual Studio: MinGW's g++ for 32-bit Windows reads each file and compiles nothing. It is not Microsoft's
# compiler, so passing here doesn't promise a clean build there, but it catches most slips in the files that only build
# on Windows (everything that touches the game's memory or OpenGL).
#   tools/syntax_check.sh                 every file it can check
#   tools/syntax_check.sh opengl.cpp ...  only those (names inside ff8_demaster/)
# Needs: g++-mingw-w64-i686 (Debian/Ubuntu package name). Files that include vcpkg's libraries (glfw3, xxhash) are checked
# only when VCPKG_INCLUDE points at a folder with their headers; files with Microsoft's inline assembly are skipped.
# tools/mingw_check/StackWalker.h stands in for the MSVC-only vendor header of that name.
cd "$(dirname "$0")/../ff8_demaster" || exit 2
SKIP_ASM="dllmain.cpp iopatch.cpp texture.cpp texturepatch_v2_battleCharacter.cpp texturepatch_v2_battleHooks.cpp
texturepatch_v2_battleScenery.cpp texturepatch_v2_fieldBackground.cpp texturepatch_v2_fieldChara.cpp
texturepatch_v2_fieldLayers.cpp texturepatch_v2_worldVehicles.cpp"
NEEDS_VCPKG="opengl.cpp texturepatch_v2_fieldLayers.cpp"
# files from before this continuation that lean on headers Microsoft's standard library includes for them (g++'s doesn't)
SKIP_MSVC_ONLY="hext.cpp zzz.cpp"
files=("$@")
if [ ${#files[@]} -eq 0 ]; then files=(*.cpp); fi
fail=0
for f in "${files[@]}"; do
	if [ $# -eq 0 ] && echo "$SKIP_ASM" | grep -qw "$f"; then echo "skipped (inline assembly) $f"; continue; fi
	if [ $# -eq 0 ] && echo "$SKIP_MSVC_ONLY" | grep -qw "$f"; then echo "skipped (needs Microsoft's headers) $f"; continue; fi
	if [ -z "$VCPKG_INCLUDE" ] && echo "$NEEDS_VCPKG" | grep -qw "$f"; then echo "skipped (needs VCPKG_INCLUDE) $f"; continue; fi
	out=$(i686-w64-mingw32-g++ -std=c++20 -fsyntax-only -w -fpermissive -fms-extensions -D_stdcall=__stdcall -DNOMINMAX \
		-DGLEW_STATIC -D_WIN32_WINNT=0x0601 -DUSE_SERVER=0 -I../tools/mingw_check -Ivendor/include -Ivendor/include/compat/mingw \
		${VCPKG_INCLUDE:+-I"$VCPKG_INCLUDE"} -I. "$f" 2>&1 | grep -E "error" | grep -v "coreHeader.h:[0-9]*:[0-9]*: error: a storage class")
	if [ -n "$out" ]; then echo "== $f"; echo "$out" | head -20; fail=1; else echo "ok $f"; fi
done
exit $fail
