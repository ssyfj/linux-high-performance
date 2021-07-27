//多个线程运行的时候，共享了同一块资源（临界资源），每个线程访问这个临界资源时，需要加上一把锁


/*
加锁mutex：如果获取不到锁就休眠，放在 锁“等待”的队列上，让出CPU。涉及到进程上下⽂的切换，效率不⾼-----其中去尝试获取锁并不耗时，耗时的是等待获取锁的时间（CPU切换）
自旋锁：获取不到锁，继续去检测。那么⾃旋锁则不是⼀种休眠等待的⽅式，⽽是⼀种忙等待的过程。⾃旋锁的pthread_spin_lock⾥有⼀个死循环，这个死循环⼀直检查锁的状态，如果是lock 状态，则继续执⾏死循环，否则上锁，结束死循环。
原子操作：单一指令，不可分割。比如CAS就是一条指令
无锁CAS：高并发
*/

/*
加锁mutex：情况比较复杂的
自旋锁：对于比较简单的多行代码
原子操作：单一指令，指令系统中有这条指令，才能进行原子操作。红黑树加锁小粒度使用cas
*/


/*
======================================内存模型======================================
https://www.codedump.info/post/20191214-cxx11-memory-model-1/
https://www.codedump.info/post/20191214-cxx11-memory-model-2/

1.Sequential Consistency (顺序一致性）
----顺序一致性实际上是一种强一致性，可以想象成整个程序过程中由一个开关来选择执行的线程，这样才能同时保证顺序一致性的两个条件。
这样实际上还是相当于同一时间只有一个线程在工作，这种保证导致了程序是低效的，无法充分利用上多核的优点。

memory_order_seq_cst：顺序严格一致，但是效率太低，没有利用到多线程并发优点


2.全存储排序（Total Store Ordering, 简称TSO）
----有一些CPU架构，在处理核心中增加写缓存，一个写操作只要写入到本核心的写缓存中就可以返回，在这种结构下，SC所不允许的一些操作可能会出现。

SC是最简单直白的内存模型，TSO在SC的基础上，加入了写缓存，写缓存的加入导致了一些在SC条件下不可能出现的情况也成为了可能。

memory_order_acquire：用来修饰一个读操作，表示在本线程中，所有后续的关于此变量的内存操作（不同线程）都必须在本条原子操作完成后执行。---用到了内存栅栏
memory_order_release：用来修饰一个写操作，表示在本线程中，所有之前的针对该变量的内存操作（不同线程）完成后才能执行本条原子操作。---用到了内存栅栏
memory_order_acq_rel：同时包含memory_order_acquire和memory_order_release标志。

对于多个变量交叉处理，还是可能出现问题！！要注意合理设置多个变量之间的顺序问题即可解决相关问题！！！


即便如此，以上两种内存模型都没有改变单线程执行一个程序时的执行顺序。在这里要讲的松弛型内存模型，则改变了程序的执行顺序。
3.松弛型内存模型（Relaxed memory models）
----在松散型的内存模型中，编译器可以在满足程序单线程执行结果的情况下进行重排序（reorder）

memory_order_relaxed：针对一个变量的读写操作是原子操作；不同线程之间针对该变量的访问操作先后顺序不能得到保证，即有可能乱序。

上面对2.Acquire-Release模型的分析可以知道，虽然可以使用这个模型做到两个线程之间某些操作的synchronizes-with关系，然后这个粒度有些过于大了。
4.memory_order_consume：在很多时候，线程间只想针对有依赖关系的操作进行同步，除此之外线程中的其他操作顺序如何无所谓。

以上可以对比Acquire-Release以及Release-Consume两个内存模型，可以知道：

（1）Acquire-Release能保证不同线程之间的Synchronizes-With关系，这同时也约束到同一个线程中前后语句的执行顺序。
（2）而Release-Consume只约束有明确的carry-a-dependency关系的语句的执行顺序，同一个线程中的其他语句的执行先后顺序并不受这个内存模型的影响。


内存栅栏：
由于有了缓冲区的出现，导致一些操作不用到内存就可以返回继续执行后面的操作（2.TSO），为了保证某些操作必须是写入到内存之后才执行，
就引入了内存栅栏（memory barrier，又称为memory fence）操作。
内存栅栏指令保证了，在这条指令之前所有的内存操作的结果，都在这个指令之后的内存操作指令被执行之前，写入到内存中。
也可以换另外的角度来理解内存栅栏指令的作用：显式的在程序的某些执行点上保证SC。-----要执行内存栅栏之后的指令，那么之前的结果必须先被写入了内存之中！！！

*/

//主要测试：mutex、自旋锁、原子操作、以及内存模型的性能

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <atomic>

#define MAX_THREAD_NUM 2
#define FOR_LOOP_COUNT 2000						//测试循环次数
#define	FOR_ADD_COUNT 100000					//用来调试锁的粒度，查看各个锁适用的范围

static int counter = 0;
static pthread_spinlock_t spinlock;				//自旋锁
static pthread_mutex_t mutex;					//mutex锁

typedef void *(*thread_func_t)(void *argv);		//注意：函数指针 返回类型 (*函数名)(参数)，这里返回类型是void *而已

void do_for_add(int count)						//通过修改count---FOR_ADD_COUNT，来调整函数执行的粒度
{
	for(int i=0;i<count;i++)
	{
		counter ++;
	}
}


//内存模型,模拟自旋锁
class atomic_flag_spinlock
{
	std::atomic_flag flag;						//原子布尔类型，免锁
public:
	atomic_flag_spinlock():
		flag(ATOMIC_FLAG_INIT)
	{}

	void lock()
	{
		while(flag.test_and_set(std::memory_order_acquire));
	}

	void unlock()
	{
		flag.clear(std::memory_order_release);
	}
};

static atomic_flag_spinlock s_atomic_flag_spinlock;

//实现原子操作为*value += add
static int atomic_add(int *value,int add)
{
	int old;
    //"__asm__"表示后面的代码为内嵌汇编
    //"__volatile__"表示编译器不要优化代码(不需要放入寄存器，就放入内存），后面的指令保留原样，"volatile"是它的别名
	//总线锁 : 就是使用处理器提供的一个LOCK信号，当一个处理器在总线上输此信号时，其他处理器的请求将被阻塞住，那么该处理器可以独占共享内存。　
	//在进行操作 (读取对应内存内容) 之前,先使用总线锁锁定这段内存(例如 : 1111 ~ 1113)。使得其他CPU无法对该内存操作,当操作 （将结果写入对应内存） 结束,就释放这段内存的总线锁。
	__asm__ volatile(
		"lock; xaddl %2, %1;"				//%1 = %1 + %2 ,其中 %0 %1代表第0个参数、第1个参数 .... , 参数看后面语句中出现()的先后
		: "=a" (old)						//old是%0，（*value）是%1 add是%2 ； 其中 =表示是输出参数，a表示rax寄存器,会将修改之前的值放入old中用于返回
		: "m" (*value), "a" (add)			//m表示内存，a表示寄存器。注意：与变量()之间 间隔 空格
		: "cc", "memory"                    //cc表示操作方法，memory表示需要使用内存
		);

	return old;
}

//========================开始测试===========================

//mutex加锁
void *mutex_thread_main(void *argv)
{
	for(int i = 0;i<FOR_LOOP_COUNT;i++)
	{
		pthread_mutex_lock(&mutex);

		do_for_add(FOR_ADD_COUNT);			//粒度FOR_ADD_COUNT
		
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

//原子模型
void *atomic_thread_main(void *argv)
{
    for (int i = 0; i < FOR_LOOP_COUNT*FOR_ADD_COUNT; i++)
    {
        atomic_add(&counter, 1);
    }
    return NULL;
}

//自旋锁
void *spin_thread_main(void *argv)
{
	for(int i = 0;i<FOR_LOOP_COUNT;i++)
	{
		pthread_spin_lock(&spinlock);

		do_for_add(FOR_ADD_COUNT);			//粒度FOR_ADD_COUNT
		
		pthread_spin_unlock(&spinlock);
	}
	return NULL;	
}

//内存模型
void *atomic_flag_spinlock_thread_main(void *argv)
{
    for (int i = 0; i < FOR_LOOP_COUNT; i++)
    {
        s_atomic_flag_spinlock.lock();
        do_for_add(FOR_ADD_COUNT);
        s_atomic_flag_spinlock.unlock();
    }
    return NULL;
}

//=================调用函数==============
int test_lock(thread_func_t func, char **argv)
{
    clock_t start = clock();
    pthread_t tid[MAX_THREAD_NUM] = {0};
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        int ret = pthread_create(&tid[i], NULL, func, argv);
        if (0 != ret)
        {
            printf("create thread failed\n");
        }
    }
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        pthread_join(tid[i], NULL);
    }
    clock_t end = clock();
    printf("spend clock : %ld, ", (end - start) / CLOCKS_PER_SEC);
    return 0;
}

// 多尝试几次
int main(int argc, char **argv)
{
    printf("THREAD_NUM:%d\n\n", MAX_THREAD_NUM);
    counter = 0;
    printf("use mutex ----------->\n");     //5
    test_lock(mutex_thread_main, NULL);
    printf("counter = %d\n", counter);

    counter = 0;
    printf("\nuse atomic ----------->\n");   //2
    test_lock(atomic_thread_main, NULL);
    printf("counter = %d\n", counter);

    counter = 0;
    printf("\nuse spin ----------->\n");    //3
    pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
    test_lock(spin_thread_main, NULL);
    printf("counter = %d\n", counter);

    counter = 0;
    printf("\nuse atomic_flag_spinlock ----------->\n");    //6
    test_lock(atomic_flag_spinlock_thread_main, NULL);
    printf("counter = %d\n\n\n", counter);

    return 0;
}

//g++ ./02锁性能与内存模型.cpp -o 02lock -lpthread -std=c++11

/*
===================test 1======================

#define MAX_THREAD_NUM 2
#define FOR_LOOP_COUNT 2000                                             //测试循环次数
#define FOR_ADD_COUNT 500000                                    //用来调试锁的粒度，查看各个锁适用的范围


use mutex ----------->
spend clock : 6, counter = 2000000000

use atomic ----------->
spend clock : 28, counter = 2000000000

use spin ----------->
spend clock : 11, counter = 2000000000

use atomic_flag_spinlock ----------->
spend clock : 10, counter = 2000000000

===================test 2======================


#define MAX_THREAD_NUM 2
#define FOR_LOOP_COUNT 2000                                             //测试循环次数
#define FOR_ADD_COUNT 5000                                      //用来调试锁的粒度，查看各个锁适用的范围


use mutex ----------->
spend clock : 0, counter = 20000000

use atomic ----------->
spend clock : 0, counter = 20000000

use spin ----------->
spend clock : 0, counter = 20000000

use atomic_flag_spinlock ----------->
spend clock : 0, counter = 20000000

===================test 3======================

#define MAX_THREAD_NUM 2
#define FOR_LOOP_COUNT 2000                                             //测试循环次数
#define FOR_ADD_COUNT 50000                                     //用来调试锁的粒度，查看各个锁适用的范围

use mutex ----------->
spend clock : 1, counter = 200000000

use atomic ----------->
spend clock : 3, counter = 200000000

use spin ----------->
spend clock : 0, counter = 200000000

use atomic_flag_spinlock ----------->
spend clock : 1, counter = 200000000
*/