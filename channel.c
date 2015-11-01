/*
 * channel.c
 *
 *  Created on: 24-Oct-2015
 *      Author: siddhanthgupta
 */
/*
 * To create a channel, first we need to
 *  - Create and initialize message queues (check if they require creation or msgget is enough)
 *  - Introduce random delays, and stuff
 *  - Introduce errors
 *  - Sender and receiver are oblivious to the existence of message queues. They simply call send and receive
 *    functions using their PIDs
 *  - Mutexes required for send and receive queue access to prevent race-conditions
 *
 */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>            // For message queue

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>              // For open

#include <netinet/ip.h>         // For IP header
#include <netinet/in.h>         // For IP header
#include <unistd.h>             // For fork

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_MESSAGE_SIZE 1024

// Message Types
#define ACK 0
#define DATA 1

char send_key_path[80] = "/tmp/send_queue";
char receive_key_path[80] = "/tmp/receive_queue";
char send_key_id = 'S';
char receive_key_id = 'R';

int send_queue_id;
int receive_queue_id;

struct my_message {
    long mtype;
    struct my_ip {
        int src_pid;
        int dest_pid;
        int msg_type;
        int msg_seq;
        char message[MAX_MESSAGE_SIZE];
        int checksum;
    } data;
};

void create_files() {
    int fd1 = open(send_key_path, O_RDWR | O_CREAT,
    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH);
    if (fd1 == -1) {
        fprintf(stderr, "Channel error: Unable to create send message queue key file. Exiting.\n");
        exit(1);
    }
    int fd2 = open(receive_key_path, O_RDWR | O_CREAT,
    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH);
    if (fd2 == -1) {
        fprintf(stderr,
                "Channel error: Unable to create receive message queue key file. Exiting.\n");
        exit(1);
    }
    close(fd1);
    close(fd2);
}

void make_send_queue() {
    key_t key = ftok(send_key_path, send_key_id);
    if (key == -1) {
        fprintf(stderr, "Channel error: Unable to make send key.\n");
        exit(1);
    }
    send_queue_id = msgget(key,
    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH | IPC_CREAT);
    if (send_queue_id == -1) {
        fprintf(stderr, "Channel error: Unable to make send queue.\n");
        exit(1);
    }
}

void make_receive_queue() {
    key_t key = ftok(receive_key_path, receive_key_id);
    if (key == -1) {
        fprintf(stderr, "Channel error: Unable to make receive key.\n");
        exit(1);
    }
    receive_queue_id = msgget(key,
    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH | IPC_CREAT);
    if (receive_queue_id == -1) {
        fprintf(stderr, "Channel error: Unable to make receive queue.\n");
        exit(1);
    }
}

void initialise_channel() {
    create_files();
    make_receive_queue();
    make_send_queue();
}

/*
 * in_cksum --
 *  Checksum routine for Internet Protocol family headers (C Version)
 *  Source is from FreeBSD open source code
 *
 *  Copyright (c) 1985, 1993
 *  The Regents of the University of California.  All rights reserved.
 *
 */
static int in_cksum(u_short *addr, int len) {
    register int nleft = len;
    register u_short *w = addr;
    register int sum = 0;
    u_short answer = 0;

    /*
     * Our algorithm is simple, using a 32 bit accumulator (sum), we add
     * sequential 16 bit words to it, and at the end, fold back all the
     * carry bits from the top 16 bits into the lower 16 bits.
     */
    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    /* mop up an odd byte, if necessary */
    if (nleft == 1) {
        *(u_char *) (&answer) = *(u_char *) w;
        sum += answer;
    }

    /* add back carry outs from top 16 bits to low 16 bits */
    sum = (sum >> 16) + (sum & 0xffff); /* add hi 16 to low 16 */
    sum += (sum >> 16); /* add carry */
    answer = ~sum; /* truncate to 16 bits */
    return (answer);
}

struct my_message* make_ip_message(int pid, int pid_dest, char* str_message, int msg_type, int msg_seq) {
    struct my_message* message = (struct my_message*) malloc(sizeof(struct my_message));
    message->mtype = (long) pid;
    strcpy((message->data).message, str_message);

    (message->data).dest_pid = pid_dest;
    (message->data).src_pid = pid;
    (message->data).msg_type = msg_type;
    (message->data).msg_seq = msg_seq;

    return message;
}

void send_message_to_send_queue(struct my_message* message) {
    if (msgsnd(send_queue_id, message, sizeof(struct my_message) - sizeof(long), 0) == -1) {
        fprintf(stderr, "Channel error: Unable to send to send queue.\n");
        exit(1);
    }
}

struct my_message* receive_message_from_receive_queue(int pid) {
    struct my_message* message = (struct my_message*) malloc(sizeof(struct my_message));
    int status = msgrcv(receive_queue_id, message, sizeof(struct my_message) - sizeof(long), (long) pid, IPC_NOWAIT);
    if (status == -1) {
        if (errno == ENOMSG)
            return NULL;
        fprintf(stderr, "Channel error: Unable to receive from receive queue.\n");
        exit(1);
    }
    return message;
}

void send_message_to_receive_queue(struct my_message* message) {
    if (msgsnd(receive_queue_id, message, sizeof(struct my_message) - sizeof(long), 0) == -1) {
        fprintf(stderr, "Channel error: Unable to send to receive queue.\n");
        exit(1);
    }
}

struct my_message* receive_message_from_send_queue(int pid) {
    struct my_message* message = (struct my_message*) malloc(sizeof(struct my_message));
    if (msgrcv(send_queue_id, message, sizeof(struct my_message) - sizeof(long), (long) pid, 0) == -1) {
        fprintf(stderr, "Channel error: Unable to receive from send queue.\n");
        exit(1);
    }
    return message;
}

int transfer_messages_between_queues() {
    struct my_message** msg_arr = (struct my_message**) malloc(sizeof(struct my_message*) * 100);
    int c = 0, status = 0, i = 0;
    do {
        struct my_message* message = (struct my_message*) malloc(sizeof(struct my_message));
        status = msgrcv(send_queue_id, message, sizeof(struct my_message) - sizeof(long), 0, IPC_NOWAIT);
        if (status == -1) {
            break;
        }
        msg_arr[c] = message;
        c++;
    } while (status != -1);
    for (i = c - 1; i >= 0; i--) {
        msg_arr[i]->mtype = (msg_arr[i]->data).dest_pid;
        send_message_to_receive_queue(msg_arr[i]);
    }
    return c;
}

void destroy_channel() {
    msgctl(send_queue_id, IPC_RMID, NULL);
    msgctl(receive_queue_id, IPC_RMID, NULL);
}

int main() {
    initialise_channel();
    destroy_channel();
    initialise_channel();
    pid_t pid_receiver = fork();
    if (pid_receiver < 0) {
        fprintf(stderr, "Channel: Error: Unable to fork receiver process.\n");
        exit(1);
    } else if (pid_receiver == 0) {
        // Child receiver Process
        pid_t pid = getpid();
        printf("Receiver process is %d\n", (int) pid);
        int c = 0, attempts = 0, expected_seq = 0;
        while (c < 5 && attempts < 20) {
            attempts++;
            struct my_message* message = receive_message_from_receive_queue(pid);
            if (message != NULL) {
                c++;
                printf("RECEIVER PROCESS %d receives message number %d: %s\n", (int) pid, c,
                        (message->data).message);
                int obtained_seq = (message->data).msg_seq;
                if (obtained_seq == expected_seq) {
                    // The sequence number is what was expected
                    // In this case, we send an acknowledgement with the updated expected number
                    expected_seq = (expected_seq + 1) % 2;
                    printf("Expected sequence number received\n");
                }
                // If the sequence number received was as expected, expected sequence number has
                // been updated. Otherwise, it sends an ACK with the same sequence number as last
                // time
                struct my_message* ret_msg = make_ip_message(pid, (message->data).src_pid, (message->data).message, ACK,
                        expected_seq);
                send_message_to_send_queue(ret_msg);
            }
            sleep(3);
        }
        if (attempts == 20) {
            printf("Receiver got tired of waiting.\n");
        }
        printf("Receiver got %d messages\n", c);
    } else {
        // Parent Process
        pid_t pid_sender = fork();
        if (pid_sender < 0) {
            fprintf(stderr, "Channel: Error: Unable to fork sender process.\n");
            exit(1);
        } else if (pid_sender == 0) {
            // Child sender process
            printf("according to sender, receiver process is %d\n", pid_receiver);
            pid_t pid = getpid();
            int i;
            for (i = 0; i < 5; i++) {
                char str[MAX_MESSAGE_SIZE];
                sprintf(str, "Hello, This is message %d", i + 1);
                struct my_message* message = make_ip_message(pid, pid_receiver, str, DATA, i % 2);
                send_message_to_send_queue(message);
                //printf("Sender sent message number %d\n", i + 1);
                sleep(6);
                struct my_message* ack_msg = receive_message_from_receive_queue(pid);
                if (ack_msg == NULL || (ack_msg->data).msg_seq != (i+1)%2) {
                    // did not receive acknowledgement
                    // Resends the message again
                    printf("Invalid seq or no ack\n");
                    i--;
                } else {
                    printf("Received acknowledgement saying send message with seq %d\n",
                            (ack_msg->data).msg_seq);
                }
            }

        } else {
            // parent process
            pid_t pid;
            int i;
            for (i = 0; i < 15; i++) {
                sleep(2);
                int x = transfer_messages_between_queues();
               // printf("%d messages transferred by channel\n", x);
            }
            while ((pid = waitpid(-1, NULL, WNOHANG)) != 0) {
                if (errno == ECHILD) {
                    break;
                }
            }
            destroy_channel();
        }
    }
    return 0;
}
