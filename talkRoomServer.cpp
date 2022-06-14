#define _GNU_SOURCE 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>

#define USER_LIMIT 5    /*最大用户数量*/
#define BUFFER_SIZE 64   /*读缓冲区大小*/
#define FD_LIMIT 65535  /*文件描述符数量限制*/
/*客户数据：客户端 socket 地址、待写到客户端待数据的位置、从客户端读入的数据*/
struct client_data
{
    sockaddr_in address;
    char* write_buf;
    char buf[BUFFER_SIZE];
};

/*设置文件描述符 fd 为非阻塞的*/
int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);   /*获取文件描述符 fd 的状态*/
    int new_option=old_option | O_NONBLOCK;   
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

int main(int argc,char *argv[])
{
    if(argc<=2)
    {
        printf("usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }
    const char* ip=argv[1];
    int port=atoi(argv[2]); /*字符串 -> 整形*/

    /*服务器绑定本机 ip 和端口*/
    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    inet_pton(AF_INET,ip,&address.sin_addr);
    address.sin_port=htons(port);

    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret!=-1);
    
    /*开启监听*/
    ret=listen(listenfd,5);
    assert(ret!=-1);

    /*
    创建 users 数组，分配 FD_LIMIT 个 client_data 对象，可以预期：
    每个可能的 socket 连接都可以获得这样一个对象，并且 socket 的值可以
    直接用来索引（作为数组待下标）socket 连接对应待 client_data 对象，
    这是将 socket 和客户数据关联待简单而高效的方式。
    */
    client_data* users=new client_data[FD_LIMIT];
    /*尽管我们分配了足够多的 client_data 对象，但是为了提高 poll 待性能，仍有必要限制用户的数量*/
    pollfd fds[USER_LIMIT+1];
    int user_counter=0;
    for(int i=1;i<=USER_LIMIT;++i)
    {
        fds[i].fd=-1;
        fds[i].events=0;
    }
    /*向 fds[0] 上注册监听 socket（listenfd）*/
    fds[0].fd=listenfd;
    fds[0].events=POLLIN | POLLERR;
    fds[0].revents=0;

    while(1)
    {
        /*阻塞方式启用 poll IO复用，fds 为文件描述符集合*/
        ret=poll(fds,user_counter+1,-1);
        if(ret<0)
        {
            printf("poll failure\n");
            break;
        }

        /*遍历 poll 对应的文件描述符集合，处理请求*/
        for(int i=0;i<user_counter+1;++i)
        {
            /*监听到新的连接请求（POLLIN 表示有数据可读）*/
            if((fds[i].fd==listenfd) && (fds[i].revents & POLLIN))
            {
                /*accept 接受新连接为 connfd*/
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

                if(connfd<0)
                {
                    printf("errnois :%d\n",errno);
                    continue;
                }
                /*如果请求太多，则关闭新到的连接*/
                if(user_counter>=USER_LIMIT)
                {
                    const char* info="too many users\n";
                    printf("%s",info);
                    send(connfd,info,strlen(info),0);
                    close(connfd);
                    continue;
                }
                user_counter++;
                users[connfd].address=client_address;
                setnonblocking(connfd);
                /*向 poll 注册新到的连接*/
                fds[user_counter].fd=connfd;
                fds[user_counter].events=POLLIN | POLLRDHUP | POLLERR;
                fds[user_counter].revents=0;
                printf("comes a new user,now have %d users\n",user_counter);
            }
            /*socket 上发生的事件出错*/
            else if (fds[i].revents & POLLERR)
            {
                printf("get an error from %d\n",fds[i].fd);
                char errors[100];
                memset(errors,'\0',100);
                socklen_t length=sizeof(errors);
                if(getsockopt(fds[i].fd,SOL_SOCKET,SO_ERROR,&errors,&length)<0)
                {
                    printf("get socket option failed\n");
                }
                continue;
            }
            /*socket 上发生的事件被挂起*/
            else if (fds[i].revents & POLLRDHUP)
            {
                /*如果客户端关闭连接，则服务器也关闭对应的连接，并将用户总数减 1*/
                users[fds[i].fd]=users[fds[user_counter].fd];
                close(fds[i].fd);
                fds[i]=fds[user_counter];
                --i;
                --user_counter;
                printf("a client left\n");
            }
            /*socket 上读缓存中还有数据需要读取*/
            else if(fds[i].revents & POLLIN)
            {
                int connfd=fds[i].fd;
                memset(users[connfd].buf,'\0',BUFFER_SIZE);
                ret=recv(connfd,users[connfd].buf,BUFFER_SIZE-1,0);
                printf("get %d bytes of client data %s from %d\n",ret,users[connfd].buf,connfd);
                if(ret<0)
                {
                    /*如果读操作出错，则关闭连接*/
                    if(errno!=EAGAIN)
                    {
                        close(connfd);
                        users[fds[i].fd]=users[fds[user_counter].fd];
                        fds[i]=fds[user_counter];
                        --i;
                        --user_counter;
                    }
                }
                else if(ret==0)
                {
                    /*未读到有效数据*/
                }
                else
                {
                    /*如果接收到客户数据，则通知其他 socket 连接准备写数据*/
                    for(int j=1;j<=user_counter;++j)
                    {
                        if(fds[j].fd==connfd) continue; /*发出消息的 socket*/
                        fds[j].events |= ~ POLLIN;
                        fds[j].events |= POLLOUT;
                        users[fds[j].fd].write_buf=users[connfd].buf;
                    }
                }
            }
             /*socket 上有数据需要写*/
            else if(fds[i].revents & POLLOUT)
            {
                int connfd=fds[i].fd;
                if(!users[connfd].write_buf) continue;
                ret=send(connfd,users[connfd].write_buf,strlen(users[connfd].write_buf),0);
                users[connfd].write_buf=NULL;
                fds[i].events |= ~ POLLOUT;
                fds[i].events |= POLLIN;
            }
        }
    }
    delete [] users;
    close(listenfd);
    return 0;
}


