# Documentation

## Working on castlemist

| document | read it when |
| -------- | ------------ |
| [architecture.md](architecture.md) | before adding anything -- which layer owns what, and the one rule (extraction on a worker thread, rendering on the UI thread) that shapes the rest |
| [building.md](building.md) | first build, or when a build fails in a way the compiler will not explain |
| [testing.md](testing.md) | writing a test, or working out why one skipped instead of running |
| [generating-data.md](generating-data.md) | the index, struct template, string keys and Bink runtime that unlock features |

## The formats

[research/](research) is where the reverse-engineering lives -- 30-odd notes on
the dat codec, the texture containers, model geometry and skinning, map terrain
and lighting, the shader cache, the string-table encryption, and the client's
own login and network layers.

Read [research/README.md](research/README.md) for the index. Two rules of thumb
for keeping them useful:

**A format fact belongs next to the code that depends on it.** The comment in
`packfile.cpp` saying pointer width comes from the version word is worth more
than the same sentence in a document nobody opens. The notes hold the
*investigation*: what was tried, what was measured, what turned out to be a
dead end.

**Say why, not just what.** "Opaque materials are alpha-cutout at 0.25 and the
output alpha is a stencil id" is the fact. That treating it as opacity is what
made every model look see-through is the reason anyone will ever need the fact.
