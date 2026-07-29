# Bink 2.7d headers

    bink_2_7d.h      Bink 2.7d / Bink 1.300d public API (2017-10-23)
    radtypes_2_7d.h  its type / platform header

Copied verbatim from the Bink 2 SDK (RAD Game Tools). These two are tracked in
git even though the rest of `external/` is not, because they are the *only*
version that works and finding them again is not trivial -- the newer SDK under
`external/bink2/` does **not** match.

They provide declarations only: the `BINK` struct layout, the flags and the
prototypes. No `.lib` is ever linked. `src/media/video_player.cpp` resolves
every entry point with `GetProcAddress` from the runtime DLL that ships inside
Guild Wars 2 itself:

    bink2w64.dll   Bink 2.7p / 1.3009 (2019-09-05)
                   extracted from Gw2.dat, BINARIES 'MZx',
                   baseId 140117 / fileId 1247272

2.7d headers against the 2.7p runtime is ABI-compatible -- same 2.7 minor
series, `sizeof(BINK) == 1744` either way -- and the newer runtime is the one
that decodes the KB2i/KB2j streams GW2 actually ships. Verified end to end
against real dat entries; see the header comment in `video_player.cpp`.
