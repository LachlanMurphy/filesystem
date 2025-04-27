/*
 * Basic TCP Web Server
 * Lachlan Murphy
 * 21 February 2025
 */


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <sys/sendfile.h>
#include "array.h"

#define BUFFERSIZE 2048

// argument struct for socket_handler function
typedef struct {
    int serverfd;
    int clientfd;
    struct sockaddr_in* serveraddr;
    socklen_t* addrlen;
    array* arr;
    pthread_t* thread_id;
} socket_arg_t;

// multi-thread function to handle new socket connections
void* socket_handler(void* arg);

// matches a file suffix with a file type
int find_file_type(char* file_name);

/*
* error - wrapper for perror
*/
void error(char *msg) {
    perror(msg);
    exit(1);
}

// sigint handler
void sigint_handler(int sig);

// global values
array socks; // semaphores used, thread safe
char server_dir[BUFFERSIZE]; // read only after main function initialization

int main(int argc, char** argv) {
    int sockfd, new_socket;
    int portno;
    int optval;
    struct sockaddr_in serveraddr;
    socklen_t addrlen = sizeof(serveraddr);

    /* 
    * check command line arguments
    */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[2]);

    strncpy(server_dir, argv[1], BUFFERSIZE);

    // check/create server dir
    struct stat st = {0};
    if (stat(argv[1], &st) == -1) {
        mkdir(argv[1], 777);
    }

    // set up signal handling
    signal(SIGINT, sigint_handler);

    // initialize shared array
    array_init(&socks);
    
    // socket: create the parent socket 
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) error("ERROR opening socket");

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    
    // build the server's Internet address
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    
    // bind: associate the parent socket with a port 
    if (bind(sockfd, (struct sockaddr *) &serveraddr, addrlen) < 0) error("ERROR on binding");
    

    // main loop, listen for sockets and deal with them
    while (1) {
        // wait for new connection to be prompted
        if (listen(sockfd, 5) < 0) error("ERROR on listen");
        
        // create new fd for that socket
        if ((new_socket = accept(sockfd, (struct sockaddr *) &serveraddr, &addrlen)) < 0) error("ERROR accepting new socket");

        // create new pthread to handle socket request
        // this pointer will eventually be removed from scopre without freeing the memmory
        // the memmory will be freed when the socket is successfully completed
        pthread_t* thread_id = malloc(sizeof(pthread_t));
        
        // init arguments for new thread
        socket_arg_t* socket_arg = malloc(sizeof(socket_arg_t)); // will also be freed later
        socket_arg->addrlen = &addrlen;
        socket_arg->clientfd = new_socket;
        socket_arg->serveraddr = &serveraddr;
        socket_arg->serverfd = sockfd;
        socket_arg->arr = &socks;

        // create thread
        pthread_create(thread_id, NULL, socket_handler, socket_arg);

        // update args
        socket_arg->thread_id = thread_id;

        // add new thread to array
        array_put(&socks, thread_id);

        // detatch thread so resources are unallocated independent of parent thread
        pthread_detach(*thread_id);
    }
}

void sigint_handler(int sig) {
    print_array(&socks);
    // wait for all sockets to finish computing
    while (socks.size);
    array_free(&socks);
    printf("Server closed on SIGINT\n");
    exit(0);
}

void* socket_handler(void* arg) {
    socket_arg_t* args = (socket_arg_t *) arg;
    char buf[BUFFERSIZE];
    bzero(buf, BUFFERSIZE);

    // read in message
    if (read(args->clientfd, buf, BUFFERSIZE) < 0) error("ERROR in reading from socket");
    printf("%s", buf);

    // parse message
    char req[2][BUFFERSIZE]; // req[0]=command ; req[1]=file
    char* tmp;
    int parse_err = 0;
    for (int i = 0; i < 2; i++) {
        if (i == 0) tmp = strtok(buf, " ");
        else tmp = strtok(NULL, " ");

        if (!tmp) {
            parse_err = 1;
            break;
        }
        memcpy(req[i], tmp, BUFFERSIZE);
    }



    // socket no longer needed
    close(args->clientfd);

    // remove current thread from global array
    array_get(args->arr, args->thread_id);

    // free memory alocated by malloc from main thread
    free(args->thread_id);
    free(args);
    return NULL;
}