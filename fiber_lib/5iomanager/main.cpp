#include "ioscheduler.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <cerrno>

using namespace sylar;

// 临时新增仅用于打印
std::mutex mutex_cout;

char recv_data[4096];

const char data[] = "GET / HTTP/1.0\r\n\r\n"; 

int sock;

void func()
{
    recv(sock, recv_data, 4096, 0);
    std::cout << recv_data << std::endl << std::endl;
}

void func2()
{
    send(sock, data, sizeof(data), 0);
}

int main(int argc, char const *argv[])
{
    IOManager manager(2);

    sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(80);  // HTTP 标准端口
    // 百度网站的ip
    server.sin_addr.s_addr = inet_addr("103.235.46.96");

    fcntl(sock, F_SETFL, O_NONBLOCK);

    connect(sock, (struct sockaddr *)&server, sizeof(server));
    
    // 发送 GET请求
    manager.addEvent(sock, IOManager::WRITE, &func2);
    manager.addEvent(sock, IOManager::READ, &func);

    {
        std::lock_guard<std::mutex> lk(mutex_cout);
        std::cout << "event has been posted\n\n";
    }

    // 等待一会，防止主线程退出提前触发调度线程等的析构，影响流程
    sleep(1);

    return 0;
}