# external/

Every third-party library castlemist consumes lives here, and **nowhere else** --
no vendored copies scattered through `src/`, no system-wide include paths. If a
build error mentions a library, the fix is in this directory.

The contents are **not tracked in git** (see the top of `.gitignore`): they are
hundreds of megabytes of code maintained upstream. Two subdirectories are the
exception, because castlemist owns them rather than merely using them:

| tracked           | why                                                            |
| ----------------- | -------------------------------------------------------------- |
| `bink2-2.7d/`     | the only header version ABI-compatible with GW2's own `bink2w64.dll` |
| `jpegcfg/`        | hand-written libjpeg-turbo build configuration                  |

`cmake/Externals.cmake` turns each entry below into a real CMake target and
fails at configure time, by name, if one is missing.

## Required to build castlemist

| directory                | version | used by            | source                                            |
| ------------------------ | ------- | ------------------ | ------------------------------------------------- |
| `nlohmann-json/`         | 3.x     | `native`           | https://github.com/nlohmann/json                  |
| `sqlite3/`               | 3.x     | `db`, `tools/gw2index` | https://sqlite.org/download.html (amalgamation) |
| `stb-master/`            | master  | `format`, `media`  | https://github.com/nothings/stb                   |
| `dr_libs-master/`        | master  | `media`            | https://github.com/mackron/dr_libs                |
| `libjpeg-turbo-3.2.0/`   | 3.2.0   | `format`           | https://github.com/libjpeg-turbo/libjpeg-turbo    |
| `libwebp-1.6.0/`         | 1.6.0   | `format`           | https://chromium.googlesource.com/webm/libwebp    |
| `glm-1.0.3/`             | 1.0.3   | `sim`              | https://github.com/g-truc/glm                     |
| `bink2-2.7d/`            | 2.7d    | `media`            | tracked here                                      |
| `jpegcfg/`               | --      | `format`           | tracked here                                      |

Only the source trees are needed; none of these are built with their own build
system. castlemist compiles the handful of translation units it actually uses,
which is why `libjpeg-turbo` and `libwebp` need no configure step.

## Used by the research tooling, not by the app

`Granny-3D-SDK-main/` (MODL animation reference), `MinHook_134_*` and
`minhook-*` (the in-process probes under `tools/hook`), `FMOD/` (FSB audio
reference), `EMotionFX3_*`, `PathEngine_SDKBase_06_04/`, `bcdec-main/`,
`half-rocm-*`, `lzo-2.10/`, `tinyxml2-11.0.0/`.

## Not currently referenced

`glad33extcore/`, `glfw-3.4.bin.WIN64/`, `imgui-docking/`, `ImGuizmo-master/`,
`libogg-*`, `libvorbis-*`, `libpng-*`, `miniaudio-*`, `minimp3-master/`,
`mpg123-*`, `zlib-1.3.2/`, `libwebp-1.6.0-windows-x64/`, `bink2/` (the newer
SDK -- see `bink2-2.7d/README.md` for why it is *not* the one to use).

Left in place deliberately: they are the shortlist for features that are
half-researched (OpenGL preview backend, an ImGui tools shell, the vorbis path
in the audio decoder) and re-finding the exact versions is the expensive part.
