// gw2patch - apply the client-only-emulator patches to a COPY of the GW2 exe.
//
//   gw2patch <input.exe> <output.exe>
//
// Never patch the live client in place; always work on a copy. This tool:
//   - verifies + applies the concrete byte patches (RC4 -> identity)
//   - best-effort string-replaces the auth endpoint constants -> 127.0.0.1
//   - reports what it did, so mismatches (wrong build) are obvious.
#include "pe_patch.h"
#include "patches.h"
#include <cstdio>
#include <string>

using namespace gw2pe;

static std::vector<uint8_t> str_bytes(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("usage: %s <input.exe> <output.exe>\n", argv[0]);
        printf("  applies the plaintext-emulator patches to a COPY of the client.\n");
        return 2;
    }
    Image img;
    if (!img.load(argv[1])) {
        printf("[!] failed to load '%s'\n", argv[1]);
        return 1;
    }
    printf("[*] loaded %s  (imagebase 0x%llx)\n", argv[1],
           (unsigned long long)img.imagebase());

    int applied = 0, failed = 0;

    // --- concrete byte patches -------------------------------------------
    for (const auto& p : byte_patches()) {
        std::string err;
        if (img.patch_at(p.va, p.expect, p.replace, err)) {
            printf("[+] %-16s @ 0x%llx  ok  (%s)\n", p.name,
                   (unsigned long long)p.va, p.detail);
            ++applied;
        } else {
            printf("[%c] %-16s @ 0x%llx  FAILED: %s\n",
                   p.required ? '!' : '~', p.name,
                   (unsigned long long)p.va, err.c_str());
            if (p.required) ++failed;
        }
    }

    // --- best-effort endpoint string replacement -------------------------
    for (const auto& sp : endpoint_strings()) {
        auto needle = str_bytes(sp.find);
        auto hits = img.find(needle);
        if (hits.empty()) {
            printf("[~] endpoint     '%s' not found (skipped)\n", sp.find);
            continue;
        }
        std::vector<uint8_t> rep(needle.size(), 0);          // NUL-pad to keep length
        size_t rl = strlen(sp.replace);
        if (rl > needle.size()) { printf("[!] replacement too long for '%s'\n", sp.find); continue; }
        memcpy(rep.data(), sp.replace, rl);
        for (size_t off : hits) img.patch_file_off(off, rep);
        printf("[+] endpoint     '%s' -> '%s'  (%zu site(s))\n", sp.find, sp.replace, hits.size());
        ++applied;
    }

    if (failed) {
        printf("[!] %d required patch(es) failed - NOT writing output.\n", failed);
        return 1;
    }
    if (!img.save(argv[2])) {
        printf("[!] failed to write '%s'\n", argv[2]);
        return 1;
    }
    printf("[*] wrote %s  (%d patch group(s) applied)\n", argv[2], applied);
    printf("[i] reminder: portal->AuthSrv trigger (patch #4) still TODO; see patches.h\n");
    return 0;
}
