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

#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#define MAX_BUFLEN          4096
#define MAX_EPOLLSIZE       1024
#define MAX_EPOLL_EVENTS    1024

#define GUID            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum
{ 
    WS_HANDSHAKE = 0,
    WS_TANSMISSION = 1,
    WS_END = 2,
};

typedef struct _ws_ophdr{
    unsigned char opcode:4,
    rsv3:1,
    rsv2:1,
    rsv1:1,
    fin:1;
    unsigned char pl_len:7,
    mask:1;
}ws_ophdr;

typedef struct _ws_head_126{
    unsigned short payload_lenght;
    char mask_key[4];
}ws_head_126;

typedef struct _ws_head_127{
    long long payload_lenght;
    char mask_key[4];
}ws_head_127;

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

    /*websocket param*/
    int status_machine;
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

    for(;idx < len;idx ++) {
        if (allbuf[idx] == '\r' && allbuf[idx+1] == '\n') {
            return idx+2;
        } else {
            *(linebuf++) = allbuf[idx];
        }
    }
    return -1;
}

int base64_encode(char *in_str, int in_len, char *out_str)
{    
    BIO *b64, *bio;    
    BUF_MEM *bptr = NULL;    
    size_t size = 0;    

    if (in_str == NULL || out_str == NULL)        
    return -1;    

    b64 = BIO_new(BIO_f_base64());    
    bio = BIO_new(BIO_s_mem());    
    bio = BIO_push(b64, bio);

    BIO_write(bio, in_str, in_len);    
    BIO_flush(bio);    

    BIO_get_mem_ptr(bio, &bptr);    
    memcpy(out_str, bptr->data, bptr->length);    
    out_str[bptr->length-1] = '\0';    
    size = bptr->length;    

    BIO_free_all(bio);    
    return size;
}

#define WEBSOCK_KEY_LENGTH  19

int websocket_handshake(struct qsevent *ev)
{
    char linebuf[128];
    int index = 0;
    char sec_data[128] = {0};
    char sec_accept[32] = {0};
    do
    {
        memset(linebuf, 0, sizeof(linebuf));
        index = readline(ev->buffer, index, linebuf);
        if(strstr(linebuf, "Sec-WebSocket-Key"))
        {
            strcat(linebuf, GUID);
            SHA1(linebuf+WEBSOCK_KEY_LENGTH, strlen(linebuf+WEBSOCK_KEY_LENGTH), sec_data);
            base64_encode(sec_data, strlen(sec_data), sec_accept);
            memset(ev->buffer, 0, MAX_BUFLEN);

            ev->length = sprintf(ev->buffer,
                                 "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-websocket-Accept: %s\r\n\r\n", sec_accept);
            break;   
        }
    }while(index != -1 && (ev->buffer[index] != '\r') || (ev->buffer[index] != '\n'));
    return 0;
}

void websocket_umask(char *payload, int length, char *mask_key)
{
    int i = 0;
    for( ; i<length; i++)
    payload[i] ^= mask_key[i%4];
}

int websocket_transmission(struct qsevent *ev)
{
    ws_ophdr *ophdr = (ws_ophdr*)ev->buffer;
    printf("ws_recv_data length=%d\n", ophdr->pl_len);
    if(ophdr->pl_len <126)
    {
        char * payload = ev->buffer + sizeof(ws_ophdr) + 4;
        if(ophdr->mask)
        {
            websocket_umask(payload, ophdr->pl_len, ev->buffer+2);
            printf("payload:%s\n", payload);
        }
        memset(ev->buffer, 0, ev->length);
        strcpy(ev->buffer, "00ok");
    }
    return 0;
}

int websocket_request(struct qsevent *ev)
{
    if(ev->status_machine == WS_HANDSHAKE)
    {
        websocket_handshake(ev);
        ev->status_machine = WS_TANSMISSION;
    }else if(ev->status_machine == WS_TANSMISSION){
        websocket_transmission(ev);
    }
    return 0;
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
        printf("send to client[%d]:\n%s\n", fd, ev->buffer);
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

        printf("client[%d]:\n%s\n", fd, ev->buffer);

        websocket_request(ev);

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

    event->status_machine = WS_HANDSHAKE;

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


