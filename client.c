#define _GNU_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>
#include <syslog.h>
#include <libgen.h>

/* tiny list */
typedef struct tlist {
    ssize_t capacity;
    ssize_t count;
    ssize_t itemsize;
    char *items;
} tlist;

int tlist_init_checked(tlist *l, ssize_t itemsize, ssize_t capacity)
{
    l->capacity = capacity;
    l->count = 0;
    l->itemsize = itemsize;
    l->items = malloc(itemsize * capacity);
    if (!l->items) {
        return -1;
    }
    return 0;
}

tlist *tlist_new(ssize_t itemsize, ssize_t capacity)
{
    assert(capacity);
    tlist *l = malloc(sizeof(struct tlist));
    if (!l) {
        return NULL;
    }
    if (tlist_init_checked(l, itemsize, capacity)) {
        free(l);
        return NULL;
    }
    return l;
}

int tlist_append_checked(tlist *l, void *item)
{
    char *new_items = NULL;
    if (l->count > l->capacity >> 1) {
        new_items = realloc(l->items, l->itemsize * (l->capacity << 1));
        if (new_items) {
            l->items = new_items;
            l->capacity <<= 1;
        }
    }
    if (l->count >= l->capacity) {
        return -1;
    }
    memcpy(l->items + l->count * l->itemsize, item, l->itemsize);
    l->count++;
    return 0;
}

int tlist_remove_checked(tlist *l, ssize_t index)
{
    if (index >= l->count) {
        return -1;
    } else if (index + 1 < l->count) {
        memcpy(l->items + index * l->itemsize,
                l->items + (index + 1) * l->itemsize, (l->count - index - 1) * l->itemsize);
    }
    l->count--;
    return 0;
}

int tlist_range_remove_checked(tlist *l, ssize_t start, ssize_t end)
{
    if ((start > end) || (end >= l->count)) {
        return -1;
    } else if (end + 1 < l->count) {
        memcpy(l->items + start * l->itemsize,
                l->items + (end + 1) * l->itemsize, (l->count - end - 1) * l->itemsize);
    }
    l->count -= end - start + 1;
    return 0;
}

int tlist_get_checked(tlist *l, ssize_t index, void *item)
{
    if (index >= l->count) {
        return -1;
    }
    if (!item) {
        return -2;
    }
    memcpy(item, l->items + index * l->itemsize, l->itemsize);
    return 0;
}

void tlist_free(tlist *l)
{
    free(l->items);
    free(l);
}

static char *arg_server_addr = "0.0.0.0";
static unsigned short arg_server_port = 2023;

#define MAXFD 64
int daemon_init(const char *pname, int facility)
{
    int i;
    pid_t pid;

    if ( (pid = fork()) < 0)
        return (-1);
    else if (pid)
        _exit(0);   /* parent terminates */

    /* child 1 continues... */
    if (setsid() < 0)   /* become session leader */
        return (-1);

    signal(SIGHUP, SIG_IGN);
    if ( (pid = fork()) < 0)
        return (-1);
    else if (pid)
        _exit(0);   /* child 1 terminates */

    /* child 2 continues... */
    chdir("/");    /* change working directory */

    /* close off file descriptors */
    for (i = 0; i < MAXFD; i++)
        close(i);

    /* redirect stdin, stdout, and stderr to /dev/null */
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_RDWR);
    open("/dev/null", O_RDWR);

    openlog(pname, LOG_PID, facility);

    return (0);    /* success */
}

int tcpv4_connect(const char *addr, unsigned short port)
{
    int fd;
    int on = 1;
    struct sockaddr_in remote_addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &remote_addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&remote_addr, sizeof(remote_addr))) {
        close(fd);
        return -1;
    }

    return fd;
}

int tcpv4_listen(struct sockaddr *addr, socklen_t addrlen)
{
    int fd;
    int on = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    if (bind(fd, addr, addrlen) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN)) {
        close(fd);
        return -1;
    }

    return fd;
}

int fds_copy(int infd, int outfd)
{
    char buf [1024];
    ssize_t nwriten = 0;
    ssize_t n, nread;

    if ((nread = read(infd, buf, sizeof buf)) <= 0) {
        return nread;
    }

    while (nwriten < nread) {
        n = write(outfd, buf + nwriten, nread - nwriten);
        if (n < 0) {
            break;
        }
        nwriten += n;
    }
    return n;
}

void send_msg(int fd, int type, int id, char *data, ssize_t len)
{
    assert(len <= 1020);
    char msg[1024];
    msg[0] = (unsigned char)0x1;     /* version */
    msg[1] = (unsigned char)type;
    *(int *)&msg[2] = id; /* identify */
    memcpy(&msg[6], data, len);
    write(fd, msg, 6 + len);
}

int recv_msg(int fd, char *msg, size_t count)
{
    int n;
    while ((n = read(fd, msg, count)) < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            continue;
        } else {
            break;
        }
    }
    return n;
}

enum {
    /* to server */
    TYPE_LISTEN_SERVICES = 0x01,
    TYPE_BUILD_TUNNEL,
    /* from server */
    TYPE_ASSIGN_IDS,
    TYPE_CONNECT_SERVICE,
    /* error */
    TYPE_ERROR_GENERAL,
    TYPE_ERROR_CONNECT_SERVICE,
};

enum {
    SERV_PROTOCOL_TCP = 0x1,
    SERV_PROTOCOL_UDP = 0x2,
};

struct service {
    int id;
    int protocol;
    char clientaddr[16]; /* ipv4 */
    unsigned short clientport;
    char serveraddr[16]; /* ipv4 */
    unsigned short serverport;
};

struct client {
    int id;
    int fd;
    struct service *services;
    ssize_t services_count;
};

int parse_c_option(char *opt_str, struct service *service)
{
    const char delimiters[] = ":-";
    char *token, *cp;

    cp = strdupa(opt_str);
    memset(service, 0, sizeof(struct service));

    token = strsep(&cp, delimiters);
    if (!token) {
        return 1;
    }
    if (!strcmp(token, "tcp")) {
        service->protocol = SERV_PROTOCOL_TCP;
    } else {
        return 1;
    }

    token = strsep(&cp, delimiters);
    if (!token) {
        return 1;
    }
    if (*token) {
        memcpy(service->serveraddr, token, strlen(token));
    } else {
        memcpy(service->serveraddr, "0.0.0.0", sizeof "0.0.0.0");
    }

    token = strsep(&cp, delimiters);
    if (!token) {
        return 1;
    }
    if (*token) {
        service->serverport = atoi(token);
    } else {
        return 1;
    }

    token = strsep(&cp, delimiters);
    if (!token) {
        return 1;
    }
    if (*token) {
        memcpy(service->clientaddr, token, strlen(token));
    } else {
        memcpy(service->clientaddr, "0.0.0.0", sizeof "0.0.0.0");
    }

    token = strsep(&cp, delimiters);
    if (!token) {
        return 1;
    }
    if (*token) {
        service->clientport = atoi(token);
    } else {
        return 1;
    }

    if (cp) {
        return 1;
    }

    return 0;
}

int connect_server(struct client *c)
{
    int i, n;
    char buf[1024];
    int connfd;
    int *service_ids;
    struct service *services = c->services;
    int services_count = c->services_count;

    for (i = 0; i < services_count; i++) {
        *((unsigned short *)buf + i) = services[i].serverport;
    }

    connfd = tcpv4_connect(arg_server_addr, arg_server_port);
    if (connfd < 0) {
        return -1;
    }
    send_msg(connfd, TYPE_LISTEN_SERVICES, services_count, buf,
            services_count * sizeof (unsigned short));

    n = recv_msg(connfd, buf, sizeof buf);
    if (n <= 0) {
        close(connfd);
        return -1;
    }
    if (buf[0] != 0x01) {
        close(connfd);
        return -1;
    }
    if (buf[1] != TYPE_ASSIGN_IDS) {
        close(connfd);
        return -1;
    }
    c->id = *(int *)&buf[2];
    service_ids = (int *)&buf[6];
    for (i = 0; i < services_count; i++) {
        services[i].id = service_ids[i];
    }
    c->fd = connfd;

    return 0;
}

int pollfds_list_append_checked(tlist *pollfds, int fd, short events)
{
    struct pollfd pd;
    pd.fd = fd;
    pd.events = events;
    return tlist_append_checked(pollfds, &pd);
}

void pollfds(struct client *c)
{
    ssize_t i, j, ni;

    struct tlist *pollfds_list = tlist_new(sizeof(struct pollfd), 8);
    struct pollfd pd;
    struct pollfd **p = (void *)&pollfds_list->items;
    ssize_t *np = &pollfds_list->count;

    struct service *services = c->services;
    int services_count = c->services_count;
    int service_listenid, service_connid;
    int remote_connfd, local_connfd;

    int n;
    char buf[1024];

    /* main fd */
    pollfds_list_append_checked(pollfds_list, c->fd, POLLIN);

    while (1) {
loop_begin:
        switch(poll(*p, *np, -1)) {
        case 0:
            break;
        case -1:
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                break;
            }
        }

        ni = 1;
        if ((*p)[0].revents & POLLIN) {
            n = recv_msg(c->fd, buf, 10);
            if (n <= 0) {
                break;
            }
            if (buf[0] != 0x01) {
                break;
            }
            if (buf[1] != TYPE_CONNECT_SERVICE) {
                break;
            }
            service_listenid = *(int *)&buf[2];
            service_connid = *(int *)&buf[6];
            for (i = 0; i < services_count; i++) {
                if (services[i].id == service_listenid) {
                    local_connfd = tcpv4_connect("0.0.0.0", services[i].clientport);
                    if (local_connfd < 0) {
                        send_msg(c->fd, TYPE_ERROR_CONNECT_SERVICE, service_listenid,
                                (void *)&service_connid, sizeof(int));
                        goto loop_begin;
                    }
                    break;
                }
            }
            remote_connfd = tcpv4_connect(arg_server_addr, arg_server_port);
            if (remote_connfd < 0) {
                break;
            }
            pollfds_list_append_checked(pollfds_list, local_connfd, POLLIN);
            pollfds_list_append_checked(pollfds_list, remote_connfd, POLLIN);
            /* build a data tunnel */
            send_msg(remote_connfd, TYPE_BUILD_TUNNEL, c->id, (void *)&service_connid, sizeof(int));
        }

        i = ni;
        for (; i < *np; i++) {
            if ((*p)[i].revents & POLLIN) {
                j = ((i + ni) ^ 1) - ni;
                if (fds_copy((*p)[i].fd, (*p)[j].fd) <= 0) {
                    i = ((i + ni) & -2) - ni;
                    close((*p)[i].fd);
                    close((*p)[i + 1].fd);
                    tlist_range_remove_checked(pollfds_list, i, i + 1);
                    i--;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    struct client client, *c = &client;
    int ret;

    struct service s;
    tlist *services_list = tlist_new(sizeof (struct service), 8);

    int ch;
    while ((ch = getopt(argc, argv, "c:dhp:P:4:")) != -1) {
        switch (ch) {
            /* configure */
            case 'c':
                /* "tcp::60022-127.0.0.1:22" */
                ret = parse_c_option(optarg, &s);
                if (ret) {
                    return 1;
                }
                tlist_append_checked(services_list, &s);
                break;
            /* daemon */
            case 'd':
                daemon_init(argv[0], 0);
                break;
            /* server ipv4 address */
            case '4':
                arg_server_addr = strdup(optarg);
                break;
            /* server port */
            case 'p':
                arg_server_port = atoi(optarg);
                break;
            /* help */
            case 'h':
            case '?':
                fprintf(stderr, "Usage: %s [-dh] [-4 serv_addr] [-p serv_port] -c tcp::serv_port-[local_addr]:local_port [-c ...]\n", basename(argv[0]));
                return 1;
        }
    }

    if (services_list->count == 0) {
        return 1;
    }
    c->services = (void *)services_list->items;
    c->services_count = services_list->count;

    if (connect_server(c) < 0) {
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    pollfds(c);

    return 0;
}
