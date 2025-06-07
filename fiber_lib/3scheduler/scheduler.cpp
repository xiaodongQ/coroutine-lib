#include "scheduler.h"

static bool debug = false;

namespace sylar {

// 每个线程都有一个调度器
static thread_local Scheduler* t_scheduler = nullptr;

Scheduler* Scheduler::GetThis()
{
    return t_scheduler;
}

void Scheduler::SetThis()
{
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name):
m_useCaller(use_caller), m_name(name)
{
    assert(threads>0 && Scheduler::GetThis()==nullptr);

    SetThis();

    Thread::SetName(m_name);

    // 使用主线程当作工作线程（也用于协程调度）
    if(use_caller)
    {
        // 需要创建的工作线程数-1，当前线程也占了一个工作线程
        threads --;

        // 创建主协程，并默认会设置其为线程的调度协程（不过下面重新设置了调度协程）
        Fiber::GetThis();

        // 创建调度协程
            // 构造函数签名：Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
            // 此处创建单独的一个调度协程，不参与协程任务的上下文切换
        m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false -> 该调度协程退出后将返回主协程
        // 并把新建的这个调度协程设置给协程所在的线程，而不是用默认情况下线程中的主协程
        Fiber::SetSchedulerFiber(m_schedulerFiber.get());
        
        // 记录一下主线程，用以和其他调度线程区分开
        m_rootThread = Thread::GetThreadId();
        m_threadIds.push_back(m_rootThread);
    }

    m_threadCount = threads;
    if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler()
{
    assert(stopping()==true);
    if (GetThis() == this) 
    {
        t_scheduler = nullptr;
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

void Scheduler::start()
{
    // 锁范围较大，线程池队列m_threads、任务队列m_tasks、线程id队列 都由该锁做竞争保护
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping)
    {
        std::cerr << "Scheduler is stopped" << std::endl;
        return;
    }

    assert(m_threads.empty());
    m_threads.resize(m_threadCount);
    for(size_t i=0;i<m_threadCount;i++)
    {
        // 创建线程
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        // 线程tid
        m_threadIds.push_back(m_threads[i]->getId());
    }
    if(debug) std::cout << "Scheduler::start() success\n";
}

// 线程函数
void Scheduler::run()
{
    int thread_id = Thread::GetThreadId();
    if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;
    
    //set_hook_enable(true);

    // 设置线程局部变量t_scheduler指向本调度类实例
    // 由于使用时只会创建一个Scheduler实例，所以各线程里指针虽然各自独立，但指向都是本调度类
        // 不同线程此处设置的this不同？？？ 答：不会，是一样的
    SetThis();

    // 运行在新创建的线程 -> 需要创建主协程。
    // 即不是主线程时，通过Fiber::GetThis()创建该线程的主协程
    if(thread_id != m_rootThread)
    {
        // 里面除了创建主协程，还指定其所在线程的调度协程也为该主协程
        Fiber::GetThis();
    }

    // 创建idle协程，协程函数`Scheduler::idle`
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
    ScheduleTask task;
    
    while(true)
    {
        task.reset();
        bool tickle_me = false;

        {
            // 保护任务队列，取出任务后就解锁。后面具体执行任务时不在锁中。
            std::lock_guard<std::mutex> lock(m_mutex);
            // 遍历任务队列，从中获取一个协程任务
            auto it = m_tasks.begin();
            // 1 遍历任务队列
            while(it!=m_tasks.end())
            {
                // 添加协程任务时，可以指定协程由哪个线程来运行（本目录下的示例中默认-1，不做指定）
                if(it->thread!=-1 && it->thread!=thread_id)
                {
                    it++;
                    tickle_me = true;
                    continue;
                }

                // 2 取出任务
                assert(it->fiber||it->cb);
                task = *it;
                m_tasks.erase(it); 
                m_activeThreadCount++;
                // 只获取一个任务就退出循环，所以不会出现一个线程一直占用任务队列的情况
                break;
            }
            // 即 tickle_me |= (it != m_tasks.end());，若获取一个任务后队列中还有任务，则tickle唤醒其他线程
            tickle_me = tickle_me || (it != m_tasks.end());
        }

        if(tickle_me)
        {
            // 但其实这里是空实现，不做任何操作。具体使用到时看io调度协程里的tickle处理。
            tickle();
        }

        // 3 执行任务
        // 即可以是协程，也可以是函数
        if(task.fiber)
        {
            {
                // 协程中的锁？有必要？
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
                if(task.fiber->getState()!=Fiber::TERM)
                {
                    // 协程恢复，执行对应的协程函数
                    task.fiber->resume();
                }
            }
            m_activeThreadCount--;
            task.reset();
        }
        else if(task.cb)
        {
            // 根据函数构造一个协程，并恢复协程执行
            std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
                cb_fiber->resume();
            }
            m_activeThreadCount--;
            task.reset();
        }
        // 4 无任务 -> 执行空闲协程
        else
        {
            // 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if (idle_fiber->getState() == Fiber::TERM) 
            {
                if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
            m_idleThreadCount++;
            // 上述idle协程创建时，绑定的协程函数为：Scheduler::idle，其中做sleep(1)后就挂起切换
            idle_fiber->resume();
            m_idleThreadCount--;
        }
    }
}

void Scheduler::stop()
{
    if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
    
    if(stopping())
    {
        return;
    }

    m_stopping = true;	

    if (m_useCaller) 
    {
        // 当使用了caller线程来调度时（调度类主线程作为一个调度线程），只能由caller线程来执行stop
        assert(GetThis() == this);
    } 
    else 
    {
        // 没搞懂，不全是 Scheduler实例的指针吗？
            // 如果主线程（caller线程）不作为其中一个调度线程，则
        assert(GetThis() != this);
    }
    
    for (size_t i = 0; i < m_threadCount; i++) 
    {
        tickle();
    }

    if (m_schedulerFiber) 
    {
        tickle();
    }

    if(m_schedulerFiber)
    {
        m_schedulerFiber->resume();
        if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
    }

    std::vector<std::shared_ptr<Thread>> thrs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        thrs.swap(m_threads);
    }

    for(auto &i : thrs)
    {
        i->join();
    }
    if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
}

void Scheduler::tickle()
{
}

void Scheduler::idle()
{
    while(!stopping())
    {
        if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
        sleep(1);	
        // 挂起当前正在执行的协程，切换到调度协程
        Fiber::GetThis()->yield();
    }
}

bool Scheduler::stopping() 
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // 当前任务队列为空 且 没有活跃的执行任务线程，且m_stopping已经设置true，才返回true，表示可以析构了
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}


}