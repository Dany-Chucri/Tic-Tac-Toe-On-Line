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

// Some definitions
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

// Checks if game is going on
volatile int active_game = 0;

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

// Server stucture to keep track of concurrent games (multithreaded approach), simple linked list with "game" nodes
// FIXME implement locks
typedef struct {
    struct game* first; // Reference to first game to maintain linked list structure
    int gameCount; // How many games the server has setup
} server;

// "game" node structure that contains a game ID, 2 players in each game, board state, and which player's turn it is
struct game{
    int gameID; // based on gameCount of server
    char playerOne[256]; // Player 1's name
    int fd1; // Player 1's connection socket file descriptor
    char playerTwo[256]; // Player 2 name
    int fd2; // Player 2's connection socket file descriptor
    const char* board; // Tic-Tac-Toe game board
    int turn; // Odd means player 1, even means player 2

    struct game* next; // Reference to the next game (if any) to maintain linked list structure
};

// Sets up game server using server structure
server* createGameServer() {
    server *gameServer = malloc(sizeof(server));
    gameServer->first = NULL;
    gameServer->gameCount = 0;
    return gameServer;
}

// Sets up a game instance using the game struct
void newGame(server* gameServer) {
    struct game* match = malloc(sizeof(struct game));

    // Some struct parameter initializers
    match->gameID = gameServer->gameCount;
    match->board = ".........";
    match->turn = 1;
    match->next = NULL;

    if (gameServer->first == NULL) { // First game in the server
        gameServer->first = match;
        gameServer->gameCount++;
    }
    else { // Iterate through list of games to place the most recent match at the end
        struct game *ptr = gameServer->first;
        while (ptr->next != NULL)
        {
            ptr = ptr->next;
        }
        ptr->next = match;
        gameServer->gameCount++;
    }

    return;
}

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
            tokens[tokptr1][tokptr2] = '\0';
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
    // No need to check for empty buffer, already done by call to read
    char *ptr = buf;

    int typesize = 4;
    char msgtype[5];
    msgtype[4] = '\0';
    msg_type type = INVLTYPE;

    char size[4]; // Size ranges from 0-255 !!!!FIXME MAKE SURE SIZE CANNOT PASS 255
    size[3] = '\0';

    int barsRead = 0;

    // Skip white space before a message (possibly from null terminator overflow?)
    while (*ptr == '\0') ptr++;

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

char* createBoard(char* board) {
    board = "........."; //char* board = malloc(9 * sizeof(char));
    //memset(board, '.', 9); //char 46 is '.'
    return board;
}
void printBoard(char* board) {
    for (int i = 0; i < 9; i++) { //could also just print %s tbh
        printf("%c", board[i]);
    }
}

int check_position(char* board, char* position) {
    int board_index;
    if (strcmp(position, "1,1") == 0) board_index = 0;
    else if (strcmp(position, "1,2") == 0) board_index = 1;
    else if (strcmp(position, "1,3") == 0) board_index = 2;
    else if (strcmp(position, "2,1") == 0) board_index = 3;
    else if (strcmp(position, "2,2") == 0) board_index = 4;
    else if (strcmp(position, "2,3") == 0) board_index = 5;
    else if (strcmp(position, "3,1") == 0) board_index = 6;
    else if (strcmp(position, "3,2") == 0) board_index = 7;
    else if (strcmp(position, "3,3") == 0) board_index = 8;
    else return -1;

    if (board[board_index] == 'X' || board[board_index] == 'O') return -1;
    else return board_index; // board[board_index] == '.';
}

int make_move(char* board, char* position, char* role) {
    int board_index = check_position(board, position);
    if (board_index == -1) return -1; //INVL MOVE
    else {
        board[board_index] = role[0];
        return 0;
    }

}

// Method for reading data from a client (threaded approach)
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
    char* board = ".........";

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

        // FIXME make sure we do not grab more than 1 message!!!!!!!!

        // Parse message
        char** tokens = malloc(sizeof(char*) * 5); //MAX FIELDS
        memset(tokens, (char) 0, 5);
        for (int i = 0; i < 5; i++) {
            tokens[i] = malloc(sizeof(char) * 256); //MAX FIELD SIZE
            memset(tokens[i], (char) 0, 256);
        }

        int tokerror = tokenize(buf, tokens);
        if (tokerror != 0) {//error has occured tokenizing
            printf("Error occured while tokenizing!\n"); // FIXME more specific error checking
        }
        else {
            printf("First Token: %s\n", tokens[0]);
            //if (first token is PLAY, MOVE, RSGN, or DRAW) //Make methods for each of these that do their proper function and returns -1 if unsuccessful or invalid
            int draw_match = 0;

            if(checkType(tokens[0]) == PLAY) {

                // FIXME for
                printf("Player Name: %s\n", tokens[2]); ///CHECK IF NAME IS TAKEN
                //board = createBoard();
                active_game = 1;

                //put in play check

            }
            else if(checkType(tokens[0]) == MOVE) {
                // This now assumes that there is an active game between 2 players
                // FIXME first grab player information from synchronized structures:
                   // playerOne, p1length, fd1, playerTwo, p2length, fd2, etc.

                // MASSIVE FIXME!!!!!!!!!!!!!!!!!!!!!!!! DO NOT USE CON FD IN FINAL, USE SPECIFIC PLAYER DESCRIPTORS!!!!!!!!!!!!!//////////////////////////

                if (make_move(board, tokens[3], tokens[2]) == -1) {
                    printf("INVL|24|That space is occupied.|\n");  
                    write(con->fd, "INVL|24|That space is occupied.|", 33); 
                    write(con->fd, "\n", 2);
                }
                else {
                    printf("MOVD|16|%s|%s|%s|\n", tokens[2], tokens[3], board); // FIXME PRINTF TO OTHER CLIENT
                    //write(con->fd, "MOVD|16|%s|%s|%s|", tokens[2], tokens[3], board); THIS DOESN"T WORK, LOOK UNDER THIS FOR THE WORKING VERSION
                    
                    // this is to write back to the client that their move was successful
                    write(con->fd, "MOVD|16|", 9);
                    write(con->fd, tokens[2], strlen(tokens[2]));
                    write(con->fd, "|", 2);
                    write(con->fd, tokens[3], strlen(tokens[3]));
                    write(con->fd, "|", 2); 
                    write(con->fd, tokens[2], strlen(board)); 
                    write(con->fd, "|", 2);
                    write(con->fd, "\n", 2);

                    // FIXME to other client, grab file descriptor
                    write(con->fd, "MOVD|16|", 9);
                    write(con->fd, tokens[2], strlen(tokens[2]));
                    write(con->fd, "|", 2);
                    write(con->fd, tokens[3], strlen(tokens[3]));
                    write(con->fd, "|", 2); 
                    write(con->fd, tokens[2], strlen(board)); 
                    write(con->fd, "|", 2);
                    write(con->fd, "\n", 2);
                }
            }
            else if(checkType(tokens[0]) == RSGN) {
                active_game = 0;
                write(con->fd, "OVER|", 6);
                //write(con->fd, (strlen() + 6), 3); // FIXME grab player name and length
                //write(con->fd, name, strlen(name));
                write(con->fd, "has resigned.|", 6);

                // FIXME PRINT TO OTHER PERSON HERE AS WELL
            }
            else if(checkType(tokens[0]) == DRAW && tokens[2][0] == 'S') {
                draw_match = 1; //means draw is suggested
                
                // FIXME change this to write to the other client
                write(con->fd, "DRAW|2|S|", 10);
                write(con->fd, "\n", 2);
            }
            else if(checkType(tokens[0]) == DRAW && tokens[2][0] == 'R') {
                //
                if(draw_match == 0) {
                    write(con->fd, "INVL|23|No draw suggested yet.|", 32); //no suggestion was made to reject or accept yet
                    write(con->fd, "\n", 2);
                }
                else {
                    draw_match = 0;
                    // FIXME change this to write to the other client
                    write(con->fd, "DRAW|2|R|", 10);
                    write(con->fd, "\n", 2);
                }
            }
            else if(checkType(tokens[0]) == DRAW && tokens[2][0] == 'A') {
                //
                if(draw_match == 0) {
                    printf("INVL TYPE - TRY AGAIN"); //no suggestion was made to reject or accept yet
                    write(con->fd, "\n", 2);
                }
                
                else {
                    active_game = 0;

                    write(con->fd, "OVER|5|Draw|", 13);
                    write(con->fd, "\n", 2);

                    // FIXME make this to the other client
                    write(con->fd, "OVER|5|Draw|", 13);
                    write(con->fd, "\n", 2);
                }
            }

        }

        for (int i = 0; i < 5; i++) {
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

// Method for setting up server sockets

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

    server *gameServer = createGameServer();
    int tempConnects = 0;

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
        
        //printf("This is before the moduluus check\n");
        // Make a new game when needed
        if (tempConnects % 2 == 0) { 
            printf("Appending a new game to the server list\n");
            newGame(gameServer);
        }
        tempConnects++;

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

    // Free game server and all associated games
    struct game *temp;
    while(gameServer->first != NULL)
    {   
        temp = gameServer->first;
        gameServer->first = gameServer->first->next;
        free(temp);
    }

    free(gameServer);
    puts("Shutting down");
    close(listener);

    pthread_exit(NULL);
    
    return EXIT_SUCCESS;

}
