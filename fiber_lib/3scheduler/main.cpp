#include <chrono>
#include <iostream>
#include <iomanip>
#include <ctime>

#include "scheduler.h"

using namespace sylar;

std::string NowTime()
{
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(local_time, "%F %T");
    return oss.str();
}

static unsigned int test_number;
std::mutex mutex_cout;
void task()
{
    {
        // 防止并发日志打印错乱
        std::lock_guard<std::mutex> lock(mutex_cout);
        std::cout << "task " << test_number ++ << " is under processing in thread: " << Thread::GetThreadId() << std::endl;		
    }
    sleep(1);
}

int main(int argc, char const *argv[])
{
    {
        // 构造函数：Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name="Scheduler");
        // 此处有3个线程用于协程调度，由于use_caller指定了true，所以只会额外创建2个线程
        std::shared_ptr<Scheduler> scheduler = std::make_shared<Scheduler>(3, true, "scheduler_1");
        
        scheduler->start();

        sleep(2);

        std::cout << "now: " << NowTime() << ", begin post\n\n"; 
        for(int i=0;i<5;i++)
        {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        sleep(6);

        std::cout << "now: " << NowTime() << ", post again\n\n"; 
        for(int i=0;i<15;i++)
        {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        sleep(3);
        scheduler->stop();
    }
    return 0;
}