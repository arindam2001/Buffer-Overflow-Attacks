#include "http.h"
#include <sys/param.h>
#ifndef BSD
#include <sys/sendfile.h>
#endif
#include <sys/uio.h>
#include <sys/stat.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Maximum size for the environment buffer used in http_request_line */
#define ENV_MAX 8192

/*
 * safe_append() appends a formatted string (plus a terminating '\0')
 * into the environment buffer. It checks for available space.
 */
static int safe_append(char **dst, size_t *remaining, const char *fmt, ...)
{
    if (*remaining <= 1) // Ensure there is always space for '\0'
        return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*dst, *remaining, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= *remaining)
    {
        *remaining = 0; // Prevent further writing if overflow happens
        return -1;
    }

    *dst += n;
    *remaining -= n;

    // Ensure null-termination
    if (*remaining > 0)
    {
        **dst = '\0';
        (*dst)++;
        (*remaining)--;
    }

    return 0;
}

/*
 * Reads one line (up to size-1 bytes) from fd into buf.
 * Skips CR characters and stops on LF.
 * Returns 0 on success, -1 on error or if the line is too long.
 */
int http_read_line(int fd, char *buf, size_t size)
{
    size_t i = 0;
    char ch;

    while (i < size - 1)
    {
        int cc = read(fd, &ch, 1);
        if (cc <= 0)
            break;
        if (ch == '\r')
            continue; /* Skip carriage return */
        if (ch == '\n')
        {
            buf[i] = '\0';
            return 0;
        }
        buf[i++] = ch;
    }
    if (i >= size - 1)
    {
        buf[size - 1] = '\0';
        return -1; /* line too long */
    }
    return (i > 0) ? 0 : -1;
}

/*
 * Decodes URL-encoded strings.
 */
void url_decode(char *dst, const char *src)
{
    for (;;)
    {
        if (src[0] == '%' && src[1] && src[2])
        {
            char hexbuf[3];
            hexbuf[0] = src[1];
            hexbuf[1] = src[2];
            hexbuf[2] = '\0';
            *dst = (char)strtol(hexbuf, NULL, 16);
            src += 3;
        }
        else if (src[0] == '+')
        {
            *dst = ' ';
            src++;
        }
        else
        {
            *dst = *src;
            src++;
            if (*dst == '\0')
                break;
        }
        dst++;
    }
}

/*
 * For lab 2: Do not remove this line.
 */
void touch(const char *name)
{
    if (access("/tmp/grading", F_OK) < 0)
        return;

    char pn[1024];
    snprintf(pn, sizeof(pn), "/tmp/%s", name);
    int fd = open(pn, O_RDWR | O_CREAT | O_NOFOLLOW, 0666);
    if (fd >= 0)
        close(fd);
}

/*
 * Parses the request line from fd.
 * Fills in reqpath with the URL-decoded path.
 * Builds an environment string (separated by '\0') in env.
 * Sets *env_len to the total length (including the final '\0').
 * Returns NULL on success or an error string.
 *
 * The parsing order is:
 *   1. Read the entire request line into a fixed buffer.
 *   2. Verify the request is not overly long.
 *   3. Parse the method (e.g., "GET") and ensure the path begins with '/'.
 *   4. Extract and process the query string (if any) from the path.
 *   5. Parse the protocol version.
 *   6. Append environment variables safely.
 */
const char *http_request_line(int fd, char *reqpath, char *env, size_t *env_len)
{
    static char buf[8192]; /* static buffer (not on the stack) */
    char *sp1, *sp2, *qp;
    char *envp = env;
    size_t remaining = ENV_MAX;

    /* For lab 2: don't remove this line. */
    touch("http_request_line");

    if (http_read_line(fd, buf, sizeof(buf)) < 0)
        return "Socket IO error";

    /* Reject overly long request lines */
    if (strlen(buf) >= sizeof(buf) - 1)
        return "HTTP request line too long";

    /* Parse request like "GET /foo.html HTTP/1.0" */
    sp1 = strchr(buf, ' ');
    if (!sp1)
        return "Cannot parse HTTP request (1)";
    *sp1 = '\0';
    sp1++;
    if (*sp1 != '/')
        return "Bad request path";

    /* If there's a query string, handle it now */
    if ((qp = strchr(sp1, '?')))
    {
        *qp = '\0';
        size_t qlen = strlen(qp + 1);
        if (qlen >= remaining)
        {
            http_err(fd, 400, "Environment buffer overflow");
            return NULL;
        }
        if (safe_append(&envp, &remaining, "QUERY_STRING=%s", qp + 1) < 0)
        {
            http_err(fd, 400, "Environment buffer overflow");
            return NULL;
        }
    }

    sp2 = strchr(sp1, ' ');
    if (!sp2)
        return "Malformed HTTP request: Missing protocol version";
    *sp2 = '\0';
    sp2++;

    /* Only support GET and POST requests */
    if (strcmp(buf, "GET") && strcmp(buf, "POST"))
        return "Unsupported request (not GET or POST)";

    if (safe_append(&envp, &remaining, "REQUEST_METHOD=%s", buf) < 0)
        return "Environment buffer overflow";
    if (safe_append(&envp, &remaining, "SERVER_PROTOCOL=%s", sp2) < 0)
        return "Environment buffer overflow";

    /* Append the REQUEST_URI (decoded) and SERVER_NAME */
    url_decode(reqpath, sp1);
    if (safe_append(&envp, &remaining, "REQUEST_URI=%s", reqpath) < 0)
        return "Environment buffer overflow";
    if (safe_append(&envp, &remaining, "SERVER_NAME=zoobar.org") < 0)
        return "Environment buffer overflow";

    *env_len = envp - env + 1;
    return NULL;
}

/*
 * Reads and parses HTTP headers from fd.
 * For each header of the form "Header-Name: value", the name is uppercased,
 * hyphens replaced with underscores, and stored as an environment variable.
 * Returns 0 on success or an error message.
 */
const char *http_request_headers(int fd)
{
    static char buf[8192]; /* static buffer (not on the stack) */
    int i;
    char value[512];
    char envvar[512];

    /* For lab 2: don't remove this line. */
    touch("http_request_headers");

    for (;;)
    {
        if (http_read_line(fd, buf, sizeof(buf)) < 0)
            return "Socket IO error";

        if (buf[0] == '\0') /* end of headers */
            break;

        /* Expect headers like "Cookie: foo bar" */
        char *sp = strchr(buf, ' ');
        if (!sp)
            return "Header parse error (1)";
        *sp = '\0';
        sp++;

        if (strlen(buf) == 0)
            return "Header parse error (2)";
        char *colon = &buf[strlen(buf) - 1];
        if (*colon != ':')
            return "Header parse error (3)";
        *colon = '\0';

        /* Uppercase header name and replace '-' with '_' */
        for (i = 0; i < (int)strlen(buf); i++)
        {
            buf[i] = toupper((unsigned char)buf[i]);
            if (buf[i] == '-')
                buf[i] = '_';
        }

        url_decode(value, sp);

        if (strcmp(buf, "CONTENT_TYPE") != 0 &&
            strcmp(buf, "CONTENT_LENGTH") != 0)
        {
            snprintf(envvar, sizeof(envvar), "HTTP_%s", buf);
            setenv(envvar, value, 1);
        }
        else
        {
            setenv(buf, value, 1);
        }
    }
    return 0;
}

/*
 * Sends an HTTP error response.
 * Formats a short HTML message, logs a warning, and closes fd.
 */
void http_err(int fd, int code, char *fmt, ...)
{
    fdprintf(fd, "HTTP/1.0 %d Error\r\n", code);
    fdprintf(fd, "Content-Type: text/html\r\n");
    fdprintf(fd, "\r\n");
    fdprintf(fd, "<H1>An error occurred</H1>\r\n");

    char *msg = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&msg, fmt, ap) < 0)
        msg = NULL;
    va_end(ap);

    if (msg)
    {
        fdprintf(fd, "%s\n", msg);
        warnx("[%d] Request failed: %s", getpid(), msg);
        free(msg);
    }
    else
    {
        warnx("[%d] Request failed: (error message formatting failed)", getpid());
    }
    close(fd);
}

/*
 * Removes any ".." sequences from pn to mitigate path traversal.
 */
static void remove_dotdot(char *pn)
{
    char *p;
    while ((p = strstr(pn, "..")) != NULL)
    {
        p[0] = '_';
        p[1] = '_';
    }
}

/*
 * Splits pn into script name and PATH_INFO.
 * Also sets the SCRIPT_FILENAME and SCRIPT_NAME environment variables.
 */
void split_path(char *pn)
{
    remove_dotdot(pn); /* mitigate path traversal */

    struct stat st;
    char *slash = NULL;
    for (;;)
    {
        int r = stat(pn, &st);
        if (r < 0)
        {
            if (errno != ENOTDIR && errno != ENOENT)
                break;
        }
        else
        {
            if (S_ISREG(st.st_mode))
                break;
        }

        if (slash)
            *slash = '/';
        else
            slash = pn + strlen(pn);

        while (--slash > pn)
        {
            if (*slash == '/')
            {
                *slash = '\0';
                break;
            }
        }

        if (slash == pn)
        {
            slash = NULL;
            break;
        }
    }

    if (slash)
    {
        *slash = '/';
        setenv("PATH_INFO", slash, 1);
        *slash = '\0';
    }
    const char *docroot = getenv("DOCUMENT_ROOT");
    if (docroot == NULL)
        docroot = "";
    setenv("SCRIPT_NAME", pn + strlen(docroot), 1);
    setenv("SCRIPT_FILENAME", pn, 1);
}

/*
 * Serves a request by mapping the URL to a file.
 * Determines whether to run a CGI script, serve a directory index, or serve a static file.
 */
void http_serve(int fd, const char *name)
{
    void (*handler)(int, const char *) = http_serve_none;
    char pn[1024];
    struct stat st;

    if (getcwd(pn, sizeof(pn)) == NULL)
    {
        http_err(fd, 500, "getcwd failed: %s", strerror(errno));
        return;
    }
    setenv("DOCUMENT_ROOT", pn, 1);

    if (strlen(name) + strlen(pn) + 1 >= sizeof(pn))
    {
        http_err(fd, 500, "Request too long");
        return;
    }
    strncat(pn, name, sizeof(pn) - strlen(pn) - 1);
    split_path(pn);

    if (!stat(pn, &st))
    {
        if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR))
            handler = http_serve_executable;
        else if (S_ISDIR(st.st_mode))
            handler = http_serve_directory;
        else
            handler = http_serve_file;
    }
    handler(fd, pn);
}

void http_serve_none(int fd, const char *pn)
{
    http_err(fd, 404, "File does not exist: %s", pn);
}

/*
 * Serves a static file.
 * Uses sendfile() to transfer the file if possible.
 */
void http_serve_file(int fd, const char *pn)
{
    int filefd;
    off_t len = 0;

    if (getenv("PATH_INFO"))
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s%s", pn, getenv("PATH_INFO"));
        http_serve_none(fd, buf);
        return;
    }
    filefd = open(pn, O_RDONLY);
    if (filefd < 0)
        return http_err(fd, 500, "open %s: %s", pn, strerror(errno));

    const char *ext = strrchr(pn, '.');
    const char *mimetype = "text/html";
    if (ext && strcmp(ext, ".css") == 0)
        mimetype = "text/css";
    else if (ext && strcmp(ext, ".jpg") == 0)
        mimetype = "image/jpeg";

    fdprintf(fd, "HTTP/1.0 200 OK\r\n");
    fdprintf(fd, "Content-Type: %s\r\n", mimetype);
    fdprintf(fd, "\r\n");

#ifndef BSD
    struct stat st;
    if (!fstat(filefd, &st))
        len = st.st_size;
    if (sendfile(fd, filefd, 0, len) < 0)
#else
    if (sendfile(filefd, fd, 0, &len, 0, 0) < 0)
#endif
        err(1, "sendfile");
    close(filefd);
}

/*
 * Joins a directory name and a filename into dst safely.
 */
void dir_join(char *dst, const char *dirname, const char *filename)
{
    strcpy(dst, dirname);
    if (dst[strlen(dst) - 1] != '/')
        strncat(dst, "/", 1024 - strlen(dst) - 1);
    strncat(dst, filename, 1024 - strlen(dst) - 1);
}

/*
 * Serves a directory by looking for index files.
 */
void http_serve_directory(int fd, const char *pn)
{
    static const char *const indices[] = {"index.html", "index.php", "index.cgi", NULL};
    char name[1024];
    struct stat st;
    int i;

    for (i = 0; indices[i]; i++)
    {
        dir_join(name, pn, indices[i]);
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode))
        {
            dir_join(name, getenv("SCRIPT_NAME"), indices[i]);
            break;
        }
    }
    if (indices[i] == NULL)
    {
        http_err(fd, 403, "No index file in %s", pn);
        return;
    }
    http_serve(fd, name);
}

/*
 * Serves a CGI executable.
 * Creates a pipe for reading the script’s output, forks, and execs the script.
 * Also sends proper HTTP headers.
 */
void http_serve_executable(int fd, const char *pn)
{
    char buf[1024], headers[4096];
    char *pheaders = headers;
    int pipefd[2], statusprinted = 0, ret, headerslen = sizeof(headers);

    if (pipe(pipefd) < 0)
    {
        http_err(fd, 500, "pipe: %s", strerror(errno));
        return;
    }
    switch (fork())
    {
    case -1:
        http_err(fd, 500, "fork: %s", strerror(errno));
        return;
    case 0:
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        dup2(fd, 0);
        close(fd);
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(pn, pn, NULL);
        http_err(1, 500, "execl %s: %s", pn, strerror(errno));
        _exit(1);
    default:
        close(pipefd[1]);
        while (1)
        {
            if (http_read_line(pipefd[0], buf, sizeof(buf)) < 0)
            {
                http_err(fd, 500, "Premature end of script headers");
                close(pipefd[0]);
                return;
            }
            if (!*buf)
                break;
            if (!statusprinted && strncasecmp("Status: ", buf, 8) == 0)
            {
                fdprintf(fd, "HTTP/1.0 %s\r\n%s", buf + 8, headers);
                statusprinted = 1;
            }
            else if (statusprinted)
            {
                fdprintf(fd, "%s\r\n", buf);
            }
            else
            {
                ret = snprintf(pheaders, headerslen, "%s\r\n", buf);
                if (ret < 0 || ret >= headerslen)
                {
                    http_err(fd, 500, "Too many script headers");
                    close(pipefd[0]);
                    return;
                }
                pheaders += ret;
                headerslen -= ret;
            }
        }
        if (statusprinted)
            fdprintf(fd, "\r\n");
        else
            fdprintf(fd, "HTTP/1.0 200 OK\r\n%s\r\n", headers);
        while ((ret = read(pipefd[0], buf, sizeof(buf))) > 0)
        {
            write(fd, buf, ret);
        }
        close(fd);
        close(pipefd[0]);
        while (waitpid(-1, NULL, WNOHANG) > 0)
        {
        }
    }
}

/*
 * Deserializes an environment block into environment variables.
 */
void env_deserialize(const char *env, size_t len)
{
    for (;;)
    {
        char *p = strchr((char *)env, '=');
        if (p == 0 || (size_t)(p - env) > len)
            break;
        *p++ = '\0';
        setenv(env, p, 1);
        p += strlen(p) + 1;
        len -= (size_t)(p - env);
        env = p;
    }
    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
    setenv("REDIRECT_STATUS", "200", 1);
}

/*
 * fdprintf() is a convenience function to print formatted output to a file descriptor.
 */
void fdprintf(int fd, char *fmt, ...)
{
    char *s = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0)
    {
        va_end(ap);
        return;
    }
    va_end(ap);
    write(fd, s, strlen(s));
    free(s);
}

/*
 * sendfd() and recvfd() pass file descriptors between processes.
 */
ssize_t sendfd(int socket, const void *buffer, size_t length, int fd)
{
    struct iovec iov = {(void *)buffer, length};
    char buf[CMSG_LEN(sizeof(int))];
    struct cmsghdr *cmsg = (struct cmsghdr *)buf;
    ssize_t r;
    cmsg->cmsg_len = sizeof(buf);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    *((int *)CMSG_DATA(cmsg)) = fd;
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;
    r = sendmsg(socket, &msg, 0);
    if (r < 0)
        warn("sendmsg");
    return r;
}

ssize_t recvfd(int socket, void *buffer, size_t length, int *fd)
{
    struct iovec iov = {buffer, length};
    char buf[CMSG_LEN(sizeof(int))];
    struct cmsghdr *cmsg = (struct cmsghdr *)buf;
    ssize_t r;
    cmsg->cmsg_len = sizeof(buf);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;
again:
    r = recvmsg(socket, &msg, 0);
    if (r < 0 && errno == EINTR)
        goto again;
    if (r < 0)
        warn("recvmsg");
    else
        *fd = *((int *)CMSG_DATA(cmsg));
    return r;
}