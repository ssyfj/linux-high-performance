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

#include "processPool.h"

/*
用于处理客户cgi请求的类，用于测试processpool的模板类
*/
class cgi_conn{
public:
	cgi_conn(){}
	~cgi_conn(){}
public:
	void init(int epollfd,int sockfd,const sockaddr_in& client_addr);
	void process();
private:
	static const int BUFFER_SIZE = 1024;									//读缓冲区大小
	static int m_epollfd;													//注意：epoll句柄是子进程中固定的，对于子进程唯一

	/*
	后面的客户端句柄和地址数据，是当子进程处理连接到达时，赋予的。
	其实m_epollfd可以设置为非静态，如果单纯从init赋值来看
	*/

	int m_sockfd;															//客户端文件句柄
	sockaddr_in m_address;													//客户端地址

	char m_buffer[BUFFER_SIZE];												//内部去获取我们要执行的程序的名称！！！使用\r\n标识结束，所以1024足够
	int m_read_idx;															//标记读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
};

int cgi_conn::m_epollfd = -1;

//注意：每次socket数据到达，都会从子进程发送过来，会重新初始化上面的变量！！！包括类的变量
void cgi_conn::init(int epollfd,int sockfd,const sockaddr_in& client_addr){
	m_epollfd = epollfd;
	m_sockfd = sockfd;
	m_address = client_addr;
	memset(m_buffer,0,BUFFER_SIZE);
	m_read_idx = 0;
}

void cgi_conn::process(){
	int idx = 0;
	int ret = -1;

	//开始循环读取和分析客户端的数据，注意：我们只管读取这一次数据，后面数据到达会从子进程发送过来，会重新初始化上面的变量！！！包括类的变量
	while(true){
		printf("666666666666666666");
		idx = m_read_idx;
		ret = recv(m_sockfd,m_buffer+idx,BUFFER_SIZE-idx-1,0);				//开始读取数据
		
		//如果读取操作出现错误，则关闭客户连接。如果没有数据读取，则退出循环
		if(ret < 0){
			if(errno != EAGAIN){
				removefd(m_epollfd,m_sockfd);								//进程池中实现的函数
			}
			break;
		}
		else if(ret == 0){													//对方关闭连接，则服务端也关闭连接
			removefd(m_epollfd,m_sockfd);
			break;
		}
		else
		{
			m_read_idx += ret;
			printf("user content is:%s\n",m_buffer);
			//遍历数据，进行解析，如果没有遇到“\r\n”，则想要读取更多的客户数据。如果遇到\r\n则标识获取到了我们要执行的程序名称，使用execl去执行
			for(;idx<m_read_idx;idx++){
				if((idx >= 1) && (m_buffer[idx-1] == '\r') && (m_buffer[idx] == '\n')){
					break;
				}
			}
			
			//如果没有遇到字符\r\n,则需要再读取更多的数据
			if(idx == m_read_idx){	
				continue;
			}

			//开始解析数据
			m_buffer[idx-1] = '\0';											//将\r\n中\r置为0，方面读取名称
			char *filename = m_buffer;

			//开始判断文件是否存在
			if(access(filename,F_OK) == -1){
				removefd(m_epollfd,m_sockfd);
				break;
			}

			//创建子进程来执行cgi程序
			ret = fork();
			if(ret == -1){
				removefd(m_epollfd,m_sockfd);
				break;
			}
			else if(ret > 0)												//父进程，关闭连接
			{
				removefd(m_epollfd,m_sockfd);
				break;
			}
			else															//子进程，将标准输出定向输出到m_sockfd,并执行CGI程序
			{
				close(STDOUT_FILENO);
				dup(m_sockfd);												//将程序输出，输出到socket描述符中
				execl(m_buffer,m_buffer,(char*)0);

				exit(0);
			}
		}

	}
}

int main(int argc,char *argv[])
{
	if(argc <= 2){
		printf("useage:%s ip_address port_number\n",basename(argv[0]));	//basename截取文件名
		return 1;
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);

	int listenfd = socket(AF_INET,SOCK_STREAM,0);
	assert(listenfd >= 0);

	int ret = 0;
	struct sockaddr_in address;
	bzero(&address,sizeof(address));

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	int reuse = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
	assert(ret != -1);

	ret = listen(listenfd,5);
	assert(ret != -1);

	processpool< cgi_conn > *pool = processpool< cgi_conn >::create(listenfd);
	if(pool){
		pool->run();
		delete pool;
	}

	close(listenfd);													//由主程序关闭listenfd

	return 0;
}