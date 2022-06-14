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
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/shm.h>


#define USER_LIMIT 5    /*最大用户数量*/
#define BUFFER_SIZE 1024   /*读缓冲区大小*/
#define FD_LIMIT 65535  /*文件描述符数量限制*/
#define MAX_EVENT_NUMBER 1024
#define PROCESS_LIMIT 65536

/*客户数据：客户端 socket 地址、socket 文件描述符、处理这个连接的子进程的 PID、和父进程通信用的管道*/
struct client_data
{
    sockaddr_in address;
    int connfd;
    pid_t pid;
    int pipefd[2];
};

static const char* shm_name="/my_shm";//共享内存名称
int sig_pipefd[2];//管道通信
int epollfd;
int listenfd;
int shmfd;
char* share_mem=0;
/*客户连接数组。进程用客户连接的编号来索引这个数组，即可取得相关的客户连接数据*/
client_data* users=0;
/*子进程和客户连接的映射关系表。用进程的PID来索引这个数组，即可取得该进程所处理的客户连接的编号*/
int* sub_process=0;
/*当前客户数量*/
int user_count=0;
bool stop_child=false;

/*设置文件描述符 fd 为非阻塞的*/
int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);   /*获取文件描述符 fd 的状态*/
    int new_option=old_option | O_NONBLOCK;   
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

/*将文件描述符 fd 上的 EPOLLIN（数据可读事件） 注册到 epollfd 指示的 epoll 内核事件表中*/
void addfd(int epollfd,int fd)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN | EPOLLET;//使用 ET 模式
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);//注册
    setnonblocking(fd);//设置文件描述符 fd 为非阻塞的
}

/*信号处理函数*/
void sig_handler(int sig)
{
    int save_errno=errno;//保留原来的 errno，在函数最后恢复，以保证函数的可重入性
    int msg=sig;
    send(sig_pipefd[1],(char *)&msg,1,0);//通过管道的写通道发送信号 sig，以通知主循环
    errno=save_errno;
}

/*设置信号处理函数*/
void addsig(int sig,void(*handler)(int),bool restart=true)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);//在信号集中设置所有信号
    assert(sigaction(sig,&sa,NULL)!=-1);//设置 sa 为 sig 信号的处理函数
}

void del_resource()
{
    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
    close(listenfd);
    close(epollfd);
    shm_unlink(shm_name);
    delete [] users;
    delete [] sub_process;
}

/*停止一个子进程*/
void child_term_handler(int sig)
{
    stop_child=true;
}

/*子进程运行的函数。参数 idx 指出该子进程处理的客户连接的编号，users 是保存所有客户连接数据的数据，参数 share_mem 指出共享内存的起始地址*/
int run_child(int idx,client_data* users,char* share_mem)
{
    epoll_event events[MAX_EVENT_NUMBER];
    /*子进程使用I/O复用技术来同时监听来两个文件描述符：客户连接 socket、与父进程通信的管道文件描述符*/
    int child_epollfd=epoll_create(5);
    assert(child_epollfd!=-1);
    int connfd=users[idx].connfd;
    addfd(child_epollfd,connfd);
    int pipefd=users[idx].pipefd[1];
    addfd(child_epollfd,pipefd);
    int ret;
    /*子进程需要设置自己的信号处理函数*/
    addsig(SIGTERM,child_term_handler,false);

    while(!stop_child)
    {
        int number=epoll_wait(child_epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number<0) && (errno!=EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for(int i=0;i<number;i++)
        {
            int sockfd=events[i].data.fd;
            /*本子进程负责的客户连接有数据到达*/
            if((sockfd==connfd) && (events[i].events & EPOLLIN))
            {
                memset(share_mem+idx*BUFFER_SIZE,'\0',BUFFER_SIZE);
                /*将客户数据读取到对应的读缓存中。该读缓存是共享内存的一段，它开始于 idx*BUFFER_SIZE 处，长度为 BUFFER_SIZE 字节。因此，各个客户连接的读缓存是共享的。*/
                ret=recv(connfd,share_mem+idx*BUFFER_SIZE,BUFFER_SIZE-1,0);
                if(ret<0)
                {
                    if(errno!=EAGAIN)
                    {
                        stop_child=true;
                    }
                }
                else if(ret==0)
                {
                    stop_child=true;
                }
                else
                {
                    /*成功读取客户数据后就通知主进程（通过管道）来处理*/
                    send(pipefd,(char *)&idx,sizeof(idx),0);
                }
            }
            /*主进程通知本进程（通过管道）将第 client 个客户的数据发送到本进程负责的客户端*/
            else if((sockfd==pipefd) && (events[i].events & EPOLLIN))
            {
                int client =0;
                /*接收主进程发送来的数据，即有客户数据到达的连接的编号*/
                ret=recv(sockfd,(char *)&client,sizeof(client),0);
                if(ret<0)
                {
                    if(errno!=EAGAIN)
                    {
                        stop_child=true;
                    }
                }
                else if(ret==0)
                {
                    stop_child=true;
                }
                else
                {
                    send(connfd,share_mem+client*BUFFER_SIZE,BUFFER_SIZE,0);
                }
            }
            else
            {
                continue;
            }
        }
    }

    close(connfd);
    close(pipefd);
    close(child_epollfd);
    return 0;
}

int main(int argc,char* argv[])
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

    listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret!=-1);

    /*开启监听*/
    ret=listen(listenfd,5);
    assert(ret!=-1);

    user_count=0;//客户数目
    users=new client_data[USER_LIMIT+1];//new 客户连接数组
    sub_process=new int[PROCESS_LIMIT];//子进程和客户连接的映射关系表。用进程的PID来索引这个数组，即可取得该进程所处理的客户连接的编号
    for(int i=0;i<PROCESS_LIMIT;++i)
    {
        sub_process[i]=-1;
    }

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd=epoll_create(5);//创建 epoll 内核事件表，用 epollfd 唯一标识（事件表的好处是无需每次调用都需要重传文件描述符集）
    assert(epollfd!=-1);
    addfd(epollfd,listenfd);//向 epoll 内核事件表中注册 监听文件描述符 listenfd

    /*使用 socketpair 创建管道，注册 sig_pipefd[0] 上的可读事件*/
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd);
    assert(ret!=-1);
    setnonblocking(sig_pipefd[1]);
    addfd(epollfd,sig_pipefd[0]);//打开 sig_pipefd[0] 表示读

    /*设置信号处理函数 - 触发信号时通知主循环*/
    addsig(SIGCHLD,sig_handler);//子进程退出或暂停
    addsig(SIGTERM,sig_handler);//kill 命令发出的终止进程信号
    addsig(SIGINT,sig_handler);//键盘输入以中断进程
    addsig(SIGPIPE,SIG_IGN);//往读端被关闭的管道或者 socket 连接中写数据，则利用 SIG_IGN 忽略目标信号
    bool stop_server=false;//停止服务器标志
    bool terminate=false;//终止服务器运行

    /*创建共享内存，作为所有客户 socket 连接的读缓存*/
    shmfd=shm_open(shm_name,O_CREAT | O_RDWR,0666);//创建无关进程间的共享内存（文件），以 只读 且 不存在则创建 的方式
    assert(shmfd!=-1);
    ret=ftruncate(shmfd,USER_LIMIT*BUFFER_SIZE);//将 shmfd 指定的文件设为 USER_LIMIT*BUFFER_SIZE 大小
    assert(ret!=-1);

    //share_mem 为指向用 mmap 申请的进程间共享的内存的地址的指针
    share_mem=(char *)mmap(NULL,USER_LIMIT*BUFFER_SIZE,PROT_READ | PROT_WRITE,MAP_SHARED,shmfd,0);
    assert(share_mem!=MAP_FAILED);
    close(shmfd);

    while(!stop_server)
    {
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);//就绪的文件描述符的个数
        if((number<0) && (errno!=EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for(int i=0;i<number;i++)//仅遍历就绪的文件描述符（在 poll 中还需要依次判断文件描述符是否就绪）
        {
            int sockfd=events[i].data.fd;
            /*新的客户连接到来*/
            if(sockfd==listenfd)//监听到新的连接请求
            {
                //accept 接受新连接为 connfd
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd<0)
                {
                    printf("errno is:%d\n",errno);
                    continue;
                }
                //如果请求太多，则发送相应信息到对端，并关闭新到的连接
                if(user_count>=USER_LIMIT)
                {
                    const char* info="too many users\n";
                    printf("%s",info);
                    send(connfd,info,strlen(info),0);
                    close(connfd);
                    continue;
                }
                /*保存第 user_count 个客户连接的相关数据*/
                users[user_count].address=client_address;
                users[user_count].connfd=connfd;
                /*在主进程和子进程间建立管道，以传递必要的数据*/
                ret=socketpair(PF_UNIX,SOCK_STREAM,0,users[user_count].pipefd);
                assert(ret!=-1);
                pid_t pid=fork();
                if(pid<0)//fork 调用失败
                {
                    close(connfd);
                    continue;
                }
                else if(pid==0)//fork 调用成功，在子进程中返回 0,故下面是子进程的处理过程
                {
                    //在子进程中关闭 epollfd、listenfd
                    close(epollfd);
                    close(listenfd);
                    //关闭针对第 user_count 个客户连接的读通道
                    close(users[user_count].pipefd[0]);
                    //关闭针对信号管道的读写通道
                    close(sig_pipefd[0]);
                    close(sig_pipefd[1]);
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
                    run_child(user_count,users,share_mem);
                    munmap((void *)share_mem,USER_LIMIT * BUFFER_SIZE);//释放 share_mem 指向的这段内存空间
                    exit(0);
                }
                else//fork 调用成功，在父进程中返回子进程的 PID,故下面是父进程的处理过程
                {
                    char *ip_client=inet_ntoa(client_address.sin_addr);
                    printf("%s is online!\n",ip_client);
                    close(connfd);//关闭本次连接的文件描述符 connfd（交给子进程处理了）
                    close(users[user_count].pipefd[1]);//关闭针对第 user_count 个客户连接的写通道
                    addfd(epollfd,users[user_count].pipefd[0]);//注册第 user_count 个客户连接的可读事件
                    users[user_count].pid=pid;//设置处理第 user_count 个客户连接的子进程的 PID

                    /*记录新的客户连接在数组 users 中的索引值，建立进程 pid 和该索引值之间的映射关系*/
                    sub_process[pid]=user_count;
                    user_count++;//用户连接数++
                }
            }
            /*处理信号事件*/
            else if((sockfd==sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret=recv(sig_pipefd[0],signals,sizeof(signals),0);
                if(ret==-1)
                {
                    continue;
                }
                else if(ret==0)
                {
                    continue;
                }
                else
                {
                    for(int i=0;i<ret;++i)
                    {
                        switch (signals[i])
                        {
                            /*子进程退出，表示有某个客户端关闭了连接*/
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while((pid=waitpid(-1,&stat,WNOHANG))>0)
                                {
                                    /*用子进程的 pid 取得被关闭的客户连接的编号*/
                                    int del_user = sub_process[pid];
                                    sub_process[pid]=-1;
                                    if((del_user<0) || (del_user>USER_LIMIT))
                                    {
                                        continue;
                                    }
                                    /*清除第 del_user 个客户连接使用的相关数据*/
                                    epoll_ctl(epollfd,EPOLL_CTL_DEL,users[del_user].pipefd[0],0);
                                    close(users[del_user].pipefd[0]);
                                    users[del_user]=users[--user_count];
                                    sub_process[users[del_user].pid]=del_user;
                                }
                                if(terminate && user_count==0)
                                {
                                    stop_server=true;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                /*结束服务器程序*/
                                printf("kill all the child now\n");
                                if(user_count==0)
                                {
                                    stop_server=true;
                                    break;
                                }
                                for(int i=0;i<user_count;++i)
                                {
                                    int pid=users[i].pid;
                                    kill(pid,SIGTERM);
                                }
                                terminate=true;
                                break;
                            }
                            default:
                            {
                                break;
                            }

                        }
                    }
                }
            }
            /*某个子进程向父进程写入了数据*/
            else if(events[i].events & EPOLLIN)
            {
                int child=0;
                /*读取管道数据，child 变量记录了是哪个客户连接有数据到达*/
                ret=recv(sockfd,(char *)&child,sizeof(child),0);
                printf("read data from child across pipe\n");
                if(ret==-1)
                {
                    continue;
                }
                else if(ret==0)
                {
                    continue;
                }
                else
                {
                    /*向除负责处理第 child 个客户连接的子进程之外的其他进程发送消息，通知它们有客户数据要写*/
                    for(int j=0;j<user_count;++j)
                    {
                        if(users[j].pipefd[0]!=sockfd)
                        {
                            printf("send data to child across pipe\n");
                            send(users[j].pipefd[0],(char *)&child,sizeof(child),0);
                        }
                    }
                }
            }
        }        
    }
    del_resource();
    return 0;
}