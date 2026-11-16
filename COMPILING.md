# Compiling ff8_demaster.dll

Two free installs (once), then double-click one file. Nothing here changes your game until step 5.

## 1. Install Git for Windows

1. Download it from https://git-scm.com/download/win ("Click here to download", the 64-bit setup).
2. Run it and click **Next** on every page. The defaults are fine.

The build uses Git to download vcpkg, the tool that fetches the two libraries DEMASTER needs (glfw3 and xxhash).

## 2. Install Build Tools for Visual Studio 2022 (the compiler)

1. Download https://aka.ms/vs/17/release/vs_buildtools.exe (Microsoft's own link) and run it.
2. The Visual Studio Installer opens. On the **Workloads** tab, tick **Desktop development with C++**.
3. Leave the list on the right as it is. It already includes "MSVC v143 - VS 2022 C++ x64/x86 build tools" and a "Windows 11 SDK".
4. Click **Install**. It is about 6-8 GB. Restart Windows if it asks.

Already have Visual Studio 2022 or 2026 (Community or Build Tools) with "Desktop development with C++"? Skip this step. The build script finds it, and with 2026 it uses that version's compiler (v145) automatically.

## 3. Put the source in a short folder

1. **Unblock the zip first.** Right-click the zip, then **Properties**, then tick **Unblock** (bottom right) and click **OK**. Otherwise Windows may refuse to run the build script.
2. **Put the source at `C:\demaster`.** Extract the zip, drag the `DEMASTER_source` folder inside it to `C:\`, and rename it to `demaster`. Afterwards `C:\demaster\ff8_demaster.sln` and `C:\demaster\build_demaster.bat` should exist.
3. **Keep the path short.** Avoid Desktop, Downloads, OneDrive, and any folder with spaces or brackets in its name. The libraries vcpkg compiles do not like long paths or spaces.

## 4. Build

1. Double-click `C:\demaster\build_demaster.bat`.
   - If Windows says "Windows protected your PC", click **More info**, then **Run anyway**.
2. **The first run takes 5-15 minutes.** It downloads vcpkg (a few hundred MB), compiles glfw3 and xxhash, then compiles DEMASTER. Later builds take about a minute.
3. **When it works,** the window says **BUILD SUCCEEDED** and opens the folder with the new `ff8_demaster.dll`. That is `C:\demaster\.build\Debug\ff8_demaster.dll`.
4. **When it fails,** the window says **BUILD FAILED**. Send me `C:\demaster\build_log.txt`; the lines with "error" in them are what I need. This code has not been through Microsoft's compiler before, so a small fix on the first try would not be a surprise.

One DLL works for both the English/European and the Japanese version. There is no separate Japanese build to make.

## 5. Install it in the game

Find the game folder: in Steam, right-click **FINAL FANTASY VIII Remastered**, then **Manage**, then **Browse local files**.

1. **Back up the old DLL.** Rename the `ff8_demaster.dll` that is already there to `ff8_demaster.dll.bak`.
2. **Copy the new DLL in.** Copy the new `ff8_demaster.dll` into the game folder.
3. **Edit `demaster.conf` with Notepad.** For each line below, change it if a line with that name is already there. Otherwise add it at the end. Never leave two lines with the same name: DEMASTER would only read the first one.
   ```
   LOG=1
   DIRECT_IO=1
   WORLD_TEXTURES=1
   BATTLE_HOOK_MONSTER_FIELDS=1
   WORLD_DEBUG=1
   WORLD_VEHICLES=1
   KEEP_HD_RESOLUTION=1
   ```
   The new settings, with explanations, are also in `C:\demaster\demaster.conf`.
4. **Test textures (optional).** Copy the `world` folder from `vehicle_test_textures` into `DEMASTER_EXP\textures\`, and let it merge with what is there.
5. **The Remastered wmset_014 rename (optional).** Add `REMASTER_WMSET014=field.fs\field_hd_new\wmset_vehicle_0`. Then copy Remastered's `wmset_014_0.png` to `DEMASTER_EXP\textures\field.fs\field_hd_new\wmset_vehicle_0.png`.

## 6. Check it

1. Start the game and load a save on the world map.
2. Quit, and open `demasterlog.txt` in the game folder.
3. **Good signs:**
   - `WORLD_VEHICLES: on`
   - lines like `section42\texture0 -> VRAM (832,0)`
   - lines like `page 15: ... -> <file>`
4. **Send me any lines that say:**
   - `missing`
   - `not known`
   - `unexpected code`
   - `not recognised`
   - `(not used)`

## Undo

Delete the new `ff8_demaster.dll` and rename `ff8_demaster.dll.bak` back to `ff8_demaster.dll`.

## Building again after a change

Double-click `build_demaster.bat` again. vcpkg is already set up, so it only recompiles DEMASTER.
