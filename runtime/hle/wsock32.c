// HLE wsock32: WinSock инициализируется, но сокетов нет (сетевой игры в Shrek 2 нет; IpDrv создаёт сокеты
// только для сетевых драйверов и beacon-ов). socket() — WSAENETDOWN, разрешение имён — только localhost.
#include <strings.h>

#include "runtime/heap.h"
#include "runtime/hle/hle.h"

#define WSAENETDOWN 10050u
#define WSAENOTSOCK 10038u
#define WSAHOST_NOT_FOUND 11001u
#define SOCKET_ERROR 0xFFFFFFFFu

static _Thread_local u32 t_wsa_error;

static u32 fail(u32 err)
{
    t_wsa_error = err;
    return SOCKET_ERROR;
}

// WSAStartup(version, WSADATA*): WSADATA = wVersion, wHighVersion, szDescription[257], szSystemStatus[129],
// iMaxSockets, iMaxUdpDg, lpVendorInfo
void hle_wsock32_WSAStartup(CPU *c)
{
    HLE_ENTER("WSAStartup", 2);
    u32 d = ARG(1);
    memset(g_mem + d, 0, 400);
    wr16(g_mem, d, 0x0101);
    wr16(g_mem, d + 2, 0x0202);
    strcpy((char *)g_mem + d + 4, "WinSock 2.0");
    strcpy((char *)g_mem + d + 4 + 257, "Running");
    wr16(g_mem, d + 390, 32767);  // iMaxSockets
    wr16(g_mem, d + 392, 65467);  // iMaxUdpDg
    RET(2, 0);
}

void hle_wsock32_WSAGetLastError(CPU *c) { HLE_ENTER("WSAGetLastError", 0); RET(0, t_wsa_error); }

void hle_wsock32_socket(CPU *c) { HLE_ENTER("socket", 3); RET(3, fail(WSAENETDOWN)); }
void hle_wsock32_accept(CPU *c) { HLE_ENTER("accept", 3); RET(3, fail(WSAENOTSOCK)); }
void hle_wsock32_bind(CPU *c) { HLE_ENTER("bind", 3); RET(3, fail(WSAENOTSOCK)); }
void hle_wsock32_closesocket(CPU *c) { HLE_ENTER("closesocket", 1); RET(1, fail(WSAENOTSOCK)); }
void hle_wsock32_connect(CPU *c) { HLE_ENTER("connect", 3); RET(3, fail(WSAENOTSOCK)); }
void hle_wsock32_getsockname(CPU *c) { HLE_ENTER("getsockname", 3); RET(3, fail(WSAENOTSOCK)); }
void hle_wsock32_getsockopt(CPU *c) { HLE_ENTER("getsockopt", 5); RET(5, fail(WSAENOTSOCK)); }
void hle_wsock32_setsockopt(CPU *c) { HLE_ENTER("setsockopt", 5); RET(5, fail(WSAENOTSOCK)); }
void hle_wsock32_ioctlsocket(CPU *c) { HLE_ENTER("ioctlsocket", 3); RET(3, fail(WSAENOTSOCK)); }
void hle_wsock32_listen(CPU *c) { HLE_ENTER("listen", 2); RET(2, fail(WSAENOTSOCK)); }
void hle_wsock32_recv(CPU *c) { HLE_ENTER("recv", 4); RET(4, fail(WSAENOTSOCK)); }
void hle_wsock32_recvfrom(CPU *c) { HLE_ENTER("recvfrom", 6); RET(6, fail(WSAENOTSOCK)); }
void hle_wsock32_send(CPU *c) { HLE_ENTER("send", 4); RET(4, fail(WSAENOTSOCK)); }
void hle_wsock32_sendto(CPU *c) { HLE_ENTER("sendto", 6); RET(6, fail(WSAENOTSOCK)); }
void hle_wsock32_shutdown(CPU *c) { HLE_ENTER("shutdown", 2); RET(2, fail(WSAENOTSOCK)); }
void hle_wsock32_select(CPU *c) { HLE_ENTER("select", 5); RET(5, fail(WSAENOTSOCK)); }
void hle_wsock32___WSAFDIsSet(CPU *c) { HLE_ENTER("__WSAFDIsSet", 2); RET(2, 0); }

static u32 bswap32(u32 v) { return __builtin_bswap32(v); }
void hle_wsock32_htonl(CPU *c) { HLE_ENTER("htonl", 1); RET(1, bswap32(ARG(0))); }
void hle_wsock32_ntohl(CPU *c) { HLE_ENTER("ntohl", 1); RET(1, bswap32(ARG(0))); }
void hle_wsock32_htons(CPU *c) { HLE_ENTER("htons", 1); RET(1, __builtin_bswap16((u16)ARG(0))); }
void hle_wsock32_ntohs(CPU *c) { HLE_ENTER("ntohs", 1); RET(1, __builtin_bswap16((u16)ARG(0))); }

// inet_addr("a.b.c.d") -> адрес в порядке байт сети; INADDR_NONE при ошибке
void hle_wsock32_inet_addr(CPU *c)
{
    HLE_ENTER("inet_addr", 1);
    unsigned a, b, d, e;
    char tail;
    if (sscanf(hle_cstr(ARG(0)), "%u.%u.%u.%u%c", &a, &b, &d, &e, &tail) != 4 || a > 255 || b > 255 || d > 255 || e > 255)
        RET(1, 0xFFFFFFFFu);
    RET(1, a | b << 8 | d << 16 | e << 24);
}

void hle_wsock32_gethostname(CPU *c)
{
    HLE_ENTER("gethostname", 2);
    if (ARG(1) < 10)
        RET(2, fail(10014));  // WSAEFAULT
    hle_put_a(ARG(0), ARG(1), "localhost");
    RET(2, 0);
}

// gethostbyname: hostent localhost -> 127.0.0.1 в статической гостевой памяти (как у WinSock — на поток)
static u32 g_hostent;

void hle_wsock32_gethostbyname(CPU *c)
{
    HLE_ENTER("gethostbyname", 1);
    const char *name = hle_cstr(ARG(0));
    if (strcasecmp(name, "localhost")) {
        t_wsa_error = WSAHOST_NOT_FOUND;
        RET(1, 0);
    }
    if (!g_hostent) {
        u32 h = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 64);
        // hostent: h_name, h_aliases, h_addrtype (u16), h_length (u16), h_addr_list
        u32 name_p = h + 32, aliases = h + 20, addrs = h + 24, addr = h + 44;
        strcpy((char *)g_mem + name_p, "localhost");
        wr32(g_mem, h, name_p);
        wr32(g_mem, h + 4, aliases);
        wr16(g_mem, h + 8, 2);  // AF_INET
        wr16(g_mem, h + 10, 4);
        wr32(g_mem, h + 12, addrs);
        wr32(g_mem, aliases, 0);
        wr32(g_mem, addrs, addr);
        wr32(g_mem, addrs + 4, 0);
        wr32(g_mem, addr, 0x0100007Fu);
        g_hostent = h;
    }
    RET(1, g_hostent);
}
