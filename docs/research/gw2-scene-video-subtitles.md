---
name: gw2-scene-video-subtitles
description: "Subtitles are 'chatter lines', not captions — the ScnCliContext path, its per-type duration table, and how Bink movies stream out of the dat"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# Scene subtitles and video

Two separate systems that both show up during a cutscene, and neither is called
what you would search for.

## Subtitles are "chatter lines"

Searching the binary for `subtitle` or `caption` finds almost nothing — one font
style named `Cinecaption` (`0x1422E52A0`) and two unrelated UI string keys. The
system itself is in `Gw2\Game\Scene\Cli\ScnCliContext.cpp` and calls a spoken
line with an on-screen caption a **chatter line**.

`ScnCli_ShowChatterLine` (`0x141476820`) is the entry point. Its two asserts name
the whole interface:

```
params && params->chatterLineType <= Content::CHATTER_LINE_TYPES   (:222)
codedSpeakerName && codedText                                      (:317)
```

`codedSpeakerName` and `codedText` are still **text-coded** at this point — they
are `strs` references, not strings, and get resolved through the `TextDecode_*`
pipeline ([[gw2-strs-decrypt]]). That is why nothing readable shows up in a
memory search for the on-screen line.

`CHATTER_LINE_TYPES` is **201**. The type selects a display duration through
`ScnCli_GetChatterLineDuration` (`0x141475F40`), which returns *two* floats — a
near and a far value — and the caller picks between them by testing whether the
speaking agent is the player's current target. So the same line is held on screen
longer when you are looking at who said it.

**Type 7 is special-cased twice** in the same function: a 30 s repeat-suppression
window instead of 60 s, and a 500 ms re-show delay instead of 5000 ms. Both
constants are inline. I did not pin down what type 7 *is*; the ten-fold shorter
re-show delay suggests something high-frequency like ambient combat barks.

Routing at the end of the function: if the speaking agent exists and answers a
virtual call, the line goes to that agent's overhead bubble; otherwise it is
drawn as a standalone caption.

## Video: Bink 2, streamed out of the dat

`Arena\Engine\Video\Video.cpp` is a thin wrapper over `bink2w64.dll`, which is
**loaded at runtime**, not imported. `Video_Open` (`0x140D7B0A0`) resolves 22
entry points by name from a length-prefixed blob at `0x1425454DA` through the
pointer table at `0x1425452B8`, then calls `BinkOpen`.

`Video_RegisterFrameBuffers` (`0x140D7A830`) asserts:

```
decodeBufferFrameCount <= 2
hasY && hasCr && hasCb
```

— planar YUV, three planes, double buffered. Conversion to RGB happens on the
GPU: `CinVideo_SetYuvShaderConstants` (`0x140A66E10`) uploads the five constants
the blit shader wants (`consta`, `crc`, `cbc`, `adj`, `yscale`).

The part worth knowing: **movies are not read off disk.**
`Arena\Engine\Video\BinkAssetIo.cpp` installs Bink IO callbacks backed by the
asset system, so playback streams through the same dat pipeline as everything
else. `BinkAssetIo_Read` (`0x140D7B750`) carries a long alignment assert relating
`bio->CurBufUsed`, `binkFile->fileIoPos` and `binkFile->fileBuffPos` — Bink's own
buffering sits on top of the asset reader's.

This complements [[gw2-bink-video]], which covers the container and seek cost
from the file side.

## Named functions

`/Arena/Engine/Video` (10), `/Arena/Engine/Cinema` (2), `/Gw2/Game/Scene` (4).

| addr | name |
| --- | --- |
| `0x141476820` | `ScnCli_ShowChatterLine` |
| `0x141476C50` | `ScnCli_ShowChatterLineWithSpeaker` |
| `0x141475F40` | `ScnCli_GetChatterLineDuration` |
| `0x140D7B0A0` | `Video_Open` |
| `0x140D7A830` | `Video_RegisterFrameBuffers` |
| `0x140D7A650` | `Video_DecodeNextFrame` |
| `0x140D7B750` | `BinkAssetIo_Read` |
| `0x140A66E10` | `CinVideo_SetYuvShaderConstants` |

## Not done

- What `chatterLineType` 7 actually is. The constant table that maps types to
  durations lives behind `ScnCli_GetChatterLineDuration` and was not dumped —
  201 entries, and dumping it would answer this outright.
- `Engine\Cinema` proper: `CinScriptFunctions.cpp` (16 funcs, the cutscene
  scripting VM) and `CinSequence.cpp` (7) are enumerated but unnamed. The
  sequence side indexes `m_contentArray` by `trackGroupIndex`, so it is granny
  track groups driving the camera.
- `CinCliLc.cpp` / `CinCliLcActions.cpp` (12 + 11 funcs) — the client-side
  cinematic action dispatch, untouched.

See also: [[gw2-bink-video]], [[gw2-strs-decrypt]], [[gw2-text-pack]].
