#include <unistd.h>

#include "jobprotocol.h"

/* Example: Something like the function below might be useful

// Find and return the location of the first newline character in a string
// First argument is a string, second argument is the length of the string
int find_newline(const char *buf, int len);
*/ 

int read_to_buf(int fd, Buffer *buf) {
    int nbytes = read(fd, buf->buf + buf->inbuf, BUFSIZE - buf->inbuf);
    if (nbytes == -1) {
    return -1;
    }

    buf->inbuf += nbytes;
    buf->consumed = nbytes;

    return nbytes;
}

int is_buffer_full(Buffer *buf) {
    return buf->inbuf == BUFSIZE;
}
