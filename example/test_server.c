#include <stdio.h>
#include <string.h>
#include "../ksock.h"

#define SERVER_PORT     6004
#define SERVER_ADDR     "192.168.44.132"

char recv_buf[100];

int main(int argc, char const *argv[])
{
    //第一步：先socket打开文件描述符
    char *addr = SERVER_ADDR;
    uint16_t port = SERVER_PORT;
    if (3 == argc)
    {
        addr = argv[1];
        port = atoi(argv[2]);
    }
    int hd = -1;
    int ret = -1;
    struct ksock_init i;
    i.af = KSOCK_INET;
    i.proto = KSOCK_TCP;
    hd = k_socket(i);
    if (KSOCK_ERR == hd)
    {
        k_perror("k_socket");
        return -1;
    }
    printf("sock hd = %d\n", hd);

    //第二步：bind绑定sockfd和当前电脑的ip地址和端口号
    ret = k_listen(hd, addr, port, KSOCK_INET);
    if (KSOCK_ERR == ret)
    {
        k_perror("k_listen");
        return -1;
    }
    ret = k_accept(hd);
    if (KSOCK_ERR == ret)
    {
        k_perror("k_accept");
        return -1;
    }

    struct ksock_connect_node node;
    node.fd = -1;
    char recv_buf[100];
    int idx = 50;
    while (1)
    {
        
        ret = k_get_accept_node(hd, &node);
        if (ret == KSOCK_SUC)
        {   
            printf("get accept!!!!!!\n");
            //收到连接，可以做想做的事情了
            ret = k_recv(node, recv_buf, sizeof(recv_buf), 0);
            if (ret > 0)
            {
                printf("get msg: %s", recv_buf);
            }
        }
        else
        {
            if (-1 == node.fd)
            {
                k_perror("get accept node");
            }
            else
            {
                printf("node.fd = %d\n", node.fd);
                ret = k_recv(node, recv_buf, sizeof(recv_buf), 0);
                if (ret > 0)
                {
                    printf("get msg: %s\n", recv_buf);
                }
            }
            memset(recv_buf, 0, sizeof(recv_buf));
        }
        sleep(1);
        idx--;
        if(idx <= 0)
        {
            ret = k_accept_cancel(hd, 1);
            if (ret == KSOCK_SUC)
            {
                printf("cancel accept!\n");
            }
        }
    }

    return 0;
}
