// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define LENGTH 2048

volatile sig_atomic_t flag = 0;
int sockfd = 0;
char name[32];

void str_overwrite_stdout() {
    printf("> ");
    fflush(stdout);
}

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; ++i) {
        if (arr[i] == '\n') { arr[i] = '\0'; break; }
    }
}

void catch_ctrl_c_and_exit(int sig) {
    flag = 1;
}

// Read one line (up to '\n')
int read_line(int sockfd, char *buffer, int maxlen) {
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        int n = recv(sockfd, &c, 1, 0);
        if (n == 1) {
            if (c == '\n') { buffer[i] = '\0'; return i; }
            buffer[i++] = c;
        } else if (n == 0) {
            return 0;
        } else {
            return -1;
        }
    }
    buffer[i] = '\0';
    return i;
}

// Receive exactly fsize bytes and write to filename
void recv_file(const char *filename, long fsize) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen");
        // drain
        char tmp[LENGTH];
        long rem = fsize;
        while (rem > 0) {
            int r = recv(sockfd, tmp, rem > sizeof(tmp) ? sizeof(tmp) : rem, 0);
            if (r <= 0) break;
            rem -= r;
        }
        return;
    }
    long rem = fsize;
    char buf[LENGTH];
    while (rem > 0) {
        int to_read = rem > sizeof(buf) ? sizeof(buf) : rem;
        int r = recv(sockfd, buf, to_read, 0);
        if (r <= 0) break;
        fwrite(buf, 1, r, fp);
        rem -= r;
    }
    fclose(fp);
    if (rem == 0)
        printf("[+] File \"%s\" received successfully\n", filename);
    else
        printf("[-] File \"%s\" incomplete\n", filename);
    str_overwrite_stdout();
}

// Thread: receive chat or file headers
void *recv_handler(void *arg) {
    char line[LENGTH];
    while (1) {
        int len = read_line(sockfd, line, LENGTH);
        if (len <= 0) break;

        if (strncmp(line, "FILE:", 5) == 0) {
            // FILE:<sender>:<filename>:<size>
            char sender[32], filename[256];
            long fsize;
            sscanf(line + 5, "%31[^:]:%255[^:]:%ld",
                   sender, filename, &fsize);
            recv_file(filename, fsize);
        } else {
            // plain chat
            printf("%s\n", line);
            str_overwrite_stdout();
        }
    }
    return NULL;
}

// Thread: read user input, send chat or files
void *send_handler(void *arg) {
    char input[LENGTH];
    while (1) {
        str_overwrite_stdout();
        if (!fgets(input, LENGTH, stdin)) break;
        str_trim_lf(input, LENGTH);

        if (strcmp(input, "exit") == 0) {
            break;
        }
        if (strncmp(input, "/sendto ", 8) == 0) {
            // /sendto <user> <filepath>
            char to[32], filepath[256];
            if (sscanf(input + 8, "%31s %255s", to, filepath) == 2) {
                FILE *fp = fopen(filepath, "rb");
                if (!fp) { perror("fopen"); continue; }
                fseek(fp, 0, SEEK_END);
                long fsize = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                // header
                char header[LENGTH];
                snprintf(header, sizeof(header),
                         "FILE:%s:%s:%ld\n",
                         to, filepath, fsize);
                send(sockfd, header, strlen(header), 0);

                // data
                char buf[LENGTH];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                    send(sockfd, buf, n, 0);
                }
                fclose(fp);
                printf("[+] File \"%s\" sent to %s\n", filepath, to);
            } else {
                printf("Usage: /sendto <username> <filepath>\n");
            }
        } else {
            // chat
            char msg[LENGTH];
            snprintf(msg, sizeof(msg), "%s: %s\n", name, input);
            send(sockfd, msg, strlen(msg), 0);
        }
    }
    catch_ctrl_c_and_exit(2);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGINT, catch_ctrl_c_and_exit);

    printf("Please enter your name: ");
    fgets(name, sizeof(name), stdin);
    str_trim_lf(name, strlen(name));

    int port = atoi(argv[1]);
    struct sockaddr_in serv_addr = {0};
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("connect");
        return EXIT_FAILURE;
    }

    // send name (32 bytes)
    send(sockfd, name, 32, 0);
    printf("=== WELCOME TO THE CHATROOM ===\n");

    pthread_t recv_tid, send_tid;
    pthread_create(&recv_tid, NULL, recv_handler, NULL);
    pthread_create(&send_tid, NULL, send_handler, NULL);

    while (!flag) { }
    printf("\nBye\n");
    close(sockfd);
    return 0;
}
