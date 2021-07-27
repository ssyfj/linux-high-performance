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

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#define THREAD_NUMBER 2						//线程数量
#define THREAD_INCR_COUNT 100000			//循环递增次数

static int s_i = 0;							//静态全局变量,用于在原子操作中递增测试
static int s_j = 0;							//静态全局变量,用于在非原子、不加锁下递增测试

//非原子、也没有加锁，作为反派处理
static int add(int* value,int add)
{
	int old = *value;
	*value += add;
	return old;
}

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

/*
虽然看上去这并不是一句汇编,但实际上只有 : lock xaddl %2,%1,这句汇编是临界区,
也就是说,这句话锁上了内存,然后将pval的虚拟地址对应的物理地址上的数据增加了num。
后面的 return old,只不过把最后结果返回,但是实际上返回值为Null也完全不会影响,因为早已经通过指针修改了那块地址的内容了。
*/


void *thread_increase(void* arg)
{
	for(int i=0;i<THREAD_INCR_COUNT;i++)
	{
		atomic_add(&s_i,1);
		add(&s_j,1);
	}

	printf("thread finish and exit\n");
	pthread_exit(NULL);
}


int main(int argc,char* argv[])
{
	pthread_t threadArr[THREAD_NUMBER];		//多线程
	for(int i=0;i<THREAD_NUMBER;i++)
	{
		pthread_create(&threadArr[i],NULL,thread_increase,NULL);	//创建线程
	}

	for(int i=0;i<THREAD_NUMBER;i++)
	{
		pthread_join(threadArr[i],NULL);	//线程退出
	}

	printf("atomic s_i actual:%d, expected:%d\n",s_i,THREAD_INCR_COUNT*THREAD_NUMBER);
	printf("s_j actual:%d, expected:%d\n",s_j,THREAD_INCR_COUNT*THREAD_NUMBER);
	
	return 0;
}

//gcc ./01原子操作.c -o 01atomic -lpthread 