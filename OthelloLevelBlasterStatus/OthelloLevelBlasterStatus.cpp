#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")

static const int DEFAULT_PORT = 17432;

int main(int argc, char* argv[])
{
    bool doStop = false;
    int  port   = DEFAULT_PORT;

    for (int i = 1; i < argc; i++)
    {
        if (_stricmp(argv[i], "--stop") == 0)
            doStop = true;
        else if (_stricmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        fprintf(stderr, "Cannot connect to OthelloLevelBlaster on port %d\n", port);
        closesocket(s);
        WSACleanup();
        return 1;
    }

    const char* cmd = doStop ? "STOP\n" : "STATUS\n";
    send(s, cmd, (int)strlen(cmd), 0);

    char buf[4096];
    int got;
    while ((got = recv(s, buf, (int)sizeof(buf) - 1, 0)) > 0)
    {
        buf[got] = '\0';
        printf("%s", buf);
    }

    closesocket(s);
    WSACleanup();
    return 0;
}
