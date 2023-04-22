// THIS FILE IMPLEMENTS THE GAME CLIENT
#define _POSIX_C_SOURCE 200809L
//PORTNUM is 3360
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>

int main(int argc, char** argv) {
    //ip = argv[1];
    //printf("%s\n", ip);
    int port = 74920; //int port = 3360;

    int sock;
    struct sockaddr_in addr;
    socklen_t addr_size;
    char buffer[256];
    int n;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket Error\n");
        exit(1);
    }
    printf("Socket Created\n");

    memset(&addr, '\0', sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = port;
    addr.sin_addr.s_addr = inet_addr(argv[1]);
    
    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != -1) printf("Connected to server\n");
    
    return 0;
}
