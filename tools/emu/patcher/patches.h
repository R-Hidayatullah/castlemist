// patches.h - the patch table for the client-only plaintext emulator.
//
// All VAs are for Gw2-64-disable-aslr.exe (imagebase 0x140000000), the build
// reverse-engineered in the IDB. Verify bytes match before applying; a
// mismatch means a different client build (re-anchor via IDA string xrefs).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gw2pe {

struct BytePatch {
    const char*          name;
    const char*          detail;
    uint64_t             va;
    std::vector<uint8_t> expect;   // current bytes (verified)
    std::vector<uint8_t> replace;  // patched bytes (same length)
    bool                 required; // if false, a verify-mismatch is a warning
};

// ---------------------------------------------------------------------------
// PATCH #2 - RC4 PRGA -> identity copy  (CONCRETE, verified)
//
//   sub_140FEB950 (RC4 encrypt/decrypt). At 0x140feb9ac the keystream is
//   applied:  xor dl, [rbx+rdi-1]   (32 54 3B FF).
//   Changing opcode 0x32 -> 0x8A makes it  mov dl, [rbx+rdi-1], i.e. output =
//   input with the keystream ignored. Both send (state+564) and recv (+300)
//   go through this one function, so the whole MsgConn channel becomes
//   plaintext in both directions.
// ---------------------------------------------------------------------------
inline BytePatch rc4_identity() {
    return {
        "rc4-identity",
        "sub_140FEB950: xor dl,[..] -> mov dl,[..] (plaintext channel)",
        0x140feb9acULL,
        { 0x32, 0x54, 0x3B, 0xFF },
        { 0x8A, 0x54, 0x3B, 0xFF },
        true,
    };
}

// ---------------------------------------------------------------------------
// PATCH #3 - force MsgConn to plaintext mode 1 (ALTERNATIVE to #2)
//
//   sub_140FE6C90 calls sub_140FE5D00(ctx, proto, 2) to init the MsgConn in
//   mode 2 (CLIENT_START). Forcing the mode arg to 1 selects the plaintext
//   stream path in the receive dispatcher (no RC4, no connect-nonce).
//   The immediate that loads the mode arg register is NOT yet byte-located in
//   this table -- disassemble sub_140FE6C90's call site and fill in va/expect/
//   replace. Prefer #2 (rc4_identity) which is already concrete; keep this
//   disabled until verified. Left here as a documented spare.
// ---------------------------------------------------------------------------
// inline BytePatch force_mode1() { ... fill after disasm ... }

// ---------------------------------------------------------------------------
// PATCH #1 - endpoint redirect (BEST-EFFORT string replace)
//
//   Replaces the auth hostname constants with a loopback host so the client
//   dials the emulator. NOTE: the live AuthSrv address is handed to GcSrv from
//   the portal-login response, so string replacement alone may not redirect
//   the real connection -- the robust redirect is patch the GcSrv open call /
//   portal-success handler (patch #4 below) or a hosts-file entry. Applied by
//   name+search in main.cpp rather than a fixed VA.
// ---------------------------------------------------------------------------
struct StringPatch {
    const char* find;     // ASCII needle present in the .exe
    const char* replace;  // replacement (must be <= find length; NUL-padded)
};
inline std::vector<StringPatch> endpoint_strings() {
    return {
        { "ArenaNetworks.com", "127.0.0.1" },
        { "NCPlatform.net",    "127.0.0.1" },
    };
}

// ---------------------------------------------------------------------------
// PATCH #4 - portal -> AuthSrv trigger  (TODO: needs one more RE step)
//
//   The client only opens the AuthSrv MsgConn after the NCPlatform portal
//   login succeeds. To run fully offline we must emit the GcSrv "open"
//   command (feeds sub_14023F920 case 2 / sub_14023FC10) with a loopback
//   address + a dummy 20-byte session key, skipping the portal. The producer
//   of that command struct is the next RE target (xref callers into
//   sub_14023F920 with cmd==2). Until then, reach the emulator by redirecting
//   the real portal/auth endpoint (patch #1 / hosts file) after a real login.
// ---------------------------------------------------------------------------

inline std::vector<BytePatch> byte_patches() {
    return { rc4_identity() };
}

} // namespace gw2pe
