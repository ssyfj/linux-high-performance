#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
 
using namespace std;
 
int main(int argc,char** argv)
{
    int clientfd;                   //文件描述符
    struct sockaddr_in serv_addr;   //目的服务端地址结构体

    char buf[1024];                 //用于发送和接收
    int buflen = 0;

 
    memset(&serv_addr,0,sizeof(serv_addr));
 
    if(argc!=3)
    {
        cout<<"Input error! Usage should be : "<<argv[0]<<"  xxx.xxx.xxx.xxx(ip)  1234(port)"<<endl;
        return 0;
    }
 
    if((clientfd = socket(AF_INET,SOCK_STREAM,0)) == -1)  //创建套接字
    {
        cout<<"creat socket failed : "<<strerror(errno)<<endl;
        return 0;
    }
    
    //将目的服务端的地址信息赋值给地址结构体
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[2]));
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
 
    cout<<"try to connect ... "<<endl;
 
    //通过套接字发起连接请求，成功后clitfd套接字则表示此次成功的连接
    if(connect(clientfd,(struct sockaddr*)& serv_addr,sizeof(serv_addr)) == -1)
    {
        cout<<"connet failed : "<<strerror(errno)<<endl;
        return 0;
    }
 
    cout<<"connect success !"<<endl;

    //进程池实现有问题，所以只能获取一次数据
 
    cout<<"(Client)send filename: ";
    memset(buf,0,sizeof(buf));
    cin >> buf;
    int len = strlen(buf);
    buf[len] = '\r';
    buf[len+1] = '\n';

    write(clientfd,buf,strlen(buf));
    memset(buf,0,sizeof(buf));
    read(clientfd,buf,sizeof(buf));
    cout<<"Server return:";
    cout<<buf<<endl;
 
    close(clientfd);
}