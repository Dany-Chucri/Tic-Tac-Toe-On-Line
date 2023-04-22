#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>

// Temporary definitons based on Menendez's example code 
#define QUEUE_SIZE 8 //represents a maximum size of requests to attempt to queue for listening before rejecting any further requests
#define BUFSIZE 1024
#define HOSTSIZE 100
#define PORTSIZE 10

// Message parsing error handlers, valid for an ok field/message
typedef enum {
    VALID, INCMPL, INVLSIZE, NEBYTE, NEBAR, BARPLCMENT, OVERFLOW, LEFTOVER, INVLFORM
} msg_err;

// Message types, INVLTYPE = 0 to help other 9 types match to assignment descriptions for simplicity
typedef enum {
    INVLTYPE, PLAY, WAIT, BEGN, MOVE, MOVD, INVL, RSGN, DRAW, OVER
} msg_type;

// Temp signal handlers
volatile int active = 1;

void handler(int signum)
{
    active = 0;
}

// Set up signal handlers for primary thread
// Return a mask blocking those signals for worker threads
// FIXME should check whether any of these actually succeeded
void install_handlers(sigset_t *mask)
{
    struct sigaction act;
    act.sa_handler = handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    sigemptyset(mask);
    sigaddset(mask, SIGINT);
    sigaddset(mask, SIGTERM);
}

// Data to be sent to worker threads
struct connection_data {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int fd;
};

// Message parser
int tokenize(char* buf, char** tokens) {
    char* ptr;
    const char delim = 32;
    int tokptr1 = 0; int tokptr2 = 0;
    if(buf != NULL) ptr = buf;
    else return EXIT_FAILURE;


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

    return EXIT_SUCCESS;

}

// Checks the type of message receieved given a string
msg_type checkType(char* type) {
    if (strcmp(type, "PLAY") == 0) return PLAY;
    else if (strcmp(type, "WAIT") == 0) return WAIT;
    else if (strcmp(type, "BEGN") == 0) return BEGN;
    else if (strcmp(type, "MOVE") == 0) return MOVE;
    else if (strcmp(type, "MOVD") == 0) return MOVD;
    else if (strcmp(type, "INVL") == 0) return INVL;
    else if (strcmp(type, "RSGN") == 0) return RSGN;
    else if (strcmp(type, "DRAW") == 0) return DRAW;
    else if (strcmp(type, "OVER") == 0) return OVER;
    else return INVLTYPE;
}

// Sets the maximum amount of bars to be parsed based on message type
// Only useful for some types but can work with all valid types
int setMaxBars(msg_type type) {
    if (type == PLAY) return 3;
    else if (type == WAIT) return 2;
    else if (type == BEGN) return 4;
    else if (type == MOVE) return 4;
    else if (type == MOVD) return 5;
    else if (type == INVL) return 3;
    else if (type == RSGN) return 2;
    else if (type == DRAW) return 3;
    else if (type == OVER) return 4;
    else return -1;
}

// Message field error checker
msg_err parsePacket(char* buf, int fd)
{
    char *ptr = buf;

    int typesize = 4;
    char msgtype[5];
    msgtype[4] = '\0';
    msg_type type = INVLTYPE;

    char size[4];// Size ranges from 0-255
    size[3] = '\0';

    int barsRead = 0;

    // First check that a valid message type was sent
    for (int i = 0; i < typesize; i++) {
        if (*ptr != '\0') {
            msgtype[i] = *ptr;
            ptr++;
        }
        else {
            printf("Not enough bytes read\n");
            return INCMPL;
        }
    }

    // Check for valid message type
    type = checkType(msgtype);
    if (type == INVLTYPE) {
        printf("Invalid message type!\n");
        write(fd, "Invalid message type!\n", 23);
        return INVLFORM;
    }

    // Set max bar amount based on message type read
    int maxBars = setMaxBars(type);

    // Checking size field
    if (*ptr != '\0' && *ptr == '|') {
        barsRead++;
        ptr++;
    }
    else if (*ptr == '\0') {
        printf("Not enough bytes read\n");
        return INCMPL;
    }
    else {
        printf("Invalid bar placement!\n");
        write(fd, "Invalid bar placement!\n", 24);
        return BARPLCMENT;
    }

    for (int i = 0; i < 3; i++){
        if (*ptr != '\0') {
            if (*ptr == '0' || *ptr == '1' || *ptr == '2' || *ptr == '3' || *ptr == '4' || *ptr == '5' || *ptr == '6' || *ptr == '7' || *ptr == '8' || *ptr == '9') { // Making sure its a numerical value in the size field
                size[i] = *ptr;
                ptr++;
            }
            else if (*ptr == '|') {
                size[i] = '\0';
                break;
            }
            else {
                printf("Invalid size2!\n");
                write(fd, "Invalid size!\n", 15);
                return INVLSIZE;
            }
        }
        else {
            printf("Not enough bytes read\n");
            return INCMPL;
        }
    }

    if (*ptr != '\0' && *ptr == '|') {
        barsRead++;
        ptr++;
    }
    else if (*ptr == '\0') {
        printf("Not enough bytes read\n");
        return INCMPL;
    }
    else {
        printf("Invalid bar placement1!\n");
        write(fd, "Invalid bar placement1!\n", 24);
        return BARPLCMENT;
    }

    long int numerSize = strtol(size, NULL, 10); // Converts the read size to a useable value
    printf("Give size is read as %ld\n", numerSize);

    // Cases for 0 size given
    if (numerSize == 0 && (type == WAIT || type == RSGN)) return VALID;
    else if (numerSize == 0) {
        printf("Invalid size3!\n");
        write(fd, "Invalid size!\n", 15);
        return INVLSIZE;
    }

    // Now perform checks on final fields
    for (int actualSize = 0; actualSize <= numerSize; actualSize++) {
        if (*ptr != '\0' && *ptr == '|') { // Found a bar in the message
            if (actualSize < numerSize && barsRead == maxBars) { // Check for if we found a bar too early
                printf("Field size mismatch!\n");
                write(fd, "Field size mismatch!\n", 22);
                return NEBYTE;
            }
            barsRead++;
            if (barsRead > maxBars) // Checks if too many bars are present in the message
            {
                printf("Too many bars present!\n");
                write(fd, "Too many bars present!\n", 24);
                return INVLFORM;
            }
            ptr++;
        }
        else if (*ptr != '\0') {
            ptr++;
        }
        else if (*ptr == '\0' && actualSize < numerSize && barsRead == maxBars) {
            printf("Field size mismatch!\n");
            write(fd, "Field size mismatch!\n", 22);
            return NEBYTE;
        }
        else if (*ptr == '\0' && actualSize == numerSize && barsRead < maxBars) {
            printf("Not enough bars!\n");
            write(fd, "Not enough bars!\n", 18);
            return NEBAR;
        }
        else if (*ptr == '\0' && actualSize == numerSize-1 && barsRead == maxBars-1) {
            printf("Missing ending bar!\n");
            write(fd, "Missing ending bar!\n", 21);
            return NEBAR;
        }
        else if (*ptr == '\0' && actualSize < numerSize && barsRead < maxBars) {
            printf("Not enough bytes read\n");
            return INCMPL;
        }
    }

    if (*ptr != '\0') { // Message is longer than indicated
        printf("Field size mismatch!\n");
        write(fd, "Field size mismatch!\n", 22);
        return INVLFORM;
    }

    return VALID;
}

// Temporary method for reading data from a client (threaded approach)
void *read_data(void *arg)
{
    struct connection_data *con = arg;
    char buf[BUFSIZE], host[HOSTSIZE], port[PORTSIZE];
    int bytes, error;

    error = getnameinfo((struct sockaddr *)&con->addr, con->addr_len, host, HOSTSIZE, port, PORTSIZE, NI_NUMERICSERV);
    if (error) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(error));
        strcpy(host, "??");
        strcpy(port, "??");
    }

    printf("Connection from %s:%s\n", host, port);

    //msg_err errStat = VALID;
    while (active && (bytes = read(con->fd, buf, BUFSIZE)) > 0) {
        buf[bytes] = '\0';

        printf("[%s:%s] read %d bytes |%s|\n", host, port, bytes, buf);
        
        // Packet and field error checking
        msg_err errStat = parsePacket(buf, con->fd);
        int bufLoc = 0;
        bufLoc += bytes;

        if (errStat != VALID) { // We have an invalid message
            if (errStat == INCMPL) { // Incomplete message received, must read more from client
                int bytes2;
                while (active && errStat == INCMPL && (bytes2 = read(con->fd, &buf[bufLoc], BUFSIZE)) > 0) {
                    errStat = parsePacket(buf, con->fd);
                    bufLoc += bytes2;
                }
            }
            else {
                printf("Message is malformed! Ending connection now\n");
                write(con->fd, "Message is malformed! Ending connection now\n", 45);
                break;
            }
        }


        // Parse message
        char** tokens = malloc(sizeof(char*) * bytes);
        memset(tokens, (char) 0, bytes);
        for (int i = 0; i < bytes; i++) {
            tokens[i] = malloc(sizeof(char) * bytes);
            memset(tokens[i], (char) 0, bytes);
        }

        int tokerror = tokenize(buf, tokens);
        if (tokerror != 0) {//error has occured tokenizing
            printf("Error occured while tokenizing!\n"); // FIXME more specific error checking
        }
        else {
            printf("First Token: %s\n", tokens[0]);
            //if (first token is PLAY, MOVE, RSGN, or DRAW) // Make methods for each of these that do their proper function and returns -1 if unsuccessful or invalid
            if(strcmp(tokens[0], "PLAY") == 0) printf("Player Name: %s\n", tokens[2]);
        
        }

        for (int i = 0; i < bytes; i++) {
            free(tokens[i]);
        }
        free(tokens);
    }

    if (bytes == 0) {
        printf("[%s:%s] got EOF\n", host, port);
    } else if (bytes == -1) {
        printf("[%s:%s] terminating: %s\n", host, port, strerror(errno));
    } else {
        printf("[%s:%s] terminating\n", host, port);
    }

    close(con->fd);
    free(con);

    return NULL;
}

// Temporary method for setting up server sockets
int open_listener(char *service, int queue_size)
{
    struct addrinfo hint, *info_list, *info;
    int error, sock;

    // Initialize hints
    memset(&hint, 0, sizeof(struct addrinfo));
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;

    // Obtain information for listening socket
    error = getaddrinfo(NULL, service, &hint, &info_list);
    if (error) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }

    // Attempt to create socket
    for (info = info_list; info != NULL; info = info->ai_next) {
        sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);

        // If we could not create the socket, try the next method
        if (sock == -1) continue;

        // Bind socket to requested port
        error = bind(sock, info->ai_addr, info->ai_addrlen);
        if (error) {
            close(sock);
            continue;
        }
        // Enable listening for incoming connection requests
        error = listen(sock, QUEUE_SIZE);
        if (error) {
            close(sock);
            continue;
        }
        // If we got this far, we have opened the socket
        break;
    }

    freeaddrinfo(info_list);
    // Info will be NULL if no method succeeded
    if (info == NULL) {
        fprintf(stderr, "Could not bind\n");
        return -1;
    }

    return sock;
}

int main(int argc, char** argv)
{
    sigset_t mask;
    struct connection_data *con;
    int error;
    pthread_t tid;

    char* service = argv[1]; // Port number "service" is the first argument for ttts.c 

    install_handlers(&mask);

    int listener = open_listener(service, QUEUE_SIZE);
    if (listener < 0) exit(EXIT_FAILURE);

    printf("Listening for incoming connections\n");

    while (active) {
        con = (struct connection_data *)malloc(sizeof(struct connection_data));
        con->addr_len = sizeof(struct sockaddr_storage);

        con->fd = accept(listener, (struct sockaddr *)&con->addr, &con->addr_len);
        if (con->fd < 0) {
            perror("accept");
            free(con);
            // FIXME check for specific error conditions
            continue;
        }

        // Temporarily disable signals
        // (the worker thread will inherit this mask, ensuring that SIGINT is only delivered to this thread)
        error = pthread_sigmask(SIG_BLOCK, &mask, NULL);
        if (error != 0) {
            fprintf(stderr, "sigmask: %s\n", strerror(error));
            exit(EXIT_FAILURE);
        }

        error = pthread_create(&tid, NULL, read_data, con);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            close(con->fd);
            free(con);
            continue;
        }

        // Automatically clean up child threads once they terminate
        pthread_detach(tid);

        // Unblock handled signals
        error = pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
        if (error != 0) {
            fprintf(stderr, "sigmask: %s\n", strerror(error));
            exit(EXIT_FAILURE);
        }
    }

    puts("Shutting down");
    close(listener);

    pthread_exit(NULL);
    
    return EXIT_SUCCESS;

}
