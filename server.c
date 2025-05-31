// server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>



#define MAX_CLIENTS 100
#define BUFFER_SZ 2048

static _Atomic unsigned int cli_count = 0;
static int uid = 10;

typedef struct {
    int sockfd;
    int uid;
    char name[32];
} client_t;

client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Trim newline
void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; ++i) {
        if (arr[i] == '\n') { arr[i] = '\0'; break; }
    }
}

// Add client
void queue_add(client_t *cl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (!clients[i]) { clients[i] = cl; break; }
    pthread_mutex_unlock(&clients_mutex);
}

// Remove client
void queue_remove(int uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i] && clients[i]->uid == uid) {
            clients[i] = NULL;
            break;
        }
    pthread_mutex_unlock(&clients_mutex);
}

// Broadcast chat
void send_message(char *msg, int uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->uid != uid) {
            send(clients[i]->sockfd, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Find client by name
client_t *find_client_by_name(const char *name) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && strcmp(clients[i]->name, name) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

// Handle a single client
void *handle_client(void *arg) {
    client_t *cli = (client_t *)arg;
    char buff[BUFFER_SZ];
    int nbytes;

    // 1) receive name (32 bytes)
    if ((nbytes = recv(cli->sockfd, cli->name, 32, 0)) <= 0) {
        close(cli->sockfd);
        free(cli);
        return NULL;
    }
    str_trim_lf(cli->name, 32);

    // announce join
    snprintf(buff, sizeof(buff), "%s has joined\n", cli->name);
    printf("%s", buff);
    send_message(buff, cli->uid);
    cli_count++;

    // 2) main loop
    while ((nbytes = recv(cli->sockfd, buff, BUFFER_SZ, 0)) > 0) {
        buff[nbytes] = '\0';

        // file transfer?
        if (strncmp(buff, "FILE:", 5) == 0) {
            // parse header
            // FILE:<recipient>:<filename>:<size>\n
            char to[32], filename[256];
            long fsize;
            char *newline = strchr(buff, '\n');
            int header_len = newline ? (newline - buff + 1) : nbytes;
            sscanf(buff + 5, "%31[^:]:%255[^:]:%ld", to, filename, &fsize);

            client_t *target = find_client_by_name(to);
            if (target) {
                // forward header
                send(target->sockfd, buff, header_len, 0);
                // forward any initial data after the newline
                int data_len = nbytes - header_len;
                if (data_len > 0) {
                    send(target->sockfd, buff + header_len, data_len, 0);
                }
                // relay remaining
                long remaining = fsize - data_len;
                char filebuf[BUFFER_SZ];
                while (remaining > 0) {
                    int to_read = remaining > BUFFER_SZ ? BUFFER_SZ : remaining;
                    int r = recv(cli->sockfd, filebuf, to_read, 0);
                    if (r <= 0) break;
                    send(target->sockfd, filebuf, r, 0);
                    remaining -= r;
                }
            } else {
                char msg[] = "[Server] User not found\n";
                send(cli->sockfd, msg, strlen(msg), 0);
            }
        }
        else {
            // chat
            send_message(buff, cli->uid);
            printf("%s", buff);  // server log
        }
    }

    // client disconnected
    close(cli->sockfd);
    queue_remove(cli->uid);
    printf("%s has left\n", cli->name);
    snprintf(buff, sizeof(buff), "%s has left\n", cli->name);
    send_message(buff, cli->uid);
    free(cli);
    cli_count--;
    pthread_detach(pthread_self());
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    int port = atoi(argv[1]);
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr = {0}, cli_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    listen(listenfd, 10);

    printf("=== WELCOME TO THE CHATROOM ===\n");
    while (1) {
        socklen_t clilen = sizeof(cli_addr);
        int connfd = accept(listenfd,
                            (struct sockaddr *)&cli_addr,
                            &clilen);
        if (cli_count + 1 == MAX_CLIENTS) {
            char *msg = "Max clients reached\n";
            send(connfd, msg, strlen(msg), 0);
            close(connfd);
            continue;
        }
        client_t *cli = malloc(sizeof(client_t));
        cli->sockfd = connfd;
        cli->uid    = uid++;
        queue_add(cli);

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, cli);
        sleep(1);
    }
    return EXIT_SUCCESS;
}
