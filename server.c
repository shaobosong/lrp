#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <libgen.h>
#include <fcntl.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>

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

/* global */
tlist *g_threads_list;
pthread_mutex_t g_lock;

static const char *arg_addr = "0.0.0.0";
static char *arg_remote_port = "2023";

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

int tcp_listen(const char *hostname, const char *service, socklen_t *addrlenp)
{
    int err;
    int listenfd = -1;
    struct addrinfo *ainfo, *ai;
    int on = 1;

    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = 0,
    };

    if ((err = getaddrinfo(hostname, service, &hints, &ainfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    ai = ainfo;
    do {
        if ((listenfd = socket(ai->ai_family, ai->ai_socktype, 0)) == -1) {
            continue;
        }
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        if (bind(listenfd, ai->ai_addr, ai->ai_addrlen) == -1) {
            close(listenfd);
            listenfd = -1;
            continue;
        }
        break;
    } while ((ai = ai->ai_next) != NULL);

    if (listenfd == -1) {
        return -1;
    }

    if (listen(listenfd, SOMAXCONN) == -1) {
        close(listenfd);
        return -1;
    }

    if (addrlenp) {
        *addrlenp = ai->ai_addrlen;
    }

    freeaddrinfo(ainfo);
    return listenfd;
}

enum {
    /* from client */
    TYPE_LISTEN_SERVICES = 0x01,
    TYPE_BUILD_TUNNEL,
    /* to client */
    TYPE_ASSIGN_IDS,
    TYPE_CONNECT_SERVICE,
    /* error */
    TYPE_ERROR_GENERAL,
    TYPE_ERROR_CONNECT_SERVICE,
};

enum {
    STATUS_INIT = 0x1,
    STATUS_LOOP,
    STATUS_DONE,
};

struct client_tunnel {
    int id;
    int fd;
    tlist *ids_list;
    tlist *pollfds_list;
};

struct thread {
    pthread_t thread;
    pthread_mutex_t lock;
    struct client_tunnel client_tunnel;
    volatile int status;
};

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

ssize_t send_msg(int fd, int type, int id, char *data, ssize_t len)
{
    assert(len <= 1020);
    char msg[1024];
    msg[0] = (unsigned char)0x1;     /* version */
    msg[1] = (unsigned char)type;
    *(int *)&msg[2] = id; /* identify */
    memcpy(&msg[6], data, len);
    return write(fd, msg, 6 + len);
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

int ids_list_append_checked(tlist *ids_list, int id)
{
    return tlist_append_checked(ids_list, &id);
}

int pollfds_list_append_checked(tlist *pollfds_list, int fd, short events)
{
    struct pollfd pd;
    pd.fd = fd;
    pd.events = events;
    return tlist_append_checked(pollfds_list, &pd);
}

int service_append_checked(struct client_tunnel *client, int id, int fd, short events)
{
    int ret;
    if ((ret = ids_list_append_checked(client->ids_list, id)) < 0) {
        return ret;
    }
    if ((ret = pollfds_list_append_checked(client->pollfds_list, fd, events)) < 0) {
        return ret;
    }
    return 0;
}

int service_range_remove_checked(struct client_tunnel *client, int start, int end)
{
    int ret;
    if ((ret = tlist_range_remove_checked(client->ids_list, start, end)) < 0) {
        return ret;
    }
    if ((ret = tlist_range_remove_checked(client->pollfds_list, start, end)) < 0) {
        return ret;
    }
    return 0;
}

void client_poll(struct thread *t, struct client_tunnel *c, char *msg)
{
    ssize_t i, ni, j;
    int nservices;
    unsigned short *services;
    char service_buf[8];
    int fd;
    char buf[1024];
    int client_id, service_id;
    int id = 0;
    int **ids;
    ssize_t nids;
    struct pollfd **pollfds;
    ssize_t *npollfds;

    c->ids_list = tlist_new(sizeof(int), 8);
    c->pollfds_list = tlist_new(sizeof(struct pollfd), 8);
    service_append_checked(c, id++, c->fd, POLLIN);

    nservices = *(int *)&msg[2];
    if (nservices == 0) {
        /* invalid connection */
        goto done;
    }
    services = (unsigned short *)&msg[6];
    for (i = 0; i < nservices; i++) {
        memset(service_buf, 0, sizeof service_buf);
        sprintf(service_buf, "%u", services[i]);
        if ((fd = tcp_listen(arg_addr, service_buf, NULL)) < 0) {
            /* services startup failed */
            goto done;
        }
        *((int *)buf + i) = id; /* assign a id */
        service_append_checked(c, id++, fd, POLLIN);
    }
    send_msg(c->fd, TYPE_ASSIGN_IDS, c->id, buf, nservices * sizeof(int));

    ids = (void *)&c->ids_list->items;
    pollfds = (void *)&c->pollfds_list->items;
    npollfds = &c->pollfds_list->count;
    t->status = STATUS_LOOP;

    pthread_mutex_lock(&t->lock);
    while (1) {
        pthread_mutex_unlock(&t->lock);
        if (poll(*pollfds, *npollfds, -1) == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                goto done;
            }
        }
        pthread_mutex_lock(&t->lock);

        if ((*pollfds)[0].revents & POLLIN) {
            if (recv_msg((*pollfds)[0].fd, buf, sizeof buf) <= 0) {
                goto done;
            }
            switch (t->status) {
            case STATUS_LOOP:
                /* check version */
                if (buf[0] != 0x01) {
                    break;
                }
                switch(buf[1]) {
                case TYPE_ERROR_CONNECT_SERVICE:
                    service_id = *(int *)&buf[6];
                    for (j = 0; j < *npollfds; j++) {
                        if ((*ids)[j] == service_id) {
                            break;
                        }
                    }
                    if (j == *npollfds) {
                        break;
                    }
                    j = ((j + ni) & -2) - ni;
                    close((*pollfds)[j].fd);
                    service_range_remove_checked(c, j, j + 1);
                    break;
                }
                break;
            default:
                assert(0);
            }
        }

        i = 1;
        ni = 1 + nservices;
        for (; i < ni; i++) {
            if ((*pollfds)[i].revents & POLLIN) {
                if ((fd = accept((*pollfds)[i].fd, NULL, NULL)) < 0) {
                    continue;
                }
                /* add a data fd  */
                service_append_checked(c, id++, fd, POLLIN);

                /* add a dummy data fd */
                *(int *)(buf) = id;
                service_append_checked(c, id++, -1, POLLIN);

                /* request a real data fd */
                send_msg((*pollfds)[0].fd, TYPE_CONNECT_SERVICE, (*ids)[i], buf, sizeof(int));
            }
        }

        i = ni;
        for (; i < *npollfds; i++) {
            if ((*pollfds)[i].revents & POLLIN) {
                j = ((i + ni) ^ 1) - ni;
                if ((*pollfds)[j].fd == -1) {
                    continue;
                }
                if (fds_copy((*pollfds)[i].fd, (*pollfds)[j].fd) <= 0) {
                    i = ((i + ni) & -2) - ni;
                    close((*pollfds)[i].fd);
                    close((*pollfds)[i + 1].fd);
                    service_range_remove_checked(c, i, i + 1);
                    i--;
                }
            }
        }
    }
done:
    for (i = 0; i < *npollfds; i++) {
        close((*pollfds)[i].fd);
    }
    tlist_free(c->ids_list);
    tlist_free(c->pollfds_list);
    t->status = STATUS_DONE;
    pthread_mutex_unlock(&t->lock);
}

void tunnel_build(struct thread *thr, struct client_tunnel *tun, char *msg)
{
    ssize_t n, m;
    int client_id = *(int *)&msg[2];
    int service_id = *(int *)&msg[6];
    struct thread **threads;
    ssize_t nthreads;
    int *ids;
    ssize_t nids;
    struct pollfd *pollfds;

    pthread_mutex_lock(&g_lock);
    threads = (void *)g_threads_list->items;
    nthreads = g_threads_list->count;
    for (n = 0; n < nthreads; n++) {
        if (threads[n]->client_tunnel.id == client_id) {
            break;
        }
    }
    if (n == nthreads) {
        close(tun->fd);
        goto client_removed;
    }

    pthread_mutex_lock(&threads[n]->lock);
    if (threads[n]->status == STATUS_DONE) {
        close(tun->fd);
        goto client_done;
    }

    ids = (void *)threads[n]->client_tunnel.ids_list->items;
    nids = threads[n]->client_tunnel.ids_list->count;
    for (m = 0; m < nids; m++) {
        if (ids[m] == service_id) {
            break;
        }
    }
    assert (m < nids);
    pollfds = (void *)threads[n]->client_tunnel.pollfds_list->items;
    pollfds[m].fd = tun->fd;
client_done:
    pthread_mutex_unlock(&threads[n]->lock);
client_removed:
    pthread_mutex_unlock(&g_lock);
    thr->status = STATUS_DONE;
}

void *client_tunnel_thread(void *arg)
{
    struct thread *t = arg;
    struct client_tunnel *ct = &t->client_tunnel;
    char buf[1024];

    if (recv_msg(ct->fd, buf, sizeof buf) <= 0) {
        close(ct->fd);
        t->status = STATUS_DONE;
        return 0;
    }

    /* check version */
    if (buf[0] != 0x01) {
        close(ct->fd);
        t->status = STATUS_DONE;
        return 0;
    }

    switch(buf[1]) {
    case TYPE_LISTEN_SERVICES:
        client_poll(t, ct, buf);
        break;
    case TYPE_BUILD_TUNNEL:
        tunnel_build(t, ct, buf);
        break;
    default:
        assert(0);
    }

    return 0;
}

void threads_list_collect(tlist *tl)
{
    ssize_t i;
    ssize_t *nt = &tl->count;
    struct thread *t;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < *nt; i++) {
        tlist_get_checked(tl, i, &t);
        if (t->status == STATUS_DONE) {
            pthread_join(t->thread, 0);
            pthread_mutex_destroy(&t->lock);
            free(t);
            tlist_remove_checked(tl, i);
            i--;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void remote_wait_connect(int listenfd)
{
    int connfd;
    struct thread *t;
    int client_id = 0;
    pthread_attr_t attr, *a;

    if (pthread_mutex_init(&g_lock, NULL)) {
        return;
    }
    g_threads_list = tlist_new(sizeof(struct thread *), 8);
    if (!g_threads_list) {
        return;
    }
    while (1) {
        a = NULL;
        threads_list_collect(g_threads_list);
        if ((connfd = accept(listenfd, NULL, NULL)) < 0) {
            continue;
        }
        t = malloc(sizeof(struct thread));
        if (!t) {
            close(connfd);
            continue;
        }
        t->status = STATUS_INIT;
        t->client_tunnel.id = client_id++;
        t->client_tunnel.fd = connfd;
        t->client_tunnel.ids_list = NULL;
        t->client_tunnel.pollfds_list = NULL;
        if (pthread_mutex_init(&t->lock, NULL)) {
            free(t);
            close(connfd);
            continue;
        }
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_setstacksize(&attr, 32 * 1024) == 0) {
                a = &attr;
            }
        }
        if (pthread_create(&t->thread, a, client_tunnel_thread, t)) {
            pthread_mutex_destroy(&t->lock);
            if (a) {
                pthread_attr_destroy(&attr);
            }
            free(t);
            close(connfd);
            continue;
        }
        if (a) {
            pthread_attr_destroy(&attr);
        }
        tlist_append_checked(g_threads_list, &t);
    }
}

int main(int argc, char **argv)
{
    int listenfd;

    int ch;
    while ((ch = getopt(argc, argv, "dhp:")) != -1) {
        switch (ch) {
            /* daemon */
            case 'd':
                daemon_init(argv[0], 0);
                break;
            /* server listen port */
            case 'p':
                arg_remote_port = strdup(optarg);
                break;
            /* help */
            case 'h':
            case '?':
                fprintf(stderr, "Usage: %s [-dh] [-p port]\n", basename(argv[0]));
                return 1;
        }
    }

    if ((listenfd = tcp_listen(arg_addr, arg_remote_port, NULL)) < 0) {
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    remote_wait_connect(listenfd);

    return 0;
}

