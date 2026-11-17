# ff8_demastered_2

> **This branch (`mcindus`) is MCINDUS's continuation of DEMASTER** for the Lunatic Pandora mods, on top of Maki's code: world map textures by name, field backgrounds from PuPu/FME layer images, PBR lighting for fields (backgrounds, characters, models), `fieldprep`, a day/night clock, the look dials and moving lights.
> - What each feature does and how to prepare images: [README_field_layers.md](README_field_layers.md), [README_field_pbr.md](README_field_pbr.md)
> - Every setting: [demaster.conf](demaster.conf). What changed in each version: [CHANGELOG.txt](CHANGELOG.txt)
> - Building: [COMPILING.md](COMPILING.md) (double-click `build_demaster.bat`)
> - Working on the code: [CODE_GUIDE.md](CODE_GUIDE.md)

FF8_Demastered_2 was forked from the original Demastered by MakiPL. 
This is an unofficial patch for the game Final Fantasy VIII Remastered by SquareEnix. It works as DLL-dependency and has a custom launcher with many brand new functions. 
Patches make it possible to load new textures, use non-fixed-resolution textures, and modify many other back-end GPU functions.

This fork was vibe-coded by MCINDUS in order to implement the following:
- World Vehicles, effects, maps, and other World elements prior versions of DEMASTER wer unable to replace
- Live No-Swizzle Field Background implementation (no need for swizzled replacements, just use the actual background layers!)
- Fixes to the Japanese version of DEMASTER to include the updated texture modules
- New Lighting Module with Day/Night system, Real Shadows, PBR, and Custom Lighting 

NEW FEATURES (with Low/Medium/High settings):

Implemented BRAND NEW Lighting Module with:
- Day/Night cycle with Sun and Character Follow Lighting
- Manual Light Generation for custom lights
- NEW Shadows for 3D Models
- Normal Smoothing on 3D Models for Lighting Application
- PBR texture capabilities (Normals, Depth, Ambient Occlusion, Roughness, Metallic, and Emissive)

PBR Modules Available:
- Pre-Rendered Field Backgrounds
- Field Models (Characters, Platforms, Enemies, GFs, etc.)
In-Progress PBR:
- Battlefield Textures
- Battlefield Models (Characters, Enemies/Bosses, GFs, etc.)
- World Textures (Environment, Vehicles, Effects)

Future Modules:
- Magic/Particle FX direct replacements (No more dependence on semi-broken Hashed Replacements)
- Triple Triad complete overhaul module with potential Online Play/Matching and Leaderboard. 


> Maki's original README follows.

FF8_Demaster is unofficial patch for Final Fantasy VIII Remastered game by SquareEnix. It works as DLL-dependency and has custom launcher with brand new functions. Patch makes it possible to load new textures, use non-fixed-resolution textures and will aim in tweaking issues

## Usage:
1. Just drop to your folder of Final Fantasy VIII Remastered installation.
Usually:<br/>
_D:\SteamLibrary\steamapps\common\FINAL FANTASY VIII Remastered_<br/>
2. If it's your first run- please open FFVIII_Demaster_Manager.exe to unpacking files

## SteamDeck / Proton:
[https://www.protondb.com/app/1026680#nyIcYa992E](https://www.protondb.com/app/1026680#nyIcYa992E)

Basically run this with:
`export WINEDLLOVERRIDES="dinput8=n,b"; %command%`

## Troubleshooting
### Compiling:
`Error	MSB8020	The build tools for v142 (Platform Toolset = 'v142') cannot be found.`<br/>
  - Open properties (right click _ff8_demaster_ and click properties) and change `Platform Toolset` to something you have installed.

`Error	MSB8036	The Windows SDK version 10.0 was not found.`<br/>
  - Open properties (right click _ff8_demaster_ and click properties) and change `Windows SDK Version` to something you have installed.

`Error	MSB3191	Unable to create directory "D:\FINAL FANTASY VIII REMASTERED\".`<br/>
  - Open properties (right click _ff8_demaster_ and click properties) and change `Output Directory` to your _FINAL FANTASY VIII Remastered_ folder.<br/>
Default steam install: `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VIII Remastered`


![Properties Page in Visual Studio 2017](https://cdn.discordapp.com/attachments/622172784667394052/628892957327753216/unknown.png)

## Third Party Libraries

| Library                                                      | License               |
| ------------------------------------------------------------ |:---------------------:|
| [BX](https://github.com/bkaradzic/bx)                        | BSD 2-Clause          |
| [BIMG](https://github.com/bkaradzic/bimg)                    | BSD 2-Clause          |
| [GLEW](https://github.com/nigels-com/glew)                   | Khronos License (MIT) |
| [inih](https://github.com/jtilly/inih)                       | New BSD               |
| [StackWalker](https://github.com/JochenKalmbach/StackWalker) | BSD-2-Clause          |

## Third party

HEXT based on HEXT format from [HEXT Tools](https://forums.qhimm.com/index.php?topic=13574.0) by Daniel L. P. Burke

## Compilation

1. Install vcpkg
2. Do a `vcpkg integrate install`
3. That's all. VS/Rider should automatically utilize the vcpkg.json file to add ports
