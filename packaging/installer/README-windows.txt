Backbreaker Recompiled for Windows

This folder builds Backbreaker on your PC from your own disc image of Backbreaker for Xbox 360
(the USA/Europe release). It holds no game content: the installer, the runtime the game links
against, and the open-source compiler that builds it.

Needs: Windows 10 (version 1803 or newer) or Windows 11, 64-bit; a CPU with AVX2 (2013 or
newer); a GPU driver with Vulkan 1.3 (any current AMD, NVIDIA or Intel driver); about 5 GB of
free disk space.

Install
  1. Unzip this folder somewhere it can stay, such as C:\Games. The game is built with the
     compiler in it, and updates use it again.
  2. Double-click Backbreaker-Recompiled.exe, and choose your disc image (the .iso file) in the
     window that opens. Dragging the image onto Backbreaker-Recompiled.exe does the same.
     A console window shows the progress: it checks the image, copies the game's files, and
     builds the game. That takes a few minutes; every CPU core is busy meanwhile.
  3. When it says "installed", Backbreaker is in the Start menu, and in Steam's library if Steam
     was closed while it installed.

  Windows may say "Windows protected your PC" (SmartScreen: the programs here aren't signed).
  Click "More info", then "Run anyway".

  From a terminal, the same thing is:  Backbreaker-Recompiled.exe install <your disc image>
  Without arguments in a terminal, Backbreaker-Recompiled.exe lists every command.

Play
  Start Backbreaker from the Start menu, or double-click Backbreaker-Recompiled.exe.
  F11 switches full screen, F12 takes a screenshot, and F1 (or the controller's Guide button)
  opens the list of achievements.

Settings
  %LOCALAPPDATA%\backbreaker-recomp\settings.ini (paste that into Explorer's address bar). The
  game writes it when its window closes, so play once first, and edit it with the game closed.
    resolution_scale=1   the 3D picture's sharpness: 1 is the console's 1280x720, 2 is
                         2560x1440, up to 4. Raise it if the game looks soft on your display;
                         lower it if the frame rate drops.
    fps=60               60, or 30 as on the console
    fullscreen=0         1 starts in full screen (F11 switches while playing)
    show_fps=0           1 shows the frame rate
    skip_intro=0         1 skips the logos at the start

Title Update 2
  Backbreaker got one patch on Xbox Live: more plays, changed catching, kicking and player
  ratings, replay cameras. If you have your own copy of its package (the file
  TU_134U1UQ_0000008000000.00000000000G2), drag it onto Backbreaker-Recompiled.exe once the
  game is installed. The game is built again with the update, which takes about as long as
  installing did. The package is kept in %LOCALAPPDATA%\backbreaker-recomp\update. To go back
  to the disc's version, take it out of that folder and double-click Backbreaker-Recompiled.exe;
  put it back and double-click again to return to the update. Switching takes a second, since
  both stay built. The Start menu's shortcut starts whichever version was built last.

Update
  Unzip the new release over this folder (or beside it, and delete the old one), then start
  the game as usual. It rebuilds only what changed. Your saves and settings stay.

Where things are
  The game:                    %LOCALAPPDATA%\backbreaker-recomp\install
  Saves, settings, logs:       %LOCALAPPDATA%\backbreaker-recomp
  If something goes wrong, the game's log is logs\backbreaker.log there. It has your Windows
  user name in its paths: look it over before posting it anywhere public.

Uninstall
  Delete this folder and %LOCALAPPDATA%\backbreaker-recomp (your saves and settings are in it:
  keep its saves folder if you want them), and the Start menu's Backbreaker shortcut.

Licences
  Backbreaker Recompiled's own code is under the GNU GPL, version 3 or later; the game itself
  belongs to its owners. bundle\MANIFEST.txt lists every part in this folder and its version,
  and bundle\NOTICES.txt has their licences and where their source is.

Built from @COMMIT@.
