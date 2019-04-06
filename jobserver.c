#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>

#include "socket.h"
#include "jobprotocol.h"

#define QUEUE_LENGTH 5
#define MAX_CLIENTS 20

#ifndef JOBS_DIR
    #define JOBS_DIR "jobs/"
#endif

// Global list of jobs
JobList job_list;

// Flag to keep track of SIGINT received
int sigint_received;

// Number of clients currently connected
int client_count;

/* SIGINT handler:
 * We are just raising the sigint_received flag here. Our program will
 * periodically check to see if this flag has been raised, and any necessary
 * work will be done in main() and/or the helper functions. Write your signal 
 * handlers with care, to avoid issues with async-signal-safety.
 */
void sigint_handler(int code) {
    write(STDOUT_FILENO, "\n", 1);
    sigint_received = 1;
}

// TODO: SIGCHLD (child stopped or terminated) handler: mark jobs as dead
void sigchld_handler(int code) {
    int stat;
    int pid = wait(&stat);

    mark_job_dead(&job_list, pid, stat);
}

int announce_buf_to_client(int client_fd, char *buf, int buflen);
int announce_str_to_client(int client_fd, char* str);
int announce_fstr_to_client(int client_fd, const char *format, ...);
int get_highest_fd(int listen_fd, Client *clients, JobList *job_list);

/*
 *  Client management
 */

/* Accept a connection and adds them to list of clients.
 * Return the new client's file descriptor or -1 on error.
 */
int setup_new_client(int listen_fd, Client *clients) {
    int new_fd = accept_connection(listen_fd);
    if (new_fd < 0) {
        return -1;
    }

    Client new_client = {new_fd};
    clients[client_count] = new_client;
    client_count++;

    return new_fd;
}

/* Closes a client and removes it from the list of clients.
 * Return the highest fd between all clients.
 */
int remove_client(int listen_fd, int client_index, Client *clients, JobList *job_list) {
    close(clients[client_index].socket_fd);

    client_count--;
    for (int i = client_index; i < client_count; i++) {
        clients[i] = clients[i + 1];
    }

    // TODO: Remove client from jobs and kill empty jobs

    return get_highest_fd(listen_fd, clients, job_list);
}

/* Read message from client and act accordingly.
 * Return their fd if it has been closed or 0 otherwise.
 */
int process_client_request(Client *client, JobList *job_list, fd_set *all_fds) {
    Buffer *client_buf = &(client->buffer);
    int client_fd = client->socket_fd;

    int read_res = read_to_buf(client_fd, client_buf);
    if (read_res == 0) {
        return client_fd;
    } else if (read_res == -1) {
        return 0;
    }

    int msg_len;
    char *msg;
    while ((msg = get_next_msg(client_buf, &msg_len, NEWLINE_CRLF)) != NULL) {
        msg[msg_len - 2] = '\0';

        int log_len;
        char cmd_log[BUFSIZE];
        snprintf(cmd_log, BUFSIZE, "[CLIENT %d] %s", client_fd, msg);
        log_len = strlen(cmd_log);
        cmd_log[log_len] = '\n';
        write(STDOUT_FILENO, cmd_log, log_len + 1);
        
        char cpy[msg_len - 1];
        strncpy(cpy, msg, msg_len - 1);
        JobCommand command = get_job_command(cpy);

        switch (command) {
            case CMD_LISTJOBS:
            {
                char jobs[BUFSIZE + 1] = "";
                for (JobNode *job = job_list->first; job != NULL; 
                        job = job->next) {
                    if (!(job->dead)) {
                        snprintf(jobs, BUFSIZE + 1, "%s %d", 
                                jobs, job->pid);
                    }
                }
                if (jobs[0] == '\0') {
                    announce_str_to_client(client_fd, "[SERVER] No currently running jobs");
                } else {
                    announce_fstr_to_client(client_fd, "[SERVER]%s", jobs);
                }
                        
                break;
            }
            case CMD_RUNJOB:
                if (job_list->count >= MAX_JOBS) {
                    announce_str_to_client(client_fd, "[SERVER] MAXJOBS exceeded");
                } else {
                    char *name = strtok(NULL, " ");
                    if (name == NULL) {
                        announce_fstr_to_client(client_fd, "[SERVER] Invalid command: %s", msg);
                    } else {
                        char exe_file[BUFSIZE];
                        snprintf(exe_file, BUFSIZE, "%s/%s", JOBS_DIR, name);
                        if (strchr(msg, '/') != NULL) {
                            announce_fstr_to_client(client_fd, "[SERVER] Invalid command: %s", msg);
                        } else {
                            char *args[BUFSIZE];
                            args[0] = name;
                            int i = 1;
                            char *arg;
                            while ((arg = strtok(NULL, " ")) != NULL) {
                                args[i] = arg;
                                i++;
                            }
                            args[i] = NULL;

                            JobNode *job = start_job(exe_file, args);
                            if (job == NULL) {
                                sigint_received = 1;
                            } else {
                                WatcherNode *watcher = malloc(sizeof(WatcherNode));
                                if (watcher == NULL) {
                                    perror("malloc");
                                    sigint_received = 1;
                                } else {
                                    watcher->client_fd = client_fd;
                                    watcher->next = NULL;
                                    job->watcher_list.first = watcher;
                                    job->watcher_list.count = 1;
                                    add_job(job_list, job);
                                    FD_SET(job->stdout_fd, all_fds);
                                    FD_SET(job->stderr_fd, all_fds);
                                    announce_fstr_to_client(client_fd, "[SERVER] Job %d created", job->pid);
                                }
                            }
                        }
                    }
                }
                break;
            case CMD_KILLJOB:
            {
                char *pid_str = strtok(NULL, " ");
                int pid;
                if (pid_str == NULL || (pid = strtol(pid_str, NULL, 10)) <= 0) {
                    announce_fstr_to_client(client_fd, "[SERVER] Invalid command: %s", msg);
                } else if (kill_job(job_list, pid) == 1) {
                    announce_fstr_to_client(client_fd, "[SERVER] Job %d not found", pid);
                }
                break;
            }
            default:    
                announce_fstr_to_client(client_fd, "[SERVER] Invalid command: %s", msg);
        }

    }

    if (is_buffer_full(client_buf) && client_buf->consumed == 0) {
        client_buf->consumed = 1;
    }

    shift_buffer(client_buf);

    return 0;
}

/*
 *  Sending to client
 */

/* Write a string to a client.
 * Returns 0 on success, 1 on failed/incomplete write, or -1 in case of error.
 */
int write_buf_to_client(int client_fd, char *buf, int buflen) {
    int nbytes = write(client_fd, buf, buflen);
    if (nbytes == buflen) {
        return 0;
    }
    if (nbytes < 0) {
        return -1;
    }
    return 1;
}


/* Print message to stdout, and send network-newline message to a client.
 * Returns 0 on success, 1 on failed/incomplete write, or -1 in case of error.
 */
int announce_buf_to_client(int client_fd, char *buf, int buflen) {
    buf[buflen] = '\n';
    write(STDOUT_FILENO, buf, buflen + 1);

    buf[buflen] = '\r';
    buf[buflen + 1] = '\n';
    return write_buf_to_client(client_fd, buf, buflen + 2);
}


/* Print string to stdout, and send network-newline string to a client.
 * Returns 0 on success, 1 on failed/incomplete write, 2 if the string
 * is too large, or -1 in case of error.
 */
int announce_str_to_client(int client_fd, char* str) {
    int len = strlen(str);
    if (len > BUFSIZE - 2) {
        return 2;
    }

    char buf[BUFSIZE];
    strncpy(buf, str, BUFSIZE - 2);
    return announce_buf_to_client(client_fd, buf, len);
}

/* Print formatted string to stdout, and send network-newline string to a client.
 * Returns 0 on success, 1 on failed/incomplete write, 2 if the string
 * is too large, or -1 in case of error.
 */
int announce_fstr_to_client(int client_fd, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char msg[BUFSIZE + 1]; // vsnprintf will add a NULL terminator, so we need +1 byte
    vsnprintf(msg, BUFSIZE - 1, format, args); // need to make sure we have space for \r\n

    va_end(args);

    return announce_str_to_client(client_fd, msg);
}


/*
 *  Announcing to watchers. Remember: leave the watch feature to the end.
 */

/* Print message to stdout, and send network-newline message to a list of
 * clients. Returns 0 on success, 1 on failed/incomplete write, or -1 in
 * case of error.
 */
int announce_buf_to_watchers(WatcherList *watcher_list, char *buf, int buflen);

/* Print string to stdout, and send network-newline string to a list of
 * clients. Returns 0 on success, 1 on failed/incomplete write, 2 if the
 * string is too large, or -1 in case of error.
 */
int announce_str_to_watchers(WatcherList *watcher_list, char *str);

/* Print formatted string to stdout, and send network-newline string to a list of
 * clients. Returns 0 on success, 1 on failed/incomplete write, 2 if the string
 * is too large, or -1 in case of error.
 */
int announce_fstr_to_watchers(WatcherList *watcher_list, const char *format, ...);

/*
 *  Childcare
 */

void process_job_output(JobNode *job_node, int fd, Buffer *buffer, char *format);
int process_dead_children(JobList *job_list, fd_set *all_fds);
JobNode *process_dead_child(JobList *job_list, JobNode *dead_job, fd_set *all_fds);

/* Process output from each child, remove them if they are dead, announce to watchers.
 * Returns 1 if at least one child exists, 0 otherwise.
 */
int process_jobs(JobList *job_list, fd_set *current_fds, fd_set *all_fds) {
    if (job_list->first == NULL) {
        return 0;
    }

    for (JobNode *job = job_list->first; job != NULL; job = job->next) {
        if (FD_ISSET(job->stdout_fd, current_fds)) {
            process_job_output(job, job->stdout_fd, &(job->stdout_buffer), "[JOB %d] %s");
        }
        if (FD_ISSET(job->stderr_fd, current_fds)) {
            process_job_output(job, job->stderr_fd, &(job->stderr_buffer), "*(JOB %d)* %s");
        }
    }

    process_dead_children(job_list, all_fds);
    return 1;
}

/* Read characters from fd and store them in buffer. Announce each message found
 * to watchers of job_node with the given format, eg. "[JOB %d] %s\n".
 */
void process_job_output(JobNode *job_node, int fd, Buffer *buffer, char *format)
{
    if (read_to_buf(fd, buffer) < 0) {
        return;
    } 

    WatcherList *watchers = &(job_node->watcher_list);
    WatcherNode *first = watchers->first;

    int msg_len;
    char *msg;
    while ((msg = get_next_msg(buffer, &msg_len, NEWLINE_LF)) != NULL) {
        msg[msg_len - 1] = '\0';
        
        announce_fstr_to_client(first->client_fd, format, job_node->pid, msg);
    }

    if (is_buffer_full(buffer) && buffer->consumed == 0) {
        announce_fstr_to_client(first->client_fd, 
                        "*(SERVER)* Buffer from job %d is full. Aborting job.", 
                        job_node->pid);
        kill_job_node(job_node);
    }

    shift_buffer(buffer);
}

/* Remove all dead children from job list, announce to watchers.
 * Returns count of dead jobs removed.
 */
int process_dead_children(JobList *job_list, fd_set *all_fds) {
    int dead_children = 0;
  
    JobNode **tail = &(job_list->first);

    for (JobNode *job = job_list->first; job != NULL; job = *tail) {
        if (job->dead) {
            *tail = job->next;

            WatcherList *watchers = &(job->watcher_list);
            WatcherNode *first_watcher = watchers->first;
            
            if (WIFEXITED(job->wait_status)) {
                announce_fstr_to_client(first_watcher->client_fd, 
                        "[JOB %d] Exited with status %d", job->pid, 
                        WEXITSTATUS(job->wait_status));
            } else {
                announce_fstr_to_client(first_watcher->client_fd, 
                        "[Job %d] Exited due to signal.", job->pid);
            }

            FD_CLR(job->stdout_fd, all_fds);
            FD_CLR(job->stderr_fd, all_fds);
            delete_job_node(job);

            job_list->count--;
            dead_children++;
        } else {
            tail = &(job->next);
        }
    }

    return dead_children;
}

/* Remove the given child from the job list, announce to watchers.
 * Returns the next node that the job pointed to.
 */
JobNode *process_dead_child(JobList *job_list, JobNode *dead_job, fd_set *all_fds);

/*
 *  Misc
 */

/* Return the highest fd between all clients and job pipes.
 */
int get_highest_fd(int listen_fd, Client *clients, JobList *job_list) {
    int max = listen_fd;

    for (int i = 0; i < client_count; i++) {
        int client_socket = clients[i].socket_fd;
        if (client_socket > max) {
            max = client_socket;
        }
    }

    JobNode *current = job_list->first;
    for (int i = 0; i < job_list->count; i++) {
        if (current->stdout_fd > max) {
            max = current->stdout_fd;
        }

        if (current->stderr_fd > max) {
            max = current->stderr_fd;
        }

        current = current->next;
    }

    return max;
}

/* Frees up all memory and exits.
 */
void clean_exit(int listen_fd, Client *clients, JobList *job_list, int exit_status) {
    close(listen_fd);

    char msg[] = "[SERVER] Shutting down\r\n";
    for (int i = 0; i < client_count; i++) {
        int socket = clients[i].socket_fd;
        write_buf_to_client(socket, msg, sizeof(msg) - 1);
        close(socket);
    }
    char log[] = "[SERVER] Shutting down\n";
    write(STDOUT_FILENO, log, sizeof(log) - 1);

    //kill_all_jobs(job_list);
    //empty_job_list(job_list);

    exit(exit_status);
}

int main(void) {
    // Reset SIGINT received flag.
    sigint_received = 0;

    // This line causes stdout and stderr not to be buffered.
    // Don't change this! Necessary for autotesting.
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    // Set up SIGCHLD handler
    struct sigaction sigchld_act = {{sigchld_handler}};
    sigchld_act.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sigchld_act, NULL);
    
    // Set up SIGINT handler
    struct sigaction sigint_act = {{sigint_handler}};
    sigint_act.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sigint_act, NULL);

    // Set up server socket
    struct sockaddr_in *self = init_server_addr(PORT);
    int listen_fd = setup_server_socket(self, QUEUE_LENGTH);
    if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) == -1) {
        exit(1);
    }

    // Initialize client tracking structure (array list)
    Client clients[MAX_CLIENTS] = {0};

    // Initialize job tracking structure (linked list)
    
    // Set up fd set(s) that we want to pass to select()
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listen_fd, &readfds);

    int nfds = listen_fd + 1;
    
    while (!sigint_received) {
	    // Use select to wait on fds, also perform any necessary checks 
	    // for errors or received signals
        fd_set retread = readfds;
        if (select(nfds, &retread, NULL, NULL, NULL) > 0) {
            // Accept incoming connections
            if (client_count < MAX_CLIENTS && FD_ISSET(listen_fd, &retread)) {                  int new_fd = setup_new_client(listen_fd, clients);
                if (new_fd >= 0) {
                    FD_SET(new_fd, &readfds);
                    if (new_fd >= nfds) {
                        nfds = new_fd + 1;
                    }
                }
            }
            // Check our job pipes, update max_fd if we got children
            if (process_jobs(&job_list, &retread, &readfds) > 0) {
                nfds = get_highest_fd(listen_fd, clients, &job_list) + 1;
            }

            // Check on all the connected clients, process any requests
	    // or deal with any dead connections etc.
            for (int i = 0; i < client_count; i++) {
                if (FD_ISSET(clients[i].socket_fd, &retread)) {
                    int client_fd = process_client_request(clients + i, 
                                                           &job_list, &readfds);
                    if (client_fd > 0) {
                        nfds = remove_client(listen_fd, i, clients, &job_list)
                                                                            + 1;
                        FD_CLR(client_fd, &readfds);
           
                        char close_log[BUFSIZE + 1];
                        snprintf(close_log, BUFSIZE + 1, 
                                "[CLIENT %d] Connection closed", client_fd);
                        int len = strlen(close_log);
                        close_log[len] = '\n';
                        write(STDOUT_FILENO, close_log, len + 1);
                    } else {
                        nfds = get_highest_fd(listen_fd, clients, &job_list)
                                                                            + 1;
                    }
                }
            }
        } else if (errno == EINTR) {
            process_dead_children(&job_list, &readfds);
            nfds = get_highest_fd(listen_fd, clients, &job_list) + 1;
        }
    }

    clean_exit(listen_fd, clients, &job_list, 0);
    return 0;
}
