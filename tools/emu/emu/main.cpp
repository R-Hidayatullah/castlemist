// gw2emu - client-only plaintext MsgConn emulator (AuthSrv login milestone).
//
//   gw2emu [port]        (default 6112)
//
// Flow per accepted client (assumes the RC4-identity-patched client, or any
// peer that speaks plaintext MsgConn):
//   1. send the raw connect packet  [00][16][20 nonce]  -> pushes client to
//      the ENCRYPTED(=plaintext) stream state.
//   2. read the stream; split messages by registered MsgDef; log each.
//   3. answer keepalives; (TODO) craft the pre-login AuthSrv responses that
//      make the Coherent login screen render.
//
// This is a scaffold: it proves the transport + framing and gives a place to
// grow the server-authoritative message handling.
#include "msgconn.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
#endif

namespace gw2 { void register_authsrv_protocol(Protocol&); }
using namespace gw2;

static void hexdump(const uint8_t* p, size_t n, size_t max = 64) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < max; ++i) { s.push_back(H[p[i]>>4]); s.push_back(H[p[i]&0xf]); s.push_back(' '); }
    if (n > max) s += "...";
    printf("      raw[%zu]: %s\n", n, s.c_str());
}

static bool send_all(sock_t s, const std::vector<uint8_t>& b) {
    size_t sent = 0;
    while (sent < b.size()) {
        int r = send(s, (const char*)b.data() + sent, (int)(b.size() - sent), 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

static void serve_client(sock_t cs, const Protocol& proto) {
    printf("[+] client connected\n");

    // Step 1: send the connect packet to drive mode 2 -> 3 (plaintext).
    auto connp = make_connect_packet();
    if (!send_all(cs, connp)) { printf("[!] send connect failed\n"); return; }
    printf("[>] sent connect packet [00 16 <20x00>]\n");

    // Step 2/3: read + split + log the stream.
    std::vector<uint8_t> acc;
    uint8_t buf[4096];
    for (;;) {
        int r = recv(cs, (char*)buf, sizeof(buf), 0);
        if (r <= 0) { printf("[-] client disconnected\n"); break; }
        acc.insert(acc.end(), buf, buf + r);

        // Drain as many complete messages as we can.
        for (;;) {
            if (acc.empty()) break;
            DecodedMsg m; bool need_more = false;
            if (proto.decode_one(acc.data(), acc.size(), m, need_more)) {
                printf("[<] msg id=%u (%s) len=%zu fields=%zu\n",
                       m.id, m.name, m.wire_len, m.fields.size());
                for (const auto& f : m.fields)
                    printf("      %-8s %s\n", f.label && *f.label ? f.label : "-",
                           f.text.c_str());
                acc.erase(acc.begin(), acc.begin() + m.wire_len);
                // TODO: dispatch -> craft server responses (keepalive echo, etc.)
            } else if (need_more) {
                break;                          // wait for more bytes
            } else {
                // Unknown msgId: we can't know its length -> log and resync-stop.
                uint16_t id = acc.size() >= 2 ? (acc[0] | (acc[1] << 8)) : 0;
                printf("[?] unknown msg id=%u  (no def; dumping + clearing buffer)\n", id);
                hexdump(acc.data(), acc.size());
                acc.clear();
                break;
            }
        }
    }
    CLOSESOCK(cs);
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 6112;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("[!] WSAStartup failed\n"); return 1; }
#endif

    Protocol proto;
    register_authsrv_protocol(proto);

    sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { printf("[!] socket() failed\n"); return 1; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0) { printf("[!] bind(127.0.0.1:%d) failed\n", port); return 1; }
    if (listen(ls, 4) != 0) { printf("[!] listen() failed\n"); return 1; }
    printf("[*] gw2emu listening on 127.0.0.1:%d (plaintext MsgConn)\n", port);

    for (;;) {
        sock_t cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) continue;
        serve_client(cs, proto);   // one client at a time (scaffold)
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
