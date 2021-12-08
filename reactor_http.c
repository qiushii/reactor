/*************************************************************************
> File Name: reactor_singlecb.c
> Author: 
> Mail: 
> Created Time: 2021年12月03日 星期五 22时51分25秒
************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/sendfile.h>

#define HTTP_METHOD_ROOT    "html"

#define MAX_BUFLEN          4096
#define MAX_EPOLLSIZE       1024
#define MAX_EPOLL_EVENTS    1024

#define HTTP_METHOD_GET     0
#define HTTP_METHOD_POST    1

typedef int (*NCALLBACK)(int, int, void*);

struct qsevent{
    int fd;
    int events;
    int status;
    void *arg;
    long last_active;

    int (*callback)(int fd, int event, void *arg);
    unsigned char buffer[MAX_BUFLEN];
    int length;

    /*http param*/
    int method;
    char resource[MAX_BUFLEN];
    int ret_code;
};

struct qseventblock{
    struct qsevent *eventsarrry;
    struct qseventblock *next;
};

struct qsreactor{
    int epfd;
    int blkcnt;
    struct qseventblock *evblk;
};

int recv_cb(int fd, int events, void *arg);
int send_cb(int fd, int events, void *arg);
struct qsevent *qsreactor_idx(struct qsreactor *reactor, int sockfd);

int readline(char *allbuf, int idx, char *linebuf)
{
    int len = strlen(allbuf);    
    for( ; idx<len; idx++ )
    {
        if(allbuf[idx] == '\r' && allbuf[idx+1] == '\n')
        return idx+2;
        else
        *(linebuf++) = allbuf[idx];
    }
    return -1;
}

void qs_event_set(struct qsevent *ev, int fd, NCALLBACK callback, void *arg)
{
    ev->events = 0;
    ev->fd = fd;
    ev->arg = arg;
    ev->callback = callback;
    ev->last_active = time(NULL);
    return;
}

int qs_event_add(int epfd, int events, struct qsevent *ev)
{
    struct epoll_event epv = {0, {0}};;
    epv.events = ev->events = events;
    epv.data.ptr = ev;

    if(ev->status == 1)
    { 
        if(epoll_ctl(epfd, EPOLL_CTL_MOD, ev->fd, &epv) < 0)
        {
            perror("EPOLL_CTL_MOD error\n");
            return -1;
        }
    }
    else if(ev->status == 0)
    {
        if(epoll_ctl(epfd, EPOLL_CTL_ADD, ev->fd, &epv) < 0)
        {
            perror("EPOLL_CTL_ADD error\n");    
            return -2;
        }
        ev->status = 1;
    }
    return 0;
}

int qs_event_del(int epfd, struct qsevent *ev)
{
    struct epoll_event epv = {0, {0}};
    if(ev->status != 1)
    return -1;
    ev->status = 0;
    epv.data.ptr = ev;
    if((epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &epv)))
    {
        perror("EPOLL_CTL_DEL error\n");
        return -1;
    }
    return 0;
}

int sock(short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in ser_addr;
    memset(&ser_addr, 0, sizeof(ser_addr));
    ser_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ser_addr.sin_family = AF_INET;
    ser_addr.sin_port = htons(port);

    bind(fd, (struct sockaddr*)&ser_addr, sizeof(ser_addr));

    if(listen(fd, 20) < 0)
    perror("listen error\n");

    printf("listener[%d] lstening..\n", fd);
    return fd;
}

int http_request(struct qsevent *ev)
{
    char linebuf[1024] = {0};
    int idx = readline(ev->buffer, 0, linebuf);
    if(strstr(linebuf, "GET"))
    {
        ev->method = HTTP_METHOD_GET;

        int i = 0;
        while(linebuf[sizeof("GET ") + i] != ' ')i++;
        linebuf[sizeof("GET ") + i] = '\0';
        sprintf(ev->resource, "./%s/%s", HTTP_METHOD_ROOT, linebuf+sizeof("GET "));
        printf("resource:%s\n", ev->resource);
    }
    else if(strstr(linebuf, "POST"))
    {}
    return 0;
}

int http_response(struct qsevent *ev)
{
    if(ev == NULL)return -1;
    memset(ev->buffer, 0, MAX_BUFLEN);

    printf("resource:%s\n", ev->resource);

    int filefd = open(ev->resource, O_RDONLY);
    if(filefd == -1)
    {
        ev->ret_code = 404;
        ev->length = sprintf(ev->buffer,
                             "HTTP/1.1 404 NOT FOUND\r\n"
                             "date: Thu, 11 Nov 2021 12:28:52 GMT\r\n"
                             "Content-Type: text/html;charset=ISO-8859-1\r\n"
                             "Content-Length: 85\r\n\r\n"
                             "<html><head><title>404 Not Found</title></head><body><H1>404</H1></body></html>\r\n\r\n");
    }
    else 
    {
        struct stat stat_buf;
        fstat(filefd, &stat_buf);
        if(S_ISDIR(stat_buf.st_mode))
        {

            printf(ev->buffer, 
                   "HTTP/1.1 404 Not Found\r\n"
                   "Date: Thu, 11 Nov 2021 12:28:52 GMT\r\n"
                   "Content-Type: text/html;charset=ISO-8859-1\r\n"
                   "Content-Length: 85\r\n\r\n"
                   "<html><head><title>404 Not Found</title></head><body><H1>404</H1></body></html>\r\n\r\n" );

        } 
        else if (S_ISREG(stat_buf.st_mode)) 
        {

            ev->ret_code = 200;

            ev->length = sprintf(ev->buffer, 
                                 "HTTP/1.1 200 OK\r\n"
                                 "Date: Thu, 11 Nov 2021 12:28:52 GMT\r\n"
                                 "Content-Type: text/html;charset=ISO-8859-1\r\n"
                                 "Content-Length: %ld\r\n\r\n", 
                                 stat_buf.st_size );

        }
        return ev->length;
    }
}

int qsreactor_init(struct qsreactor *reactor)
{
    if(reactor == NULL)
    return -1;
    memset(reactor, 0, sizeof(struct qsreactor));
    reactor->epfd = epoll_create(1);
    if(reactor->epfd <= 0)
    {
        perror("epoll_create error\n");
        return -1;
    }
    struct qseventblock *block = (struct qseventblock*)malloc(sizeof(struct qseventblock));
    if(block == NULL)
    {
        printf("blockinit malloc error\n");
        close(reactor->epfd);
        return -2;
    }
    memset(block, 0, sizeof(block));

    struct qsevent *evs = (struct qsevent*)malloc(MAX_EPOLLSIZE * sizeof(struct qsevent));
    if(evs == NULL)
    {
        printf("evsnit malloc error\n");
        close(reactor->epfd);
        return -3;
    }
    memset(evs, 0, sizeof(evs));

    block->next = NULL;
    block->eventsarrry = evs;

    reactor->blkcnt = 1;
    reactor->evblk = block;
    return 0;
}

int qsreactor_alloc(struct qsreactor *reactor)
{
    if(reactor == NULL)return -1;
    if(reactor->evblk == NULL)return -1;
    struct qseventblock *tailblock = reactor->evblk;
    while(tailblock->next != NULL)
    tailblock = tailblock->next;
    struct qseventblock *newblock = (struct qseventblock*)malloc(sizeof(struct qseventblock));
    if(newblock == NULL)
    {
        printf("newblock alloc error\n");
        return -1;
    }
    memset(newblock, 0, sizeof(newblock));

    struct qsevent *neweventarray = (struct qsevent*)malloc(sizeof(struct qsevent) * MAX_EPOLLSIZE);
    if(neweventarray == NULL)
    {
        printf("neweventarray malloc error\n");
        return -1;
    }
    memset(neweventarray, 0, sizeof(neweventarray));

    newblock->eventsarrry = neweventarray;
    newblock->next = NULL;

    tailblock->next = newblock;
    reactor->blkcnt++;

    return 0;
}

struct qsevent *qsreactor_idx(struct qsreactor *reactor, int sockfd)
{
    int index = sockfd / MAX_EPOLLSIZE;
    while(index >= reactor->blkcnt)qsreactor_alloc(reactor);
    int i=0;
    struct qseventblock *idxblock = reactor->evblk;
    while(i++<index && idxblock != NULL)
    idxblock = idxblock->next;

    return &idxblock->eventsarrry[sockfd%MAX_EPOLLSIZE];
}

int qsreactor_destory(struct qsreactor *reactor)
{
    close(reactor->epfd);
    free(reactor->evblk);
    reactor = NULL;
    return 0;
}

int qsreactor_addlistener(struct qsreactor *reactor, int sockfd, NCALLBACK acceptor)
{
    if(reactor == NULL)return -1;
    if(reactor->evblk == NULL)return -1;

    struct qsevent *event = qsreactor_idx(reactor, sockfd);
    qs_event_set(event, sockfd, acceptor, reactor);
    qs_event_add(reactor->epfd, EPOLLIN, event);

    return 0;
}

int send_cb(int fd, int events, void *arg)
{
    struct qsreactor *reactor = (struct qsreactor*)arg;
    struct qsevent   *ev = qsreactor_idx(reactor, fd);

    http_response(ev);
    int ret = send(fd, ev->buffer, ev->length, 0);
    if(ret < 0)
    {
        qs_event_del(reactor->epfd, ev);
        printf("clent[%d] ", fd);
        perror("send error\n");
        close(fd);
    }
    else if(ret > 0)
    {
        if(ev->ret_code == 200) 
        {
            int filefd = open(ev->resource, O_RDONLY);
            struct stat stat_buf;
            fstat(filefd, &stat_buf);

            sendfile(fd, filefd, NULL, stat_buf.st_size);
            close(filefd);   
        }
        printf("send to client[%d]:%s", fd, ev->buffer);
        qs_event_del(reactor->epfd, ev);
        qs_event_set(ev, fd, recv_cb, reactor);
        qs_event_add(reactor->epfd, EPOLLIN, ev);
    }
    return ret;
}

int recv_cb(int fd, int events, void *arg)
{
    struct qsreactor *reactor = (struct qsreactor*)arg;
    struct qsevent *ev = qsreactor_idx(reactor, fd);

    int len = recv(fd, ev->buffer, MAX_BUFLEN, 0);
    qs_event_del(reactor->epfd, ev);
    if(len > 0)
    {
        ev->length = len;
        ev->buffer[len] = '\0';

        printf("client[%d]:%s", fd, ev->buffer);

        http_request(ev);

        qs_event_del(reactor->epfd, ev);
        qs_event_set(ev, fd, send_cb, reactor);
        qs_event_add(reactor->epfd, EPOLLOUT, ev);
    }
    else if(len == 0)
    {
        qs_event_del(reactor->epfd, ev);
        close(fd);
        printf("client[%d] close\n", fd);
    }
    else
    {
        qs_event_del(reactor->epfd, ev);
        printf("client[%d]", fd);
        perror("reacv error,\n");
        close(fd);
    }
    return 0;
}

int accept_cb(int fd, int events, void *arg)
{
    struct qsreactor *reactor = (struct qsreactor*)arg;
    if(reactor == NULL)return -1;

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);

    int clientfd;


    if((clientfd = accept(fd, (struct sockaddr*)&client_addr, &len)) == -1)
    {
        if(errno != EAGAIN && errno != EINTR)
        {}
        perror("accept error\n");
        return -1;
    }

    int flag = 0;
    if((flag = fcntl(clientfd, F_SETFL, O_NONBLOCK)) < 0)
    {
        printf("fcntl noblock error, %d\n",MAX_BUFLEN);
        return -1;
    }
    struct qsevent *event = qsreactor_idx(reactor, clientfd);

    qs_event_set(event, clientfd, recv_cb, reactor);
    qs_event_add(reactor->epfd, EPOLLIN, event);

    printf("new connect [%s:%d], pos[%d]\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), clientfd);

    return 0;
}

int qsreactor_run(struct qsreactor *reactor)
{
    if(reactor == NULL)
    return -1;
    if(reactor->evblk == NULL)
    return -1;
    if(reactor->epfd < 0)
    return -1;

    struct epoll_event events[MAX_EPOLL_EVENTS + 1];
    while(1)
    {
        int nready = epoll_wait(reactor->epfd, events, MAX_EPOLL_EVENTS, 1000);

        if(nready < 0)
        {
            printf("epoll_wait error\n");
            continue;
        }
        for(int i=0; i<nready; i++)
        {
            struct qsevent *ev = (struct qsevent*)events[i].data.ptr;
            if((events[i].events & EPOLLIN) && (ev->events & EPOLLIN))
            {
                ev->callback(ev->fd, events[i].events, ev->arg);    
            }
            if((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT))
            {
                ev->callback(ev->fd, events[i].events, ev->arg);
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned short port = atoi(argv[1]);

    int sockfd = sock(port);


    struct qsreactor *reactor = (struct qsreactor*)malloc(sizeof(struct qsreactor));
    qsreactor_init(reactor);

    qsreactor_addlistener(reactor, sockfd, accept_cb);
    qsreactor_run(reactor);

    qsreactor_destory(reactor);
    close(sockfd);
}


