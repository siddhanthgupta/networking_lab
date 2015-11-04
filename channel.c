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
#include <semaphore.h>

#define MAX_MESSAGE_SIZE 1024
#define MAX_ATTEMPTS 50

#define PROBABILITY_MESSAGE_LOSS 40

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
        fprintf(stderr,
                "Channel error: Unable to create send message queue key file. Exiting.\n");
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
    srand(1);
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

/*Semaphore man info:
 Named semaphores
 A  named  semaphore  is identified by a name of the form /somename; that is, a null-
 terminated string of up to NAME_MAX-4 (i.e., 251) characters consisting of  an  ini‐
 tial slash, followed by one or more characters, none of which are slashes.  Two pro‐
 cesses can operate on  the  same  named  semaphore  by  passing  the  same  name  to
 sem_open(3).

 The  sem_open(3)  function  creates a new named semaphore or opens an existing named
 semaphore.  After the semaphore has  been  opened,  it  can  be  operated  on  using
 sem_post(3)  and  sem_wait(3).   When a process has finished using the semaphore, it
 can use sem_close(3) to close the semaphore.  When all processes have finished using
 the semaphore, it can be removed from the system using sem_unlink(3).

 On  Linux,  named  semaphores  are  created in a virtual filesystem, normally mounted under
 /dev/shm, with names of the form sem.somename.  (This is the reason  that  semaphore  names
 are limited to NAME_MAX-4 rather than NAME_MAX characters.)

 */

/*mmap info:

 #include <sys/mman.h>

 void *mmap(void *addr, size_t length, int prot, int flags,
 int fd, off_t offset);

 mmap()  creates  a  new  mapping  in the virtual address space of the calling process.  The
 starting address for the new mapping is specified in addr.  The length  argument  specifies
 the length of the mapping.

 If  addr  is NULL, then the kernel chooses the address at which to create the mapping; this
 is the most portable method of creating a new mapping.

 The prot argument describes the desired memory protection of the mapping (and must not con‐
 flict  with the open mode of the file).  It is either PROT_NONE or the bitwise OR of one or
 more of the following flags:

 PROT_EXEC  Pages may be executed.

 PROT_READ  Pages may be read.

 PROT_WRITE Pages may be written.

 PROT_NONE  Pages may not be accessed.

 The flags argument determines whether updates to the mapping are visible to other processes
 mapping  the  same  region, and whether updates are carried through to the underlying file.
 This behavior is determined by including exactly one of the following values in flags:

 MAP_SHARED Share this mapping.  Updates to the mapping are visible to other processes  that
 map this file, and are carried through to the underlying file.  The file may not
 actually be updated until msync(2) or munmap() is called.

 MAP_ANONYMOUS
 The mapping is not backed by any file; its contents are initialized to zero.  The fd
 and  offset arguments are ignored; however, some implementations require fd to be -1
 if MAP_ANONYMOUS (or MAP_ANON) is specified, and portable applications should ensure
 this.  The use of MAP_ANONYMOUS in conjunction with MAP_SHARED is supported on Linux
 only since kernel 2.4.
 */

sem_t* mutex_send;                    //Semaphore to access the buffer
sem_t* mutex_receive;      //Holds the number of full buffers (intiialised to 0)

/*     sem_wait() decrements (locks) the semaphore pointed to by sem.  If the semaphore's value is
 greater than zero, then the decrement proceeds, and the function returns, immediately.   If
 the  semaphore  currently  has the value zero, then the call blocks until either it becomes
 possible to perform the decrement (i.e., the semaphore value rises above zero), or a signal
 handler interrupts the call.

 All of these functions return 0 on success; on error, the value of the  semaphore  is  left
 unchanged, -1 is returned, and errno is set to indicate the error.

 ERRORS
 EINTR  The call was interrupted by a signal handler; see signal(7).

 */
//Function encapsulates error handling for the sem_wait
void semaphoreWait(sem_t* semaphore) {
    int x;
    fprintf(stderr,
            "Status Report: semaphoreWait : Semaphore wait sent from process %ld.\n",
            (long) getpid());
    while ((x = sem_wait(semaphore)) == -1 && errno == EINTR) //Restart if interrupted by handler
        continue;
    if (x == -1)                     //Some error happened. We abort the program
            {
        fprintf(stderr,
                "Error: semaphoreWait : Semaphore failed to lock (wait)\n");
        abort();
    }
    fprintf(stderr,
            "Status Report: semaphoreWait : Process %ld ain't waitin' no more.\n",
            (long) getpid());
}

/*
 sem_post(3)
 SYNOPSIS
 #include <semaphore.h>

 int sem_post(sem_t *sem);

 Link with -pthread.

 DESCRIPTION
 sem_post()  increments  (unlocks) the semaphore pointed to by sem.  If the semaphore's value consequently becomes
 greater than zero, then another process or thread blocked in a sem_wait(3) call will be woken up and  proceed  to
 lock the semaphore.

 RETURN VALUE
 sem_post()  returns  0  on  success;  on error, the value of the semaphore is left unchanged, -1 is returned, and
 errno is set to indicate the error.
 */

//Function encapsulates the sem_post error handling part
void semaphoreSignal(sem_t* semaphore) {
    int x;
    x = sem_post(semaphore);
    fprintf(stderr,
            "Status Report: semaphoreSignal : Semaphore signal sent from process %ld.\n",
            (long) getpid());
    if (x == -1) {
        fprintf(stderr,
                "Error: semaphoreSignal : Semaphore failed to unlock (signal)\n");
        abort();
    }
}

//Initialises the semaphores and shared variables
int initializeSemaphores() {
    int i;
    char str[1024];
    sprintf(str, "MUTEX SEND QUEUE");
    sem_unlink(str);
    //printf("%s\n",str);
    if ((mutex_send = sem_open(str, O_CREAT, S_IRUSR | S_IWUSR, 1))
            == SEM_FAILED) {
        fprintf(stderr,
                "Error: semaphoreInitialization : Semaphore %s failed to initialise\n",
                str);
        abort();
    }

    sprintf(str, "MUTEX RECEIVE QUEUE");
    sem_unlink(str);
    //printf("%s\n",str);
    if ((mutex_receive = sem_open(str, O_CREAT, S_IRUSR | S_IWUSR, 1))
            == SEM_FAILED) {
        fprintf(stderr,
                "Error: semaphoreInitialization : Semaphore %s failed to initialise\n",
                str);
        abort();
    }
    return 1;
}

struct my_message* make_ip_message(int pid, int pid_dest, char* str_message,
        int msg_type, int msg_seq) {
    struct my_message* message = (struct my_message*) malloc(
            sizeof(struct my_message));
    message->mtype = (long) pid;
    strcpy((message->data).message, str_message);

    (message->data).dest_pid = pid_dest;
    (message->data).src_pid = pid;
    (message->data).msg_type = msg_type;
    (message->data).msg_seq = msg_seq;

    return message;
}

void send_message_to_send_queue(struct my_message* message) {
    semaphoreWait(mutex_send);
    if (msgsnd(send_queue_id, message, sizeof(struct my_message) - sizeof(long),
            0) == -1) {
        printf("Channel error: Unable to send to send queue.\n");
        semaphoreSignal(mutex_send);
        exit(1);
    }
    semaphoreSignal(mutex_send);
}

struct my_message* receive_message_from_receive_queue(int pid) {
    struct my_message* message = (struct my_message*) malloc(
            sizeof(struct my_message));
    semaphoreWait(mutex_receive);
    int status = msgrcv(receive_queue_id, message,
            sizeof(struct my_message) - sizeof(long), (long) pid, IPC_NOWAIT);
    if (status == -1) {
        if (errno == ENOMSG) {
            semaphoreSignal(mutex_receive);
            return NULL;
        }
        fprintf(stderr,
                "Channel error: Unable to receive from receive queue.\n");
        semaphoreSignal(mutex_receive);
        exit(1);
    }
    semaphoreSignal(mutex_receive);
    return message;
}

void send_message_to_receive_queue(struct my_message* message) {
    semaphoreWait(mutex_receive);
    if (msgsnd(receive_queue_id, message,
            sizeof(struct my_message) - sizeof(long), 0) == -1) {
        fprintf(stderr, "Channel error: Unable to send to receive queue.\n");
        semaphoreSignal(mutex_receive);
        exit(1);
    }
    semaphoreSignal(mutex_receive);
}

struct my_message* receive_message_from_send_queue(int pid) {
    semaphoreWait(mutex_send);
    struct my_message* message = (struct my_message*) malloc(
            sizeof(struct my_message));
    int status = msgrcv(send_queue_id, message,
            sizeof(struct my_message) - sizeof(long), (long) pid, IPC_NOWAIT);
    if (status == -1) {
        if (errno == ENOMSG) {
            semaphoreSignal(mutex_send);
            return NULL;
        }
        fprintf(stderr, "Channel error: Unable to receive from send queue.\n");
        semaphoreSignal(mutex_send);
        exit(1);
    }
    semaphoreSignal(mutex_send);
    return message;
}

int transfer_messages_between_queues() {
    struct my_message** msg_arr = (struct my_message**) malloc(
            sizeof(struct my_message*) * 100);

    int c = 0, status = 0, i = 0;
    do {
        struct my_message* message = receive_message_from_send_queue(0);
        if (message == NULL) {
            break;
        }
        msg_arr[c] = message;
        c++;
    } while (status != -1);
    for (i = c - 1; i >= 0; i--) {
        msg_arr[i]->mtype = (msg_arr[i]->data).dest_pid;
        int r = rand() % 100;
        if (r > PROBABILITY_MESSAGE_LOSS)
            send_message_to_receive_queue(msg_arr[i]);
    }
    return c;
}

void destroy_channel() {
    msgctl(send_queue_id, IPC_RMID, NULL);
    msgctl(receive_queue_id, IPC_RMID, NULL);
    char str[150];
    sprintf(str, "MUTEX SEND QUEUE");
    sem_unlink(str);

    sprintf(str, "MUTEX RECEIVE QUEUE");
    sem_unlink(str);
}

int receiver() {
    pid_t pid = getpid();
    printf("Receiver process is %d\n", (int) pid);
    int c = 0, attempts = 0;
    int expected_seq = 0;
    char dummy_str[2] = "\0";
    while (attempts < MAX_ATTEMPTS) {
        sleep(3);
        attempts++;
        struct my_message* message = receive_message_from_receive_queue(pid);
        if (message == NULL) {
            //printf("RECEIVER %d : No message received\n", (int) pid);
        } else if ((message->data).msg_seq != expected_seq) {
            int obtained_seq = (message->data).msg_seq;
            printf("RECEIVER %d : Invalid SEQ received. Resend old ACK\n",
                    (int) pid);
            struct my_message* ret_msg = make_ip_message(pid,
                    (message->data).src_pid, dummy_str,
                    ACK, (obtained_seq + 1) % 2);
            send_message_to_send_queue(ret_msg);
        } else {
            printf("RECEIVER %d : Receives message: %s\n", (int) pid,
                    (message->data).message);
            int obtained_seq = (message->data).msg_seq;
            expected_seq = (obtained_seq + 1) % 2;
            struct my_message* ret_msg = make_ip_message(pid,
                    (message->data).src_pid, dummy_str,
                    ACK, expected_seq);
            send_message_to_send_queue(ret_msg);
        }
    }
    if (attempts == MAX_ATTEMPTS) {
        printf("RECEIVER %d : Receiver got tired of waiting.\n", (int) pid);
    }
    printf("RECEIVER %d : Exiting.\n", (int) pid);
    return 0;
}

int sender(pid_t pid_receiver) {
    // Child sender process
    pid_t pid = getpid();
    printf("SENDER %d : Receiver process is %d\n", (int) pid, pid_receiver);
    int i, quit_count = 0;
    for (i = 0; i < 10 && quit_count < 10; i++) {
        char str[MAX_MESSAGE_SIZE];
        sprintf(str, "Hello, This is message %d", i);
        struct my_message* message = make_ip_message(pid, pid_receiver, str,
        DATA, i % 2);
        send_message_to_send_queue(message);
        printf("SENDER %d : Sender sent message %s\n", (int) pid,
                message->data.message);
        sleep(6);
        struct my_message* ack_msg = receive_message_from_receive_queue(pid);
        if (ack_msg == NULL) {
            // did not receive acknowledgment
            // Resends the message again
            printf("SENDER %d: Did not receive ACK\n", (int) pid);
            quit_count++;
            if (quit_count == 10)
                printf("SENDER %d: Too many quits. Exiting.\n", (int) pid);
            i--;
        } else if ((ack_msg->data).msg_seq != ((i + 1) % 2)) {
            printf(
                    "SENDER %d: Received acknowledgment saying send message with seq %d, but expected seq %d\n",
                    (int) pid, (ack_msg->data).msg_seq, ((i + 1) % 2));
            i--;
            quit_count = 0;
        } else {
            printf(
                    "SENDER %d: Received acknowledgment saying send message with seq %d\n",
                    (int) pid, (ack_msg->data).msg_seq);
            quit_count = 0;
        }
    }
    printf("SENDER %d : Exiting.\n", (int) pid);
    return 0;
}

int main() {
    initialise_channel();
    destroy_channel();
    initialise_channel();
    initializeSemaphores();

    pid_t pid_receiver = fork();
    if (pid_receiver < 0) {
        fprintf(stderr, "Channel: Error: Unable to fork receiver process.\n");
        exit(1);
    } else if (pid_receiver == 0) {
        // Receiver Process
        receiver();
    } else {
        // Parent Process
        pid_t pid_sender = fork();
        if (pid_sender < 0) {
            fprintf(stderr, "Channel: Error: Unable to fork sender process.\n");
            exit(1);
        } else if (pid_sender == 0) {
            // Sender process
            sender(pid_receiver);

        } else {
            // Parent process
            pid_t pid;
            int i;
            for (i = 0; i < 1000; i++) {
                sleep(1);
                int messages_transferred = transfer_messages_between_queues();
            }
            while (wait(NULL) != -1)
                ;
            destroy_channel();
        }
    }
    return 0;
}
