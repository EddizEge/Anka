# Building NX2Perf

The release program is `eden-cli` (target `yuzu-cmd`), built on Windows with Visual Studio 2022
Build Tools (MSVC 14.44.35207, its CMake and Ninja). The general build documentation of Eden is in
`docs/`; this file lists what the NX2Perf release used.

## Third-party libraries

This source tree does not contain all of its dependencies. At configure time CMake's CPM
(`CMakeModules/`, `cpmfile.json`) downloads them from the repositories and versions pinned in
`cpmfile.json`. Source packages are checked against the hashes pinned in `cpmfile.json`; the
four CI prebuilts (ffmpeg, openssl, sdl3, sirit; `"ci": true`) have no pinned hash, CPM checks
them against the `.sha512sum` file published next to each archive. `-DCPM_SOURCE_CACHE=<dir>` keeps
the downloads in one folder. For the release build these were fetched (name: version):

boost 1.92.0, cubeb fa02160712, enet v1.3.18, ffmpeg (prebuilt, crueter-ci
9.0.2-1790726865-946fcce07b), fmt 12.1.0, frozen 61dce5ae18, gamemode ce6fe122f3, httplib
v0.46.0, libusb v1.0.29, lz4 ebb370ca83, nlohmann_json v3.12.0, nx_tzdb, openssl (prebuilt,
crueter-ci 4.0.1-1788234794-b64f68a94e: a development snapshot, the library reports
`OpenSSL 4.1.0-dev`), opus a3f0ec02b3, reshade v6.8.0, sdl3 (prebuilt,
crueter-ci 3.4.14-1788231389-147a8ee32d), simpleini v4.25, sirit (prebuilt, v1.0.7),
spirv-headers vulkan-sdk-1.4.341.0, Vulkan-Headers v1.4.345, VulkanMemoryAllocator v3.3.0,
Vulkan-Utility-Libraries v1.4.345, xbyak v7.40.1, zbic 11b08f2712, zlib v1.3.2, zstd 33618c8.

The prebuilt packages come from the CI repositories named in `cpmfile.json` (`crueter-ci/...`,
`eden-emulator/sirit`), which build them from the upstream sources of those projects (the commit is the last part of
each version above). The release's `-deps-source.zip` holds the sources of the libraries CPM built
from source; for the four prebuilts it links their upstream sources
(`THIRD_PARTY_NOTICES_NX2PERF.txt`). Other
components are in the tree: `externals/` and `src/dynarmic`. Their licenses are in `LICENSES/`
and in their own folders.

The shaders need a `glslangValidator` (Khronos glslang; the release used glslang 16.6.0); pass
it with `-DGLSLANGVALIDATOR=`.

## Configure and build

From a Developer shell of Visual Studio 2022 (x64):

```sh
cmake -S . -B build-lto -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -Ob2 -DNDEBUG -Z7" "-DCMAKE_C_FLAGS_RELEASE=-O2 -Ob2 -DNDEBUG -Z7" \
  -DENABLE_LTO=ON -DENABLE_QT=OFF -DENABLE_WERROR=OFF -DENABLE_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF \
  -DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT -DGLSLANGVALIDATOR=<path to glslangValidator.exe>
cmake --build build-lto --target yuzu-cmd
```

This gives `build-lto/bin/eden-cli.exe`: link-time code generation, no profile. It behaves the
same as the release program and is slower (about 16-22% in our measurements).

## Profile-guided optimization (the release program)

The release program is the same objects linked twice more with MSVC's PGO.
`dist/nx2perf/build/pgo_link.sh` does both links from yuzu-cmd's own link line (run it from Git
Bash inside the Developer shell):

1. Instrument: `pgo_link.sh build-lto instrument eden-cli-pgi` links `yuzu-cmd`'s objects again
   with `/LTCG /FASTGENPROFILE:PGD=bin\eden-cli-pgi.pgd` (the same link line otherwise;
   `/GENPROFILE` builds are too slow to play).
2. Train: play Donkey Kong Bananza with the instrumented program and the recommended switches
   (`dist/nx2perf/start-recommended.bat`), in the opening area and an open area; it writes
   `.pgc` files next to the `.pgd`. `pgort140.dll` from the MSVC tools must be next to it.
3. Optimize: `pgo_link.sh build-lto optimize eden-cli-pgo <the .pgd> <folder of .pgc files>`
   runs `pgomgr /merge` for each `.pgc` and links again with `/LTCG /USEPROFILE:PGD=...`.

The script takes `link.exe` and `pgomgr.exe` from the linker folder in yuzu-cmd's link line and
adds `/CGTHREADS:8`; `PGO_LINK_EXTRA` adds options (for example `/PDBALTPATH:%_PDB%`, so the
program names its `.pdb` without a build path). A profile need not come from the same commit: the
linker applies an earlier training run's profile to the code that still matches it. Which profile
the release program was linked with is written in `VERSION_NX2PERF.txt` in the program zip.

The profile (`.pgd`) is training data from playing a game, not source code, and is not
published. Without it, step 1-3 can be repeated with your own training run; the program is
functionally the same either way.

## Release package

The release zips were made by a packaging script outside this tree. The program zip holds the
program, the launchers from `dist/nx2perf/`, the README / BENIOKU / CHANGELOG / BUILDING files,
`LICENSE.txt`, `LICENSES/`, `THIRD_PARTY_NOTICES_NX2PERF.txt` (the license texts and notices of
the third-party components), a NOTICE file and `dist/nx2_switches.txt` (renamed so that it does
not replace 2.0.0's own). The `-source.zip` is `git archive` of the same commit plus the NOTICE
file, the `-deps-source.zip` the third-party sources CPM fetched, and `SHA256SUMS.txt` lists the
hashes of the zips.
