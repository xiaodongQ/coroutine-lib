#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {

// work flow
// 1 register one event -> 2 wait for it to ready -> 3 schedule the callback -> 4 unregister the event -> 5 run the callback
class IOManager : public Scheduler, public TimerManager 
{
public:
    enum Event 
    {
        NONE = 0x0,
        // READ == EPOLLIN
        READ = 0x1,
        // WRITE == EPOLLOUT
        WRITE = 0x4
    };

private:
    // 私有数据，赋值给`struct epoll_event`中的私有数据指针：event.data.ptr
    struct FdContext 
    {
        // 事件上下文定义，里面包含了3部分：协程调度器指针 + 协程 + 事件回调函数
        struct EventContext 
        {
            // scheduler
            Scheduler *scheduler = nullptr;
            // callback fiber
            std::shared_ptr<Fiber> fiber;
            // callback function
            std::function<void()> cb;
        };

        // read event context
        EventContext read;
        // write event context
        EventContext write;
        int fd = 0;
        // events registered
        Event events = NONE;
        std::mutex mutex;

        EventContext& getEventContext(Event event);
        // 重置传入的上下文
        void resetEventContext(EventContext &ctx);
        // 把触发的事件，通过协程或函数的方式 加到对应的调度器中（事件上下文中的scheduler指针指向的调度器）
        void triggerEvent(Event event);
    };

public:
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    // add one event at a time
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // delete event
    bool delEvent(int fd, Event event);
    // delete the event and trigger its callback
    // 删除事件并调用 triggerEvent，会把事件的回调处理作为一个任务传入调度器
    bool cancelEvent(int fd, Event event);
    // delete all events and trigger its callback
    bool cancelAll(int fd);

    static IOManager* GetThis();

protected:
    // 下面显式override的函数都重载掉
    void tickle() override;
    
    bool stopping() override;
    
    void idle() override;

    void onTimerInsertedAtFront() override;

    void contextResize(size_t size);

private:
    // epoll句柄，epoll_create/epoll_create1 创建
    int m_epfd = 0;
    // fd[0] read，fd[1] write
    // pipe管道，用于idle通知。
        // pipe管道是半双工的，1个管道需要2个文件fd，一读一写， POSIX标准规定fd[1]写，fd[0]读。
        // POSIX标准没有明确是否可以fd[0]写，fd[1]读。
    int m_tickleFds[2];
    std::atomic<size_t> m_pendingEventCount = {0};
    // C++17起才支持，读写锁
    std::shared_mutex m_mutex;
    // store fdcontexts for each fd
    std::vector<FdContext *> m_fdContexts;
};

} // end namespace sylar

#endif