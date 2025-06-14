#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;
// 临时新增仅用于打印
extern std::mutex mutex_cout;

namespace sylar {

IOManager* IOManager::GetThis() 
{
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) 
{
    assert(event==READ || event==WRITE);    
    switch (event) 
    {
    case READ:
        return read;
    case WRITE:
        return write;
    }
    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx) 
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

// no lock
void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    assert(events & event);

    // delete event 
    events = (Event)(events & ~event);
    
    // trigger
    EventContext& ctx = getEventContext(event);
    if (ctx.cb) 
    {
        // call ScheduleTask(std::function<void()>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.cb);
    } 
    else 
    {
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }

    // reset event context
    resetEventContext(ctx);
    return;
}

// IO调度构造函数
IOManager::IOManager(size_t threads, bool use_caller, const std::string &name): 
Scheduler(threads, use_caller, name), TimerManager()
{
    // create epoll fd
    // 参数自Linux 2.6.8会被忽略，但要>0
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // create pipe
    // 创建一个pipe管道时，需要2个文件fd，一读一写。一般是fd[1]写，fd[0]读
    int rt = pipe(m_tickleFds);
    assert(!rt);

    // add read event to epoll
    epoll_event event;
    // 注册读事件，且使用边缘触发（来事件后需一次性读取完对应数据）
    event.events  = EPOLLIN | EPOLLET; // Edge Triggered
    event.data.fd = m_tickleFds[0];

    // non-blocked
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);
    // 此处注册的句柄为pipe的 fd[0]，用于读
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    // 初始化 FdContext数组
    contextResize(32);

    // 这里的start()没有在IOManager类中重载，用的是父类Scheduler中的函数实现。
    // 里面会初始化线程池，创建threads个线程都用于协程调度
    start();
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]) 
        {
            delete m_fdContexts[i];
        }
    }
}

// no lock
void IOManager::contextResize(size_t size) 
{
    // 调整vector大小，下面会初始化创建FdContext结构
    m_fdContexts.resize(size);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]==nullptr) 
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) 
{
    // attemp to find FdContext
    FdContext *fd_ctx = nullptr;
    
    // 读锁
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        // fd作为数组下标，好处是便于索引查找，不过没使用的fd下标存在一些浪费
        fd_ctx = m_fdContexts[fd];
        // 解除读锁。加锁只为了访问 m_fdContexts
        read_lock.unlock();
    }
    else
    {
        // 先解除上面的读锁
        read_lock.unlock();
        // 写锁
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        // fd作下标超出vector容量，则根据 fd*1.5 来扩容，而不是之前的capacity
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    // fd上下文整体加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // the event has already been added
    if(fd_ctx->events & event)
    {
        return -1;
    }

    // add new event
    // 原来的事件不是NONE（0），则op是修改，按位或增加本次要注册的事件
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    // 边缘触发模式
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    // 事件注册
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    // 注册的事件计数+1
    ++m_pendingEventCount;

    // update fdcontext
    // 更新成 Event 里限定的3个枚举（无事件、读、写），去掉了前面 按位| 的边缘触发标志
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // update event context
    // 根据读写类型获取对应的 FdContext，设置其信息：调度类指针 和 协程/回调函数
    // fd_ctx指针设置给了上述 epoll_event 中的私有数据指针，只是个指针。此处更新fd_ctx指向结构的内容，前后顺序没影响
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) 
    {
        // 如果传入了函数对象，则记录在EventContext中
        event_ctx.cb.swap(cb);
    } 
    else 
    {
        // 没传函数则创建一个新协程（新协程默认是RUNNING），并记录在EventContext中
        event_ctx.fiber = Fiber::GetThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;
}

bool IOManager::delEvent(int fd, Event event) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    // 删除注册的事件
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }


    --m_pendingEventCount;

    // update fdcontext
    fd_ctx->events = new_events;

    // update event context
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

bool IOManager::cancelEvent(int fd, Event event) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount;

    // update fdcontext, event context and trigger
    fd_ctx->triggerEvent(event);    
    return true;
}

bool IOManager::cancelAll(int fd) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // none of events exist
    if (!fd_ctx->events) 
    {
        return false;
    }

    // delete all events
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    // update fdcontext, event context and trigger
    if (fd_ctx->events & READ) 
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if (fd_ctx->events & WRITE) 
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

void IOManager::tickle() 
{
    // no idle threads
    if(!hasIdleThreads()) 
    {
        return;
    }
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping() 
{
    uint64_t timeout = getNextTimer();
    // no timers left and no pending events left with the Scheduler::stopping()
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}


void IOManager::idle()
{
    static const uint64_t MAX_EVNETS = 256;
    // 创建临时的epoll_event数组（没用vector<epoll_event>方式），用于接收epoll_wait返回的就绪事件，每次最大256个
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

    while (true) 
    {
        if(debug) {
            std::lock_guard<std::mutex> lk(mutex_cout);
            std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl; 
        }

        if(stopping()) 
        {
            if(debug) {
                std::lock_guard<std::mutex> lk(mutex_cout);
                std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            }
            break;
        }

        // blocked at epoll_wait
        int rt = 0;
        // 此处while循环为了结合定时器做超时检查，等待epoll事件超时触发，有触发则break此处的while(true)
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000;
            // 返回堆中最近的超时时间，还有多少ms到期（set里第一个成员时间最小，最先到期）
            uint64_t next_timeout = getNextTimer();
            next_timeout = std::min(next_timeout, MAX_TIMEOUT);

            // 获取events原始指针，接收epoll触发的事件。此处阻塞等待事件发生，避免idle协程空转
            rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);
            // EINTR -> retry
            if(rt < 0 && errno == EINTR)
            {
                continue;
            }
            else
            {
                // 只要有任何事件通知就break出小循环
                break;
            }
        // 怎么}后还加了个`;` ？ sylar里是 `do{} while(true);`，这里“借鉴”不完全
        // };
        }

        // collect all timers overdue
        // 既然有超时触发的事件，此处捞取超时定时器的回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if(!cbs.empty()) 
        {
            // 把这些回调函数都加入到协程调度器的任务队列里
            for(const auto& cb : cbs) 
            {
                // 如果是第一次添加任务，则会tickle()一次：其中会向管道的fd[1]进行一次write（fd[0]就可以收到epoll读事件）
                scheduleLock(cb);
            }
            cbs.clear();
        }
        
        // collect all events ready
        for (int i = 0; i < rt; ++i) 
        {
            epoll_event& event = events[i];

            // tickle event
            // pipe管道，则做read，由于是边缘触发，此处while处理。虽然fd[1] write时也只是写了1个字符。
            if (event.data.fd == m_tickleFds[0]) 
            {
                uint8_t dummy[256];
                // edge triggered -> exhaust
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // other events
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // convert EPOLLERR or EPOLLHUP to -> read or write event
            if (event.events & (EPOLLERR | EPOLLHUP)) 
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            // events happening during this turn of epoll_wait
            // 事件只保留读和写类型
            int real_events = NONE;
            if (event.events & EPOLLIN) 
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) 
            {
                real_events |= WRITE;
            }

            if ((fd_ctx->events & real_events) == NONE) 
            {
                continue;
            }

            // delete the events that have already happened
            int left_events = (fd_ctx->events & ~real_events);
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // schedule callback and update fdcontext and event context
            // 根据类型触发相应的事件回调处理
            if (real_events & READ) 
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for

        Fiber::GetThis()->yield();
  
    } // end while(true)
}

void IOManager::onTimerInsertedAtFront() 
{
    tickle();
}

} // end namespace sylar