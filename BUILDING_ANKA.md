# Building Anka

Anka 1.0.0 has two programs, built on Windows with Visual Studio 2022 Build Tools (MSVC
14.44.35207, its CMake and Ninja) from the same build folder: the program without menus (target
`yuzu-cmd`, SDL; the build names it `eden-cli.exe`) and the menu program (target `yuzu`, Qt; the
build names it `eglantine.exe`). The release package names them `anka-cli.exe` and `anka.exe`,
so they never replace 2.0.0's own `eden-cli.exe` and `eglantine.exe`. The general build
documentation of Eden is in `docs/`; this file lists what the Anka release used.

## Third-party libraries

This source tree does not contain all of its dependencies. At configure time CMake's CPM
(`CMakeModules/`, `cpmfile.json`) downloads them from the repositories and versions pinned in
`cpmfile.json`. Source packages are checked against the hashes pinned in `cpmfile.json`; the
four CI prebuilts (ffmpeg, openssl, sdl3, sirit; `"ci": true`) have no pinned hash, CPM checks
them against the `.sha512sum` file published next to each archive. `-DCPM_SOURCE_CACHE=<dir>` keeps
the downloads in one folder. For the release build these were fetched (name: version):

boost 1.92.0, cubeb fa02160712, enet v1.3.18, ffmpeg (prebuilt, crueter-ci
9.0.2-1790726865-946fcce07b), fmt 12.1.0, frozen 61dce5ae18, gamemode ce6fe122f3 (Linux only, not linked on Windows), httplib
v0.46.0, libusb v1.0.29, lz4 ebb370ca83, nlohmann_json v3.12.0, nx_tzdb, openssl (prebuilt,
crueter-ci 4.0.1-1788234794-b64f68a94e: a development snapshot, the library reports
`OpenSSL 4.1.0-dev`), opus a3f0ec02b3, reshade v6.8.0, sdl3 (prebuilt,
crueter-ci 3.4.14-1788231389-147a8ee32d), simpleini v4.25, sirit (prebuilt, v1.0.7),
spirv-headers vulkan-sdk-1.4.341.0, Vulkan-Headers v1.4.345, VulkanMemoryAllocator v3.3.0,
Vulkan-Utility-Libraries v1.4.345, xbyak v7.40.1, zbic 11b08f2712, zlib v1.3.2, zstd 33618c8,
and for the menu program QuaZip 2e95c9001b (`quazip-qt6`, built from source, LGPL-2.1 with a
static linking exception).

### Qt (menu program)

The menu program is built against **Qt 6.8.3 for MSVC 2022 x64, shared** (the official Qt
binaries: `qtbase`, `qtcharts` and `qtsvg` 6.8.3 for `win64_msvc2022_64`, from the Qt online
installer or its repository), the same Qt version PokeDinner-Eglantine 2.0.0 ships its DLLs of.
The release `anka.exe` uses those DLLs from the 2.0.0 folder; the release zips carry no Qt. Point
CMake at it with `-DYUZU_USE_BUNDLED_QT=OFF -DQt6_DIR=<Qt>/lib/cmake/Qt6 -DCMAKE_PREFIX_PATH=<Qt>`.
Not the Qt CPM would fetch (`qt6` in `cpmfile.json`, Eden-CI's static Qt 6.11.1): it does not
link with MSVC 14.44 (unresolved `__std_rotate` and similar STL symbols), and it would be Qt
6.11 next to 2.0.0's 6.8.3 DLLs. Switching an existing build folder from one Qt to another:
configure with `-U 'Qt6*' -U 'QT_*'` and delete the `*_autogen` folders, so moc runs again.

The prebuilt packages come from the CI repositories named in `cpmfile.json` (`crueter-ci/...`,
`eden-emulator/sirit`), which build them from the upstream sources of those projects (the commit is the last part of
each version above). `Anka-1.0.0-deps-source.zip` holds the sources of the libraries CPM built
from source; `Anka-1.0.0-prebuilt-source.zip` holds the sources of the four prebuilts: for each, the
upstream source archive of the exact commit and the crueter-ci recipe (build scripts) at the
release tag the package was made from, FFmpeg's patch, and sirit's complete source with its
SPIRV-Headers (its `README_PREBUILT_SOURCES.txt` lists commits, tags, options and hashes). Other
components are in the tree: `externals/` and `src/dynarmic`. Their licenses are in `LICENSES/`
and in their own folders.

FFmpeg (LGPL-2.1-or-later) is linked statically. The prebuilt package is FFmpeg 9.0.2, commit
946fcce07b6dcd0331c8cc609192aeff5e1924f8, with Eden's `0001-steamdeck.patch` applied (it
comments out the use of `VK_KHR_internally_synchronized_queues` in `libavutil/hwcontext_vulkan.c`
and `libavutil/vulkan.c`), configured and built by `tools/configure.sh` / `tools/package.sh` of
`crueter-ci/FFmpeg` at adbaa353e2 (release tag `9.0.2-1790726865-946fcce07b`; the Windows / amd64 /
MSVC branch: static, `--disable-everything` plus the h264 / vp8 / vp9 / opus decoders and hardware
decoding, without `--enable-gpl`). `tools/configure.sh` downloads that patch from the Eden repository
and applies it itself (`curl ... | patch -Np1`); to build offline, apply the patch from the
`-prebuilt-source.zip` to the unpacked source (`patch -Np1 < 0001-steamdeck.patch` in its root) and
remove those two lines from `configure.sh` first, so that it is not applied twice. All of it is in
the `-prebuilt-source.zip` (its `README_PREBUILT_SOURCES.txt` has the steps). To link the program
with a modified FFmpeg, build it the same way, put its `include/` and `lib/` in place of the prebuilt
package's
in the CPM cache (`<CPM_SOURCE_CACHE>/ffmpeg/9.0.2-1790726865-946fcce07b-windows-amd64/`), and
build as below. (Not tried for this release; `-DYUZU_USE_BUNDLED_FFMPEG=OFF` is the build
system's own path for an FFmpeg built from source.)

The shaders need a `glslangValidator` (Khronos glslang; the release used glslang 16.6.0); pass
it with `-DGLSLANGVALIDATOR=`.

## Configure and build

From a Developer shell of Visual Studio 2022 (x64):

```sh
cmake -S . -B build-lto -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -Ob2 -DNDEBUG -Z7" "-DCMAKE_C_FLAGS_RELEASE=-O2 -Ob2 -DNDEBUG -Z7" \
  -DENABLE_LTO=ON -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
  -DQt6_DIR=<Qt 6.8.3>/lib/cmake/Qt6 -DCMAKE_PREFIX_PATH=<Qt 6.8.3> \
  -DENABLE_WERROR=OFF -DENABLE_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF \
  -DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT -DGLSLANGVALIDATOR=<path to glslangValidator.exe>
cmake --build build-lto --target yuzu-cmd yuzu -- -d keeprsp
```

This gives `build-lto/bin/eden-cli.exe` and `build-lto/bin/eglantine.exe` (windeployqt copies
Qt's DLLs next to it, for running it from the build folder): link-time code generation, no
profile. (`-d keeprsp` keeps the menu program's link response file for the PGO link below.)
This build behaves the same as the release programs and is slower (the PGO link made it about
16-22% faster in our measurements).

The logic of the Anka settings tab has unit tests, the preset tests included: configure another
folder without LTO and with `-DYUZU_TESTS=ON`, build `tests` and run `bin/tests.exe "[core]"`.

## Logo

`python dist/anka/anka_logo.py` draws the logo again and writes `dist/anka.ico` (the program
icon, 16-256 px) and `dist/qt_themes/default/icons/256x256/anka.png` (the window and About
image). It needs Pillow (`pip install pillow`). Both files are in the tree, so a build does not
need Python.

## Profile-guided optimization (the release programs)

The release programs are the same objects linked again with MSVC's PGO.
`dist/anka/build/pgo_link.sh` does both links from the target's own link line (run it from Git
Bash inside the Developer shell; give paths relative or as `D:/...`, not `/d/...`: they go to the
MSVC tools unconverted). It links the program without menus (`yuzu-cmd`); with `PGO_TARGET=yuzu`
it links the menu program (`yuzu`) instead, which the release only optimizes (step 3):

1. Instrument: `pgo_link.sh build-lto instrument anka-cli-pgi` links `yuzu-cmd`'s objects again
   with `/LTCG /FASTGENPROFILE:PGD=bin\anka-cli-pgi.pgd` (the same link line otherwise;
   `/GENPROFILE` builds are too slow to play).
2. Train: play Donkey Kong Bananza with the instrumented program and the recommended switches
   (`dist/anka/start-recommended.bat`), in the opening area and an open area; it writes
   `.pgc` files next to the `.pgd`. `pgort140.dll` from the MSVC tools must be next to it.
3. Optimize: `pgo_link.sh build-lto optimize anka-cli-pgo <the .pgd> <folder of .pgc files>`
   runs `pgomgr /merge` for each `.pgc` and links again with `/LTCG /USEPROFILE:PGD=...`.
   The release links the menu program with the same profile:
   `PGO_TARGET=yuzu pgo_link.sh build-lto optimize anka-pgo <the same .pgd> <the same folder>`.
   The linker applies it to the emulator code the two programs share (it reported profile data
   for 99.3% of the menu program's functions); the Qt front end's own code gets none.

The script takes `link.exe` and `pgomgr.exe` from the linker folder in the target's link line and
adds `/CGTHREADS:8`; `PGO_LINK_EXTRA` adds options (for example `/PDBALTPATH:%_PDB%`, so the
program names its `.pdb` without a build path). A profile need not come from the same commit: the
linker applies an earlier training run's profile to the code that still matches it. Which profile
the release programs were linked with is written in `VERSION_ANKA.txt` in the program zip. The
packaging script takes the two PGO-linked programs (`bin/anka-cli-pgo.exe` and
`bin/anka-pgo.exe` in the example above) and names them `anka-cli.exe` and `anka.exe`.

The profile (`.pgd`) is training data from playing a game, not source code, and is not
published. Without it, step 1-3 can be repeated with your own training run; the program is
functionally the same either way.

## Release package

The release zips were made by a packaging script outside this tree. `Anka-1.0.0-win64.zip` holds
the two programs `anka.exe` and `anka-cli.exe` (no Qt DLLs: the menu program uses 2.0.0's), the
launchers from `dist/anka/` (`start-recommended.bat`, `start-reactive-experimental.bat`,
`start-safe.bat`) with `anka-config.ps1` (the small PowerShell script they call with `-File` to
prepare the `.ini` files on the first start), an empty `Games` folder with its note
(`dist/anka/Games/PUT-YOUR-GAMES-HERE.txt`), `README_ANKA.md`, `BENIOKU_ANKA.md`,
`CHANGELOG_ANKA.md`, `BUILDING_ANKA.md`, `LICENSE.txt`, `LICENSES/`,
`THIRD_PARTY_NOTICES_ANKA.txt` (the license texts and notices of the third-party components),
`NOTICE_ANKA.txt`, `VERSION_ANKA.txt` and `dist/nx2_switches.txt` as `nx2_switches_ANKA.txt`
(renamed so that it does not replace 2.0.0's own); no name in it is one of 2.0.0's files.
`Anka-1.0.0-pdb.zip` holds the two programs' debug symbols. `Anka-1.0.0-source.zip` is
`git archive` of the same commit (tag `anka-v1.0.0` of https://github.com/EddizEge/Anka) plus
`GIT-COMMIT`, `GIT-REFSPEC` and `GIT-TAG` (CMake names the build from them without `.git`),
`SOURCE_ANKA.txt`, the NOTICE file and the third-party notices; `Anka-1.0.0-deps-source.zip`
holds the third-party sources CPM fetched (not the prebuilt packages),
`Anka-1.0.0-prebuilt-source.zip` the sources and build recipes of the prebuilt packages, and
`SHA256SUMS.txt` lists the hashes of the zips.
