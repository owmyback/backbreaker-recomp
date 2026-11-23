Backbreaker's installer: it builds the game on your machine from your own disc image of Backbreaker for Xbox 360 (the USA/Europe release). It holds no game content: the programs, the runtime the game links against, and the open-source toolchain that builds it.

**Needs:** an x86-64 CPU with AVX2 (2013 or newer) and a Vulkan 1.3 driver. On Linux, glibc 2.35 or newer: the Steam Deck and any current distribution. On Windows, Windows 10 (version 1803 or newer) or 11.

**On Windows:** unzip `Backbreaker-Recompiled-windows-x86_64.zip` somewhere it can stay, and drag your disc image onto `Backbreaker-Recompiled.exe` in it. It builds the game in a few minutes and puts Backbreaker in the Start menu. Windows may warn that the program isn't signed: "More info", then "Run anyway". Settings (the picture's sharpness, `resolution_scale`, and more) are in `%LOCALAPPDATA%\backbreaker-recomp\settings.ini`. The folder's `README.txt` has the rest.

**With RecompLauncher:** add this repository's link in RecompLauncher. It downloads the installer for your system (`Backbreaker-Recompiled-x86_64.AppImage` on Linux, `Backbreaker-Recompiled-windows-x86_64.zip` on Windows), asks for your disc image, builds the game and adds it to Steam.

**From a terminal:**
```
chmod +x Backbreaker-Recompiled-x86_64.AppImage
./Backbreaker-Recompiled-x86_64.AppImage install <your disc image>
```
That takes about 4 minutes on a Steam Deck. The game is then in the applications menu, and in Steam's library if Steam was closed (otherwise `add-to-steam` adds it later). On a Steam Deck, do this in Desktop Mode.

**Updating from an earlier release:** replace the AppImage and run `./Backbreaker-Recompiled-x86_64.AppImage play` once. It rebuilds only what changed.

**Achievements:** F1, or the controller's Guide (PS) button, opens the list of achievements over the game, and the same key or button closes it. On a Steam Deck the Steam button belongs to Steam and there's no F1 key: in Steam, open the game's controller settings and map the back grip R4 to the keyboard key F1. The list has the 26 achievements you can earn offline, worth 1000 G: the three online ones are left out, since they can only be earned online.

Built from @COMMIT@. Backbreaker's code is under the GNU GPL, version 3 or later; the game itself belongs to its owners. `MANIFEST.txt` lists every package and version inside the AppImage; `NOTICES.txt` has their licences and where their source is (`MANIFEST-windows.txt` and `NOTICES-windows.txt` do the same for the Windows zip); `third-party-sources.tar.xz` holds the source of the GPL and LGPL parts no distribution keeps. More in [docs/runtime.md, "Installer tools"](https://github.com/@REPO@/blob/@COMMIT@/docs/runtime.md#installer-tools).
