#include <unistd.h>
#include <string.h>

#include "jobprotocol.h"

/* Example: Something like the function below might be useful

// Find and return the location of the first newline character in a string
// First argument is a string, second argument is the length of the string
int find_newline(const char *buf, int len);
*/ 

JobCommand get_job_command(char* str) {
    if (strncmp(str, "jobs", BUFSIZE + 1) == 0) {
        return CMD_LISTJOBS;
    }
    if (strncmp(str, "run", BUFSIZE + 1) == 0) {
        return CMD_RUNJOB;
    }
    if (strncmp(str, "kill", BUFSIZE + 1) == 0) {
        return CMD_KILLJOB;
    }
    if (strncmp(str, "watch", BUFSIZE + 1) == 0) {
        return CMD_WATCHJOB;
    }
    if (strncmp(str, "exit", BUFSIZE + 1) == 0) {
        return CMD_EXIT;
    }

    return CMD_INVALID;
}

int find_network_newline(const char *buf, int n) {
    for (int i = 0; i < n - 1; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return i + 2;
        }
    }
    
    return -1;
}

int find_unix_newline(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            return i + 1;
        }
    }
    
    return -1;
}

int read_to_buf(int fd, Buffer *buf) {
    int nbytes = read(fd, buf->buf + buf->inbuf, BUFSIZE - buf->inbuf);
    if (nbytes == -1) {
        return -1;
    }

    buf->inbuf += nbytes;

    return nbytes;
}

char* get_next_msg(Buffer* buf, int* len, NewlineType ntype) {
    char *start = buf->buf + buf->consumed;

    int length;
    switch (ntype) {
        case NEWLINE_CRLF:
            length = find_network_newline(start, buf->inbuf - buf->consumed);
            break;
        case NEWLINE_LF:
            length = find_unix_newline(start, buf->inbuf - buf->consumed);
            break;
        default:
            return NULL;
    }

    if (length < 0) {
        return NULL;
    }

    buf->consumed += length;
    *len = length;
    return start;
}

void shift_buffer(Buffer *buf) {
    int chars_left = buf->inbuf - buf->consumed;
    memmove(buf, buf + buf->consumed, chars_left);
    buf->inbuf = chars_left;
    buf->consumed = 0;
}

int is_buffer_full(Buffer *buf) {
    return buf->inbuf == BUFSIZE;
}
