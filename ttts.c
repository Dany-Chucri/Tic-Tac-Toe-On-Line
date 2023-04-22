//THIS FILE COORDINATES GAMES AND ENFORCES GAME RULES
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>

// temporary definitons based on Menendez's example code 
#define QUEUE_SIZE 8 //represents a maximum size of requests to attempt to queue for listening before rejecting any further requests
#define BUFSIZE 256
#define HOSTSIZE 100
#define PORTSIZE 10

// temp signal handlers
volatile int active = 1;


void tokenize(char* buf, char** tokens) {
    char* ptr;
    const char delim = 32;
    int tokptr1 = 0; int tokptr2 = 0;
    if(buf != NULL) ptr = buf;
    else return;


    while(*ptr == delim && *(ptr+1) != '\0') {  ////moves it up if it starts with a whitespace
        ptr++;
    }

    while(*ptr != '\0') {
        if(*ptr != '|') { //if(*ptr != delim && *ptr != '|') {
            tokens[tokptr1][tokptr2] = *ptr;
            tokptr2++;
            ptr++;
        }
        else if (*ptr == '|'){
            tokptr1++; tokptr2 = 0;
            ptr++;
        }
    }

    return;

}



void handler(int signum)
{
    active = 0;
}

void install_handlers(void)
{
    struct sigaction act;
    act.sa_handler = handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
}

// temporary method for reading data from a client
void read_data(int sock, struct sockaddr *rem, socklen_t rem_len)
{
    char buf[BUFSIZE + 1], host[HOSTSIZE], port[PORTSIZE];
    int bytes, error;

    error = getnameinfo(rem, rem_len, host, HOSTSIZE, port, PORTSIZE, NI_NUMERICSERV);
    if (error) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(error));
        strcpy(host, "??");
        strcpy(port, "??");
    }

    printf("Connection from %s:%s\n", host, port);

    while (active && (bytes = read(sock, buf, BUFSIZE)) > 0) {
        buf[bytes] = '\0';
        printf("[%s:%s] read %d bytes |%s|\n", host, port, bytes, buf);


//inspect the buffer to see if it contains any of the key words///////// NEW SHIV ADDITION
    char** tokens = malloc(sizeof(char*) * bytes);
    memset(tokens, (char) 0, bytes);
    for (int i = 0; i < bytes; i++) {
        tokens[i] = malloc(sizeof(char) * bytes);
        memset(tokens[i], (char) 0, bytes);
    }

    tokenize(buf, tokens);
    printf("First Token: %s\n", tokens[0]);

    //if (first token is PLAY, MOVE, RSGN, or DRAW) //Make methods for each of these that do their proper function and returns -1 if unsuccessful or invalid
    if(strcmp(tokens[0], "PLAY") == 0) printf("Player Name: %s\n", tokens[2]);

    for (int i = 0; i < bytes; i++) {
        free(tokens[i]);
    }
    free(tokens);

/////////////////////////////////////////////////////////////////////////
    }


    if (bytes == 0) {
        printf("[%s:%s] got EOF\n", host, port);
    } else if (bytes == -1) {
        printf("[%s:%s] terminating: %s\n", host, port, strerror(errno));
    } else {
        printf("[%s:%s] terminating\n", host, port);
    }

    close(sock);
}

// temporary method for setting up server sockets
int open_listener(char *service, int queue_size)
{
    struct addrinfo hint, *info_list, *info;
    int error, sock;

    // initialize hints
    memset(&hint, 0, sizeof(struct addrinfo));
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;

    // obtain information for listening socket
    error = getaddrinfo(NULL, service, &hint, &info_list);
    if (error) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }

    // attempt to create socket
    for (info = info_list; info != NULL; info = info->ai_next) {
        sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        // if we could not create the socket, try the next method
        if (sock == -1) continue;
        // bind socket to requested port
        error = bind(sock, info->ai_addr, info->ai_addrlen);
        if (error) {
            close(sock);
            continue;
        }
        // enable listening for incoming connection requests
        error = listen(sock, QUEUE_SIZE);
        if (error) {
            close(sock);
            continue;
        }
        // if we got this far, we have opened the socket
        break;
    }

    freeaddrinfo(info_list);
    // info will be NULL if no method succeeded
    if (info == NULL) {
        fprintf(stderr, "Could not bind\n");
        return -1;
    }

    return sock;
}

int main(int argc, char** argv)
{
    char* service = argv[1]; //port number "service" is the first argument for ttts.c 
    
    struct sockaddr_storage remote_host;
    socklen_t remote_host_len;

    int listener = open_listener(service, QUEUE_SIZE);
    if (listener < 0) exit(EXIT_FAILURE);

    printf("Listening for incoming connections\n");

    while (active) {
        remote_host_len = sizeof(remote_host);
        int sock = accept(listener, (struct sockaddr *)&remote_host, &remote_host_len);

        if (sock < 0) {
            perror("accept");
            continue;
        }

        read_data(sock, (struct sockaddr *)&remote_host, remote_host_len);
    }

    puts("Shutting down");
    close(listener);
    
    return EXIT_SUCCESS;

}
