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
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <openssl/md5.h>

#define BUFFERSIZE 2048
#define NUM_SERVERS 4
#define CONF_FILE "./dfc.conf"
#define DIG_LEN 10
#define PART_LEN 1

typedef struct {
    char filename[BUFFERSIZE];
    int parts[NUM_SERVERS];
    int disp; // boolean to tell if it should be displayed
    time_t time;
} file_status_t;

// error - wrapper for perror
void error(char *msg) {
    perror(msg);
    exit(1);
}

// gets addresses and port numbers for each server
int get_server_data(char addrs[NUM_SERVERS][BUFFERSIZE], int ports[NUM_SERVERS]);

// connects to a server
int server_connect(int* server_socket, struct hostent* server, struct sockaddr_in* serveraddr, char hostname[BUFFERSIZE], int port);

//parses the file name
int parse_filename(char* str, time_t* time, int* part, char* filename);

// compares two instances of file_status_t, used in qsort
int compare_filestatus(const void *a, const void *b);

int main(int argc, char** argv) {
    char server_addr[NUM_SERVERS][BUFFERSIZE];
    int portno[NUM_SERVERS];

    // check argument count
    if (argc == 1) {
        fprintf(stderr, "usage: %s <command> [filename] ... [filename]\n", argv[0]);
        exit(1);
    }

    if (get_server_data(server_addr, portno) < 0) error("ERROR reading conf file");

    // handle command
    if (!strncmp(argv[1], "list", sizeof("list"))) {
        int server_socket;
        struct hostent* server = NULL;
        struct sockaddr_in serveraddr;
        char buf[BUFFERSIZE];
        file_status_t* files = malloc(sizeof(file_status_t));
        int num_files = 0;
        int files_len = 1;

        // connect to each server and get list of all files
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (server_connect(&server_socket, server, &serveraddr, server_addr[i], portno[i]) < 0) continue;
            
            if (send(server_socket, "LIST NULL NULL", BUFFERSIZE, 0) < 0) error("ERROR in send");

            while (1) {
                char filename[BUFFERSIZE];
                int part;
                time_t time;
                int c;
                bzero(buf, BUFFERSIZE);
                if ((c = recv(server_socket, buf, BUFFERSIZE, 0)) < 0) error("ERROR in recv");

                char* line = buf;
                int end_signal = 0;
                while(1) {
                    if (!strncmp(line, "END_SEND", strlen("END_SEND"))) {end_signal = 1; break;}

                    parse_filename(line, &time, &part, filename);

                    int i;
                    for (i = 0; i < num_files; i++) 
                        if (!strncmp(filename, files[i].filename, BUFFERSIZE) && time == files[i].time) break;
                    
                    if (i == num_files) { // file doesn't exist in array
                        if (files_len == num_files) {
                            files = realloc(files, files_len*2*sizeof(file_status_t));
                            files_len *= 2;
                        }

                        strncpy(files[num_files].filename, filename, BUFFERSIZE);
                        files[num_files].parts[part-1] = 1;
                        files[num_files].disp = 1;
                        files[num_files].time = time;
                        num_files++;
                    } else { // file does exist in array
                        files[i].parts[part-1] = 1;
                    }

                    line += strlen(line) + 1;
                    if (strlen(line) == 0) break;
                }
                if (end_signal) break;
            }
        }

        // sort 
        qsort(files, num_files, sizeof(file_status_t), compare_filestatus);

        // now that we have all the server names we can print them out.
        if (num_files == 0) {
            printf("Server Empty\n");
        } else {
            for (int i = 0; i < num_files; i++) {
                int complete = 1;
                for (int part = 0; part < NUM_SERVERS; part++) if (!files[i].parts[part]) {complete = 0; break;}
                if (!files[i].disp) continue;
                
                if (complete)
                    printf("%s\n", files[i].filename);
                else
                    printf("%s [incomplete]\n", files[i].filename);
            }
        }

        // deallocate the array
        free(files);
    } else if (!strncmp(argv[1], "put", sizeof("put"))) {
        // put each file in the command line
        for (int file_num = 2; file_num < argc; file_num++) {
            unsigned char hash_bin[BUFFERSIZE] = {0};
            unsigned long long hash_offset;
            unsigned long file_size;
            time_t put_time;
            int server_socket;
            struct hostent* server = NULL;
            struct sockaddr_in serveraddr;

            // attempt to open file
            FILE* file = fopen(argv[file_num], "r");
            if (!file) {
                printf("File %s does not exist\n", argv[file_num]);
                continue;
            }

            // hash filename to determine where to go
            MD5_CTX md5_context;
            MD5_Init(&md5_context);
            MD5_Update(&md5_context, argv[file_num], sizeof(argv[file_num]));
            MD5_Final(hash_bin, &md5_context);

            for (int i = 0; i < 8; i++) hash_offset |= (unsigned long long)hash_bin[i] << (i * 8);
            hash_offset %= 4;

            put_time = time(NULL);

            //split file into chunks and send
            fseek(file, 0, SEEK_END);
            file_size = ftell(file);
            fseek(file, 0, SEEK_SET);

            int servers_down = 0;
            for (int file_part = 0; file_part < NUM_SERVERS; file_part++) {
                char part_name[BUFFERSIZE];
                unsigned long write_end = (file_part+1)*(file_size/NUM_SERVERS)+1;
                int server_index = (hash_offset+file_part) % NUM_SERVERS;
                if (file_part == NUM_SERVERS - 1) write_end = file_size;

                sprintf(part_name, "PUT %.10ld:%d:%s %ld", put_time, file_part+1, argv[file_num], write_end-file_part*(file_size/NUM_SERVERS));
                
                // connect to server
                if (server_connect(&server_socket, server, &serveraddr, server_addr[server_index], portno[server_index]) < 0) {
                    servers_down++;
                } else {
                    // send header so server knows file is coming
                    if (send(server_socket, part_name, strlen(part_name)+1, 0) < 0) error("ERROR in send");

                    // send file part over
                    off_t offset = file_part*(file_size/NUM_SERVERS);
                    if (sendfile(server_socket, fileno(file), &offset, write_end-offset) < 0) error("ERROR in sendfile1");
                    
                }
                close(server_socket);

                // attempt to send to next server
                server_index = (server_index+1) % NUM_SERVERS;
                if (server_connect(&server_socket, server, &serveraddr, server_addr[server_index], portno[server_index]) < 0) {
                    servers_down++;
                } else {
                    // send header so server knows file is coming
                    if (send(server_socket, part_name, strlen(part_name)+1, 0) < 0) error("ERROR in send");

                    // send file part over
                    off_t offset = file_part*(file_size/NUM_SERVERS);

                    if (sendfile(server_socket, fileno(file), &offset, write_end-offset) < 0) error("ERROR in sendfile");
                    
                }
                close(server_socket);
            }
        }
    }
}

int get_server_data(char addrs[NUM_SERVERS][BUFFERSIZE], int ports[NUM_SERVERS]) {
    // open conf file
    FILE* conf;
    if (!(conf = fopen(CONF_FILE, "r"))) return -1;

    char buf[BUFFERSIZE];
    int server_num = 0;
    while (fgets(buf, BUFFERSIZE, conf)) {
        char* tmp;
        for (int i = 0; i < 3; i++) {
            if (i == 0) tmp = strtok(buf, " ");
            else tmp = strtok(NULL, " ");
    
            if (i == 2) {
                int colon = strcspn(tmp, ":");
                strncpy(addrs[server_num], tmp, colon);
                addrs[server_num][colon] = '\0';
                ports[server_num] = atoi(tmp+colon+1);
            }
        }
        server_num++;
    }

    return 0;
}

int server_connect(int* server_socket, struct hostent* server, struct sockaddr_in* serveraddr, char hostname[BUFFERSIZE], int port) {
    int flags, result;
    fd_set fdset;
    struct timeval timeout;
    
    if ((*server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) error("server socket");
            
    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) serveraddr, sizeof(*serveraddr));
    serveraddr->sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&(serveraddr->sin_addr.s_addr), server->h_length);
    serveraddr->sin_port = htons(port);

    if ((flags = fcntl(*server_socket, F_GETFL, 0)) < 0 || fcntl(*server_socket, F_SETFL, flags | O_NONBLOCK) < 0)
        error("ERROR in fcntl");

    result = connect(*server_socket, (struct sockaddr *)serveraddr, sizeof(*serveraddr));

    if (result != 0) {
        FD_ZERO(&fdset);
        FD_SET(*server_socket, &fdset);
        timeout.tv_sec = 1; // one second timeout for sockets
        timeout.tv_usec = 0;

        result = select(*server_socket + 1, NULL, &fdset, NULL, &timeout);
        if (result == 0) {
            errno = ETIMEDOUT; // Set errno to indicate timeout
            return -1;
        } else if (result < 0) {
            return -1;
        }

        // Check for connection success or failure
        int err;
        socklen_t len = sizeof(err);
        if (getsockopt(*server_socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            errno = err;
            return -1;
        }
    }

    // Restore the socket to blocking mode
    if (fcntl(*server_socket, F_SETFL, flags) < 0) error("ERROR in fcntl");

    return 0;
}

int parse_filename(char* str, time_t* time, int* part, char filename[BUFFERSIZE]) {
    *time = strtol(str, NULL, 10);
    *part = atoi(str+DIG_LEN+1);
    strncpy(filename, str+DIG_LEN+PART_LEN+2, BUFFERSIZE);
    return 0;
}

int compare_filestatus(const void *a, const void *b) {
    file_status_t* one = (file_status_t*) a;
    file_status_t* two = (file_status_t*) b;

    int n = strncmp(one->filename, two->filename, BUFFERSIZE);
    
    if (n == 0) { // if they're the same file, compare times
        n = one->time - two->time;
        if (n > 0) two->disp = 0; else one->disp = 0;
        return n;
    } else {
        return n;
    }
}