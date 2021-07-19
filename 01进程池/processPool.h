#ifndef __PROCESSPOOL_H
#define __PROCESSPOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#include <netinet/in.h>
#include <arpa/inet.h>


//用于描述一个子进程的类
class process
{
public:
	pid_t m_pid;				//m_pid是目标子进程的PID
	int m_pipefd[2];			//m_pipefd是子进程和父进程之间通信用的管道,父进程只对fd[0]进行读写操作,子进程只对fd[1]进行读写操作
public:
	process() : m_pid(-1){}
};

//进程池类，定义为模板类，实现代码复用
template<typename T>
class processpool
{
private:
	processpool(int listenfd,int process_number=0);							//私有，单例模式访问
public:
	static processpool< T >* create(int listenfd,int process_number = 8){		//饿汉模式
		if(!m_instance){
			m_instance = new processpool< T >(listenfd,process_number);
		}
		return m_instance;
	}

	~processpool(){															//析构函数，释放子进程描述信息
		delete[] m_sub_process;  
	}

	void run();																//启动进程池

private:
	void setup_sig_pipe();
	void run_parent();
	void run_child();

private:
	static const int MAX_PROCESS_NUMBER = 16;								//进程所拥有的最大子进程数量
	static const int USER_PER_PROCESS = 65535;								//每个子进程最多可以处理的客户数量
	static const int MAX_EVENT_NUMBER = 10000;								//epoll最多能处理的事件数量

	int m_process_number;													//进程池中的进程总数
	int m_idx;																//子进程在池中的序号，从0开始
	int m_epollfd;															//每个进程都有一个epoll内核时间表，使用m_epollfd表示
	int m_listenfd;															//监听socket
	int m_stop;																//结束标识符，子进程通过m_stop决定是否停止

	process *m_sub_process;													//保存所有的子进程描述信息

	static processpool< T > *m_instance;										//进程池的静态实例对象
};

template<typename T>
processpool< T > *processpool< T >::m_instance = NULL;

static int sig_pipefd[2];													//用于处理信号！！！！！的管道，以实现统一事件源

/*
实现对描述符设置为非阻塞状态
*/
static int setnonblocking(int fd){											
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}

/*
添加新的文件描述符fd到epollfd中，进行监听
*/
static void addfd(int epollfd,int fd){										
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
	setnonblocking(fd);
}

/*
对应添加，这里进行删除操作。从epollfd表示的epoll内核事件表中删除fd上的所有注册事件
*/
static void removefd(int epollfd,int fd){
	epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
	close(fd);
}

/*
errno 是线程安全，即每个线程有自己的 errno，但不是异步信号安全。
如果信号处理函数比较复杂，且调用了可能会改变 errno 值的库函数，必须考虑在信号处理函数开始时保存、结束的时候恢复被中断线程的 errno 值；

void io_handler(int signo)
{
    chdir("/hommmm");  			//“/hommmm” 目录不存在
}
int main()
{
    signal(SIGIO, io_handler);
    int ret = rmdir("/home");
    //kill(getpid(), SIGIO);	//发送信号，会去处理信号io_handler
    if (ret < 0 ) 
    {   
            printf("%s:%d rmdir error:%s/n", __FILE__,  __LINE__, strerror(errno));
    }   
    return 0;
}
大部分情况下，上面的代码执行正常，因为权限不足，rmdir("/home")会返回错误，并且得到以下输出：
signal.c:23 rmdir error: Permission denied

但是如果把第9行的注释打开，程序的输出结果就不太对了：
signal.c:23 rmdir error:No such file or directory

为什么呢，根据输出的错误信息我们知道，两种情况下strerror返回的字符串不一样，也就是errno不一样。
原因在于第8行调用rmdir后，errno被设置成了“Permission denied”但是在第9行发出了信号，于是程序跳到io_handler中执行chdir，因为chdir执行出错（目标目录不存在）所以errno被改成了“No such file or directory”此时io_handler函数返回，第12行的时候errno已经是chdir设置的了，而rmdir设置的errno已经丢了。
根本原因在于errno是一个全局变量，在io_handler中可以对它进行修改，从而导致第12行得不到准确的出错提示！！！！！
*/
static void sig_handler(int sig){
	int old_errno = errno;													//注意errno是定义在头文件中的全局变量
	int msg = sig;
	send(sig_pipefd[1],(char*)&msg,1,0);									//发送信号给对端,sizeof(msg)也可以
	errno = old_errno;														//注意：sig_pipefd[1]用于发送信号，sig_pipefd[0]用于接收信号！！！！
}

/*
用于设置信号sig所对应的的处理动作handler，当遇到这个信号后，会去调用这个处理函数
*/
static void addsig(int sig,void(handler)(int),bool restart = true){		//设置信号对应的处理函数！！！
	struct sigaction sa;
	memset(&sa,0,sizeof(sa));

	sa.sa_handler = handler;
	if(restart){
		sa.sa_flags |= SA_RESTART;
	}

	sigfillset(&sa.sa_mask);
	assert(sigaction(sig,&sa,NULL) != -1);
}

//==============================================模板类的实现想要与头文件放在一起！！！================================================
//另外一种方法，避免惊群问题！！！，注意查看父进程的信息
//方法一：专门使用父进程accept接收客户端的连接，然后将连接发送给子进程处理
//方法二：使用一个标识，然后合理选取一个子进程，发送标识给该进程，告诉这个进程去处理处理客户端的连接
//我们这里使用的是方法二

/*
进程池构造函数:
参数listenfd是监听socket，需要在参加进程池之前被创建，否则子进程无法直接引用
参数process_number是指定进程池中的子进程数量
*/
template<typename T>
processpool< T >::processpool(int listenfd,int process_number)
	:m_process_number(process_number),m_listenfd(listenfd),m_stop(false),m_idx(-1){		//注意：m_idx=-1表示为主进程
		assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

		m_sub_process =new process[process_number];										//设置进程描述符个数
		assert(m_sub_process);

		//开始创建对应的子进程，并简历他们与父进程之间的管道
		for(int i=0;i<process_number;i++){
			int ret = socketpair(PF_UNIX,SOCK_STREAM,0,m_sub_process[i].m_pipefd);		//注意使用socketpair是全双工管道
			assert(ret == 0);

			m_sub_process[i].m_pid = fork();											//创建子进程，记录进程id
			assert(m_sub_process[i].m_pid >= 0);

			if(m_sub_process[i].m_pid > 0){												//父进程
				close(m_sub_process[i].m_pipefd[1]);									//关闭fd[1],父进程只对fd[0]进行读写操作
				continue;
			}else{																		//子进程
				close(m_sub_process[i].m_pipefd[0]);									//关闭fd[0],子进程只对fd[1]进行读写操作
				m_idx = i;																//产生子进程，会拷贝m_idx信息，所以m_idx对于子进程操作的本进程的m_idx数据
				break;																	//子进程break，不会去进行循环产生子进程的子进程
			}	
		}
}

/*
统一事件源，用于处理信号在父子进程之间的传递
*/
template<typename T>
void processpool< T >::setup_sig_pipe()
{
	//创建epoll事件，监听表和信号管道
	m_epollfd = epoll_create(5);
	assert(m_epollfd != -1);

	int ret = socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd);								//专门用于处理信号的管道
	assert(ret != -1);

	setnonblocking(sig_pipefd[1]);														//1用于写，0用于读，我们这里不处理写，在sig_handler中处理写
	addfd(m_epollfd,sig_pipefd[0]);														//使用epoll监听管道的读取（接收）

	//设置该信号的处理函数
	addsig(SIGCHLD,sig_handler);														//进程终止或者停止信号，调用sig_handler中的send发送给子进程，对于子进程也会调用，没有坏处（如果子进程创建了孙子进程，可以这样被结束）
	addsig(SIGTERM,sig_handler);														//警告信号
	addsig(SIGINT,sig_handler);															//中断信号
	addsig(SIGPIPE,SIG_IGN);															//接收到管道消息的信号，比如客户端--->服务端，服务端接收到SIGPIPE信号，才去内核读取
	//注意：对于管道信号，我们采取忽略，不想下面子进程传递！！！
}

/*
主运行函数：用于运行运行父子进程程序，通过m_idx判断区分父子进程
对于父进程的m_idx为-1，子进程的m_idx大于等于0
*/
template<typename T>
void processpool< T >::run(){
	if(m_idx != -1){																	//运行子进程
		run_child();
		return;
	}
	run_parent();																		//运行父进程
}

//先查看父进程，父进程将到达的客户端连接，交给子进程处理，避免了惊群现象的出现！！！
template<typename T>
void processpool< T >::run_parent(){														//运行父进程
	setup_sig_pipe();																	//创建epoll池，监听listenfd,将接受到的客户端描述符交给子进程进行通信。并且注册信号处理函数

	addfd(m_epollfd,m_listenfd);														//添加listenfd进行监听新的客户端的到达

	epoll_event events[MAX_EVENT_NUMBER];

	//下面的局部变量，相对于这个函数中的while循环来说，可以认为是个全局变量
	int sub_process_index = 0;															//用来索引下一次应该使用哪个子进程
	int new_conn_flag = 1;																//用来作为标识符，发送给子进程,告诉子进程，接收到为1的数据，那么就去accpet客户端的数据
	
	int number = 0;																		//标识epoll响应的事件个数
	int ret = -1;																		//充当recv接收数据标识符

	//开始处理
	while(!m_stop){
		number = epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
		if((number < 0) && (errno != EINTR)){											//没有事件，并且不是中断
			printf("epoll failure!\n");
			break;
		}

		//遍历事件
		for(int i = 0;i<number;i++){
			int sockfd = events[i].data.fd;												//获取描述符
			if(sockfd == m_listenfd){													//有客户端打算连接，停止子进程去accept操作
				int i = sub_process_index;												//获取应该选取的子进程索引位置
				do
				{
					if(m_sub_process[i].m_pid != -1){									//!=-1表示，该子进程存在，可以处理任务
						break;
					}
					i = (i + 1) % m_process_number;										//+1处理
				}while(i != sub_process_index);											//从索引开始，去查找一圈子进程，找合适的子进程，如果无法找到合适的，就选择当前这个
				
				//下面判断是否找到了合适的索引子进程
				if(m_sub_process[i].m_pid == -1){
					m_stop = true;
					break;																//没有子进程可用，退出算了
				}

				//开始记录进程信息和发送标识给子进程
				sub_process_index = (i + 1) % m_process_number;							//选取的子进程号是i,这里记录下次的索引号i+1
				send(m_sub_process[i].m_pipefd[0],(char*)&new_conn_flag,sizeof(new_conn_flag),0);
				printf("send request to child %d\n",i);
			}
			else if((sockfd == sig_pipefd[0]) && (events[i].events && EPOLLIN))			//处理父进程接收的信号
			{
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0],signals,sizeof(signals),0);					//接收父进程接收的信号
				if(ret <= 0){															//没有接收到信号
					continue;
				}else{
					for(int i = 0; i < ret;i++){
						switch(signals[i]){
							case SIGCHLD:												//SIGCHLD，在一个进程终止或者停止时，将SIGCHLD信号发送给其父进程，按系统默认将忽略此信号，如果父进程希望被告知其子系统的这种状态，则应捕捉此信号。
							{
								pid_t pid;
								int stat;

								while((pid = waitpid(-1,&stat,WNOHANG)) > 0){			//找出是哪些子进程要退出！！！
									for(int i = 0;i<m_process_number;i++){				//遍历所有子进程
										if(m_sub_process[i].m_pid == pid){
											printf("child %d join\n", i);
											close(m_sub_process[i].m_pipefd[0]);		//关闭与之通信的管道
											m_sub_process[i].m_pid = -1;
										}
									}
								}

								//如果所有子进程都退出了，那么父进程也退出
								m_stop = true;
								for(int i=0;i<m_process_number;i++){
									if(m_sub_process[i].m_pid != -1){
										m_stop = false;
									}
								}
								break;
							}
							case SIGTERM:												//警告、中断
							case SIGINT:
							{
								//如果父进程接收到终止信号，那就杀死所有的子进程，并等待他们全部退出，最好使用信号，这里没有使用
								printf("kill all the child now!\n");
								for(int i=0;i<m_process_number;i++){
									int pid = m_sub_process[i].m_pid;
									if(pid != -1){
										kill(pid,SIGTERM);
									}
								}
								break;
							}
							default:
								break;
						}
					}	
				}
			}
			else																		//其他的不做过多处理
			{
				continue;	
			}
		}
	}

	//父进程退出循环，资源释放
	close(m_epollfd);
}

template<typename T>
void processpool< T >::run_child(){
	setup_sig_pipe();																	//对于子进程，也是有必要处理信号，并且创建epoll池，防止我们出现孙子进程

	//对于每个子进程，我们可以根据在进程池中的序号值m_idx找到与父进程通信的管道
	int pipefd = m_sub_process[m_idx].m_pipefd[1];										//子进程只对fd[1]进行读写操作
	//子进程需要去监听这个管道文件描述符pipefd,因为父进程会通过这个管道来通知子进程accept新连接
	addfd(m_epollfd,pipefd);

	epoll_event events[MAX_EVENT_NUMBER];												//子进程最大监听数量

	T *users = new T[USER_PER_PROCESS];													//每个子进程最多可以处理的客户数量
	assert(users);

	int number = 0;
	int ret = -1;

	while(!m_stop){
		number = epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);						//等待事件,其中我们是把所有监听的句柄设置为非阻塞的，所以会一直循环
		if(number < 0){
			if(errno==EINTR){
				printf("EINTR\n");
				continue;
			}else{
				printf("epoll failure\n");
				break;
			}
		}
		if(number == -1){
			printf("%s\n", strerror(errno));
		}
		for(int i=0;i<number;i++){														//处理响应的文件套接字
			int sockfd = events[i].data.fd;

			if((sockfd == pipefd) && (events[i].events & EPOLLIN)){						//父进程数据到达，是父进程传递过来的文件描述符，表示新的客户到达，我们会主动去监听这个描述符，去监听数据的到达！！！
				int client = 0;
				ret = recv(sockfd,(char*)&client,sizeof(client),0);						//从父进程中，接收标识符，如果为1，大于0，那么就是这个子进程被分配任务，来处理当前要连接的客户端！！！！
				if(((ret < 0) && (errno != EAGAIN)) || ret == 0){
					continue;															//读取出错
				}else{																	//开始进行处理，添加监听的描述符信息
					struct sockaddr_in client_address;
					socklen_t client_addrlength = sizeof(client_address);
					int connfd = accept(m_listenfd,(struct sockaddr*)&client_address,
												&client_addrlength);					//注意：m_listenfd是监听本地文件描述符，在创建进程池之前被实现

					if(connfd < 0){
						printf("errno is: %d\n",errno);									//连接出错
						continue;
					}

					addfd(m_epollfd,connfd);											//添加连接的文件描述符，设置为非阻塞状态
					//注意：模板类T必须实现init方法进行初始化客户连接。另外，我们使用数组实现直接使用connfd来索引逻辑处理对象（T）
					users[connfd].init(m_epollfd,connfd,client_address);				//将获取的所有相关数据，传递给模板类，进行初始化
				}
			}
			else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))			//有信号到达																	//下面处理子进程接收到的信号
			{																		
				int sig;
				char signals[1024];

				ret = recv(sig_pipefd[0],signals,sizeof(signals),0);					//接收多个信号,信号量原本是长整型，但是我们发送的时候转换为char类型，所以接收也同样使用char类型即可！！！
				if(ret <= 0){
					continue;															//没有信号到达
				}else{
					for(int i = 0; i < ret; i++){										//遍历所有的信号
						switch(signals[i]){
							case SIGCHLD:{												//SIGCHLD，在一个进程终止或者停止时，将SIGCHLD信号发送给其父进程
								pid_t pid;
								int stat;												//传出参数，可以设置为NULL
								while((pid = waitpid(-1,&stat,WNOHANG))){				//-1表示任意子进程，WNOHANG表示不阻塞模式
									continue;											//表示结束所有子进程，如果父进程接收到终止或者停止信号
								}
								break;
							}
							case SIGTERM:												//警告
							case SIGINT:{												//中断
								m_stop = true;											//可以退出循环，结束
								break;									
							}
							default:
								break;
						}
					}
				}
			}
			else if(events[i].events & EPOLLIN)											//有其他可读数据到达，客户端数据到达，需要进行处理。调用逻辑处理对象的process方法处理到达的数据
			{
				users[sockfd].process();												//注意：由子进程决定调用哪一个模板类处理对应的socket数据到达！！！
			}
			else
			{
				continue;
			}
		}
	}

	//开始回收子进程的资源
	delete[] users;
	users = NULL;
	close(pipefd);																		//关闭子进程与父进程之间通信的管道描述符（就是用来接收客户端连接的accpet描述符）
	close(m_epollfd);																	//关闭epoll描述符
	//方法结束，子进程结束！！！
}

#endif