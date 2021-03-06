#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <fcntl.h>  
#include <sys/time.h>
#include <string.h>
#include "ksock.h"

#define HD_SIZE 79
#define MAX_EVENTS 1000000
const int LISTEN_QUEUE_MAX_NUM = 1000;
const int ACCEPT_QUEUE_MAX_NUM = 100;
const int RECV_QUEUE_MAX_NUM = 100;
struct ksock_node *_hd_array[HD_SIZE] = {NULL};
struct ksock_connect_node *_connect_head = NULL;
struct ksock_connect_node *_connect_tail = NULL;
int _connect_cnt = 0;

int epoll_fd = 0;
pthread_t accept_thread = -1;
pthread_t recv_thread = -1;

void __event_del(struct ksock_connect_node *p);
int __event_add(struct ksock_connect_node *p);
static inline int __check_hd(const int hd)
{
    if (hd < 0 || hd >= HD_SIZE)
    {
        _error_msg = "hd error!";
        return KSOCK_ERR;
    }
    if (NULL == _hd_array[hd])
    {
        _error_msg = "hd not find!";
        return KSOCK_ERR;
    }
    return KSOCK_SUC;
}

static inline int __check_nd(const long nd)
{
    struct ksock_connect_node *p = (struct ksock_connect_node *)nd;
    if (nd != p->nd)
    {
        _error_msg = "nd not find!";
        return KSOCK_ERR;
    }
    return KSOCK_SUC;
}

void k_perror(const char *msg)
{
    char c[50] = "";
    strcat(c, msg);
    strcat(c, ": %s\n");
    printf(c, _error_msg);
}

int __k_add(const int fd, const struct ksock_init i)
{
    struct ksock_node *p = (struct ksock_node*)malloc(sizeof(struct ksock_node));
    p->init.af = i.af;
    p->init.proto = i.proto;
    p->fd = fd;
    p->state = KSOCK_STATE_SLEEP;
    p->accept_head = NULL;
    p->accept_tail = NULL;
    p->connect_node = NULL;
    p->accept_state = 0;
    p->accept_count = 0;
    p->mode = KSOCK_UNKNOW;

    for(int i = 0; i < HD_SIZE && (NULL == _hd_array[i]); i++)
    {
        _hd_array[i] = p;
        return i;
    }
    _error_msg = "no more space!";
    return KSOCK_ERR;
}

int __k_recv_pop(struct ksock_connect_node *node, struct ksock_msg *msg)
{
    if (NULL == node->msg_head)
    {
        _error_msg = "no msg!";
        return KSOCK_ERR;
    }
    memcpy(msg->buf, node->msg_head->buf, node->msg_head->len);
    msg->len = node->msg_head->len;
    msg->next = NULL;
    struct ksock_msg *temp = node->msg_head;
    node->msg_head = node->msg_head->next;
    node->recv_count -= 1;
    free(temp->buf);
    free(temp);
    temp = NULL;
    if (NULL == node->msg_head)
    {
        node->msg_tail = NULL;
    }
    return KSOCK_SUC;
}

int __k_remove(const int hd)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }
    struct ksock_node *p = _hd_array[hd];
    
    if(0 != p->accept_state)
    {
        k_accept_cancel(hd, 1);
    }
    
    if (NULL != p->connect_node)
    {
        if (1 == p->connect_node->recv_state)
        {
            __event_del(p->connect_node);
        }

        struct ksock_msg msg;
        while(__k_recv_pop(p->connect_node, &msg) == KSOCK_SUC)
        {
            
        }

        free(p->connect_node);
        p->connect_node = NULL;
    }
    close(p->fd);
    free(p);
    _hd_array[hd] = NULL;
    return KSOCK_SUC;
    
}

int __k_accept_push(struct ksock_connect_node *node)
{
    int hd = node->hd;
    if(_hd_array[hd]->accept_count < ACCEPT_QUEUE_MAX_NUM)
    {
        if (NULL == _hd_array[hd]->accept_tail)
        {
            _hd_array[hd]->accept_tail = _hd_array[hd]->accept_head = node;
        }
        else
        {
            _hd_array[hd]->accept_tail->next = node;
            _hd_array[hd]->accept_tail = node;
        }
        _hd_array[hd]->accept_count += 1;
        return KSOCK_SUC;
    }
    else
    {
        // TODO 把错误信息返回给连接，然后断开连接
        char send_buf[100];
        strcpy(send_buf, "accept overflow.");
        send(node->fd, send_buf, strlen(send_buf), 0);
        close(node->fd);
        free(node);
        _hd_array[hd]->state = KSOCK_ACCEPT_OVERFLOW;
        return KSOCK_ERR;
    }
}

int __k_accept_pop(const int hd, struct ksock_connect_node *node)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }

    if (NULL == _hd_array[hd]->accept_head)
    {
        _error_msg = "no accept!";
        return KSOCK_ERR;
    }
    node->fd = _hd_array[hd]->accept_head->fd;
    node->hd = _hd_array[hd]->accept_head->hd;
    node->addr_in = _hd_array[hd]->accept_head->addr_in;
    node->recv_state = _hd_array[hd]->accept_head->recv_state;
    node->msg_head = NULL;
    node->msg_tail = NULL;
    node->recv_count = _hd_array[hd]->accept_head->recv_count;
    node->next = NULL;
    node->last = NULL;
    node->nd = -1;
    node->pa.len = 0;
    node->pa.flag = -1;
    struct ksock_connect_node *temp = _hd_array[hd]->accept_head;
    _hd_array[hd]->accept_head = _hd_array[hd]->accept_head->next;
    _hd_array[hd]->accept_count -= 1;
    free(temp);
    temp = NULL;
    if (NULL == _hd_array[hd]->accept_head)
    {
        _hd_array[hd]->accept_tail = NULL;
    }
    return KSOCK_SUC;
}


int __k_connect_push(struct ksock_connect_node *node)
{
    if (_connect_cnt > MAX_EVENTS)
    {
        return KSOCK_ERR;
    }
    if (NULL == _connect_tail)
    {
        _connect_head = _connect_tail = node;
    }
    else
    {
        _connect_tail->next = node;
        node->last = _connect_tail;
        _connect_tail = node;
    }
    _connect_cnt++;
    return KSOCK_SUC;
}

int __k_connect_remove(struct ksock_connect_node *p)
{
    if (NULL == p->next)
    {
        _connect_tail = p->last;
        if (NULL == _connect_tail)
        {
            _connect_head = NULL;
        }
        else
        {
            _connect_tail->next = NULL;
        }
    }
    else if(NULL == p->last)
    {
        _connect_head = p->next;
        if (NULL == _connect_head)
        {
            _connect_tail = NULL;
        }
        else
        {
            _connect_head->last = NULL;
        }
    }
    else
    {
        p->next->last = p->last;
        p->last->next = p->next;
    }

    close(p->fd);

    if (1 == p->recv_state)
    {
        __event_del(p);
    }

    struct ksock_msg msg;
    while(__k_recv_pop(p, &msg) == KSOCK_SUC)
    {
        
    }
    _connect_cnt--;
    free(p);
    return KSOCK_SUC;
}

int __k_recv_push(struct ksock_connect_node *node, struct ksock_msg *msg)
{
    if (NULL == node->msg_tail)
    {
        node->msg_tail = node->msg_head = msg;
    }
    else
    {
        node->msg_tail->next = msg;
        node->msg_tail = msg;
    }
    node->recv_count += 1;
    return KSOCK_SUC;
}

void * accept_func(void *arg)
{
    int max_fd = 0, ret = -1;
    fd_set fdtbl;
    struct timeval tval;
    tval.tv_sec = 0;
    tval.tv_usec = 2;
    while(1)
    {
        FD_ZERO(&fdtbl);
        max_fd = 0;
        ret = -1;
        for (size_t i = 0; i < HD_SIZE; i++)
        {
            if (KSOCK_ERR == __check_hd(i))
            {
                break;
            }
            if (_hd_array[i]->accept_state == 1)
            {
                if (_hd_array[i]->fd > max_fd)
                max_fd = _hd_array[i]->fd;
                FD_SET(_hd_array[i]->fd, &fdtbl);
            }
        }
        ret = select(max_fd + 1, &fdtbl, NULL, NULL, &tval);
        if (ret == 0)
        {
            //time out
        }
        else if (ret == -1)
        {
            printf("accept error occurred in accept thread of select！");
        }
        else
        {
            for (size_t i = 0; i < HD_SIZE; i++)
            {
                if (KSOCK_ERR == __check_hd(i))
                {
                    break;
                }
                if (ret <= 0)
                {
                    break;
                }
                if (FD_ISSET(_hd_array[i]->fd, &fdtbl))
                {
                    int accept_fd = -1;
                    struct sockaddr_in client_addr = {0};
                    socklen_t len = 0;
                    accept_fd = accept(_hd_array[i]->fd, (struct sockaddr*)&client_addr, &len);
                    if (fcntl(accept_fd, F_SETFL, O_NONBLOCK) < 0)
                    {
                        _error_msg = "connect fd nonblocking failed!";
                        send(accept_fd, "fd nonblocking failed, then close!", 35, 0);
                        close(accept_fd);
                    }
                    else
                    {
                        struct ksock_connect_node *p = (struct ksock_connect_node *)malloc(sizeof(struct ksock_connect_node));
                        p->fd = accept_fd;
                        p->hd = i;
                        p->state = _hd_array[i]->state;
                        p->addr_in = client_addr;
                        p->msg_head = NULL;
                        p->msg_head = NULL;
                        p->recv_count = 0;
                        p->recv_state = 0;
                        p->next = NULL;
                        p->last = NULL;
                        p->nd = -1;
                        p->pa.len = 0;
                        p->pa.flag = -1;
                        __k_accept_push(p);
                        ret--;
                    }
                }
            }
        }
    }
}

int __event_add(struct ksock_connect_node *p)
{
    struct epoll_event epv;
    epv.data.ptr = p;
    epv.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, p->fd, &epv) < 0)
    {
        _error_msg = "epoll add fail!";
        return KSOCK_ERR;
    }
    p->recv_state = 1;
    return KSOCK_SUC;
}

void __event_del(struct ksock_connect_node *p)
{
    struct epoll_event epv;
    epv.data.ptr = p;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, p->fd, &epv);
    p->recv_state = 0;
}

void * recv_func(void *arg)
{
    struct epoll_event events[MAX_EVENTS];
    int fds = epoll_wait(epoll_fd, events, MAX_EVENTS, 2);
    if (fds < 0)
    {
        //主线程需要回收本子线程
        printf("epoll wait error! please exit thread!\n");
        return NULL;
    }
    
    for(int i = 0; i < fds; i++)
    {
        int ret = 0;
        struct ksock_connect_node *p = (struct ksock_connect_node *)events[i].data.ptr;
        if (events[i].events&(EPOLLIN | EPOLLET))
        { 
            void *temp_buf = malloc(p->pa.len);
            do
            {
                memset(temp_buf, 0, p->pa.len);
                ret = recv(p->fd, temp_buf, p->pa.len, p->pa.flag);
                if (ret > 0)
                {
                    if (p->recv_count < RECV_QUEUE_MAX_NUM)
                    {
                        struct ksock_msg *msg = (struct ksock_msg *)malloc(sizeof(struct ksock_msg));
                        msg->next = NULL;
                        void *buf = malloc(ret);
                        memcpy(buf, temp_buf, ret);
                        msg->buf = buf;
                        msg->len = ret;
                        __k_recv_push(p, msg);
                    }
                    else
                    {
                        printf("warnning fd: %d, recv queue overflow!", p->fd);
                        p->state = KSOCK_RECV_OVERFLOW;
                    }
                }
            } while (ret > 0);
            free(temp_buf);
            if (ret == 0)
            {
                //socket被正常关闭,可以做后续的移除工作
                p->state = KSOCK_CLOSED;
                k_remove_connect_node(p->nd);
                __event_del(p);
            }
        }
    }
}

int k_socket(const struct ksock_init i)
{
    int sockfd = -1;
    sockfd = socket(i.af, i.proto, 0);
    if (sockfd < 0)
    {
        perror("k_socket");
        return KSOCK_ERR;
    }
    __k_add(sockfd, i);
}

int k_listen(const int hd, const char *address, const uint16_t port, const short family)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }

    struct sockaddr_in addr_in = {0};
    int ret = -1;

    addr_in.sin_family = family;
    addr_in.sin_port = htons(port);
    addr_in.sin_addr.s_addr = inet_addr(address);
    ret = bind(_hd_array[hd]->fd, (const struct sockaddr*)&addr_in, sizeof(addr_in));
    if (ret < 0)
    {
        perror("k_listen of bind");
        return KSOCK_ERR;
    }

    ret = listen(_hd_array[hd]->fd, LISTEN_QUEUE_MAX_NUM);
    if (ret < 0)
    {
        perror("k_listen");
        return KSOCK_ERR;
    }
    _hd_array[hd]->mode = KSOCK_SERVER;
    _hd_array[hd]->state = KSOCK_STATE_LISTEN;
    return KSOCK_SUC;
}

int k_accept(const int hd)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }
    
    if (0 != _hd_array[hd]->accept_state)
    {
        _error_msg = "this socket hd had accept!";
        return KSOCK_ERR;
    }
    _hd_array[hd]->accept_state = 1;
    if (-1 == accept_thread)
        pthread_create(&accept_thread, NULL, accept_func, NULL);
    return KSOCK_SUC;
}

int k_accept_cancel(const int hd, int is_clear_accept)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }
    
    if (0 == _hd_array[hd]->accept_state)
    {
        _error_msg = "accept_state not exist!";
        return KSOCK_ERR;
    }
    // pthread_cancel(_hd_array[hd]->accept_state);
    _hd_array[hd]->accept_state = 0;
    _hd_array[hd]->state = KSOCK_STATE_LISTEN;
    if (is_clear_accept)
    {
        struct ksock_connect_node p;
        while(__k_accept_pop(hd, &p) == KSOCK_SUC)
        {
            //TODO 断开连接 p->fd
            close(p.fd);
        }
    }
    return KSOCK_SUC;
}

int k_get_accept_node(const int hd, long *nd)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }
    //这里待优化，不用每次都申请堆内存，确定有链接之后再去申请也不迟
    struct ksock_connect_node *p = (struct ksock_connect_node *)malloc(sizeof(struct ksock_connect_node));

    if (__k_accept_pop(hd, p) == KSOCK_SUC)
    {
        
        if (__k_connect_push(p) == KSOCK_SUC)
        {
            memcpy(nd, &p, sizeof(void *));
            p->nd = *nd;
            return KSOCK_SUC;
        }
        else
        {
            send(p->fd, "sever connect cnt overflow!", 30, 0);
            close(p->fd);
            free(p);
            return KSOCK_ERR;
        }
        
    }
    else
    {
        free(p);
        return KSOCK_ERR;
    }
}

int k_remove_connect_node(const long nd)
{
    if (__check_nd(nd) == KSOCK_ERR)
    {
        return KSOCK_ERR;
    }
    struct ksock_connect_node *p = (struct ksock_connect_node *)nd;
    __k_connect_remove(p);
    return KSOCK_SUC;
}

int k_get_connect_node(const int hd, long *nd)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }

    if (NULL == _hd_array[hd]->connect_node)
    {
        _error_msg = "connect node not exist!";
        return KSOCK_ERR;
    }
    if (_hd_array[hd]->connect_node->nd > 0)
    {
        *nd = _hd_array[hd]->connect_node->nd;
        printf("get nd aready, please check it!");
        return KSOCK_SUC;
    }
    if (__k_connect_push(_hd_array[hd]->connect_node) == KSOCK_SUC)
    {
        memcpy(nd, &(_hd_array[hd]->connect_node), sizeof(void *));
        _hd_array[hd]->connect_node->nd = *nd;
        return KSOCK_SUC;
    }   
    else
    {
        send(_hd_array[hd]->connect_node->fd, "client connect cnt overflow!", 30, 0);
        close(_hd_array[hd]->connect_node->fd);
        free(_hd_array[hd]->connect_node);
        _hd_array[hd]->connect_node = NULL;
        return KSOCK_ERR;
    }
}

int k_connect(const int hd, const char *address, const uint16_t port, const short family)
{
    if (KSOCK_ERR == __check_hd(hd))
    {
        return KSOCK_ERR;
    }
    
    struct ksock_connect_node *p = (struct ksock_connect_node*)malloc(sizeof(struct ksock_connect_node));
    p->fd = _hd_array[hd]->fd;
    p->hd = hd;
    p->state = _hd_array[hd]->state;
    p->msg_head = NULL;
    p->msg_tail = NULL;
    p->recv_count = 0;
    p->next = NULL;
    p->last = NULL;
    p->nd = -1;
    p->recv_state = 0;
    p->pa.len = 0;
    p->pa.flag = -1;

    struct sockaddr_in addr_in = {0};
    addr_in.sin_family = family;
    addr_in.sin_port = htons(port);
    addr_in.sin_addr.s_addr = inet_addr(address);

    p->addr_in.sin_family = addr_in.sin_family;
    p->addr_in.sin_port = addr_in.sin_port;
    p->addr_in.sin_addr.s_addr = addr_in.sin_addr.s_addr;
    int ret = connect(p->fd, (const struct sockaddr *)&addr_in, sizeof(addr_in));
    if (ret < 0)
    {
        perror("k_connect");
        _error_msg = "connect fail, error message printed!";
        return KSOCK_ERR;
    }
    if (fcntl(p->fd, F_SETFL, O_NONBLOCK) < 0)
    {
        _error_msg = "connect fd nonblocking failed!";
        close(p->fd);
        free(p);
        return KSOCK_ERR;
    }
    _hd_array[hd]->connect_node = p;
    _hd_array[hd]->state = KSOCK_STATE_ACTIVE;
    return KSOCK_SUC;
}

int k_send(const long nd, void *buf, size_t len, int flag)
{
    if (__check_nd(nd) == KSOCK_ERR)
    {
        return KSOCK_ERR;
    }
    struct ksock_connect_node *p = (struct ksock_connect_node *)nd;
    return send(p->fd, buf, len, flag);
}

int k_recv(const long nd, size_t len, int flag)
{

    if (__check_nd(nd) == KSOCK_ERR)
    {
        return KSOCK_ERR;
    }
    struct ksock_connect_node *p = (struct ksock_connect_node *)nd;

    if (1 == p->recv_state)
    {
        _error_msg = "this socket hd is on recv!";
        return KSOCK_ERR;
    }

    p->pa.len = len;
    p->pa.flag = flag;
    if (epoll_fd == 0)
    {
        epoll_fd = epoll_create(MAX_EVENTS);
        if (epoll_fd <= 0)
        {
            _error_msg = "create epoll fail!";
            return KSOCK_ERR;
        }
    }
    if (__event_add(p) == KSOCK_ERR)
        return KSOCK_ERR;

    if (-1 == recv_thread)
        pthread_create(&recv_thread, NULL, recv_func, NULL);
    return KSOCK_SUC;
}

int k_get_recv_msg(const long nd, struct ksock_msg *msg)
{
    if (__check_nd(nd) == KSOCK_ERR)
    {
        return KSOCK_ERR;
    }
    struct ksock_connect_node *p = (struct ksock_connect_node *)nd;

    if (__k_recv_pop(p, msg) == KSOCK_SUC)
    {
        return KSOCK_SUC;
    }
    else
    {
        return KSOCK_ERR;
    }
}

int k_recv_cancel(const long nd, int is_clear_recv)
{
    
    if (__check_nd(nd) == KSOCK_ERR)
    {
        return KSOCK_ERR;
    }
    struct ksock_connect_node *node = (struct ksock_connect_node *)nd;

    if (0 == node->recv_state)
    {
        _error_msg = "socket not in recv!";
        return KSOCK_ERR;
    }
    node->recv_state = 0;
    __event_del(node);
    if (is_clear_recv)
    {
        struct ksock_msg p;
        while(__k_recv_pop(node, &p) == KSOCK_SUC)
        {
            
        }
    }
    return KSOCK_SUC;
}

int k_close(const int hd)
{
    return __k_remove(hd);
}