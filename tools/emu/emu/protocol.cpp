// protocol.cpp - register the AuthSrv message defs we know from RE.
//
// These are the CLIENT->SERVER sends observed in GcAuthCmd.cpp (each sender
// fills a struct starting with a u16 msgId; the struct layout == wire layout).
// Registering them lets the emulator parse what the client sends. Server->
// client pre-login messages are not fully known yet -- instrument a real login
// (log DispatchStream on the auth conn) to capture their defs, then add them
// here and craft responses so the Coherent login HTML renders.
#include "msgconn.h"

namespace gw2 {

void register_authsrv_protocol(Protocol& p) {
    // id 2, size 6: [u16 id][u32 rate]  - keepalive / heartbeat
    p.register_def({ 2, "AuthKeepAlive", {
        { MP_DWORD, 0, nullptr, "rate" },
    }});

    // id 3, size 3: [u16 id][u8]        - small ack/flag
    p.register_def({ 3, "AuthCmd3", {
        { MP_BYTE, 0, nullptr, "flag" },
    }});

    // id 6, size 6: [u16 id][u32]
    p.register_def({ 6, "AuthCmd6", {
        { MP_DWORD, 0, nullptr, "arg" },
    }});

    // id 11, size 6: [u16 id][u32]      - logout
    p.register_def({ 11, "AuthLogout", {
        { MP_DWORD, 0, nullptr, "arg" },
    }});

    // id 15, size 3: [u16 id][u8]
    p.register_def({ 15, "AuthCmd15", {
        { MP_BYTE, 0, nullptr, "flag" },
    }});

    // id 37, size 0x26 (38): [u16 id][u32][u64][u64][u64][u64]
    p.register_def({ 37, "AuthCmd37", {
        { MP_DWORD, 0, nullptr, "a" },
        { MP_QWORD, 0, nullptr, "b" },
        { MP_QWORD, 0, nullptr, "c" },
        { MP_QWORD, 0, nullptr, "d" },
        { MP_QWORD, 0, nullptr, "e" },
    }});

    // NOTE: the encrypt-setup callback also sends ids 2/3/5/6 during handshake;
    // with RC4 identity + our connect packet those are plaintext here too.
}

} // namespace gw2
