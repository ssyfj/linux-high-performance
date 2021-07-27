#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <mutex>
#include <time.h>
#include <atomic>
#include <list>
#include <memory>

#define MAX_THREAD_NUM 1
#define FOR_LOOP_COUNT 10000000

static int counter = 0;							//计数
static pthread_mutex_t mutex;					//mutex锁,与无锁进行对比

static int s_count_push = 0;					//记录出入队列
static int s_count_pop = 0;

typedef void *(&thread_func_t)(void *argv);

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

template<typename ElemType>
struct qnode								//链表节点
{
	struct qnode* _next;
	ElemType _data;
};

template<typename ElemType>
class Queue
{
private:
	struct qnode<ElemType> *volatile _head = NULL;	//模拟队列，所以pop会改变Head
	struct qnode<ElemType> *volatile _tail = NULL;

public:
	Queue()
	{
		_head = _tail = new qnode<ElemType>;		//头节点
		_head->_next = NULL;
		_tail->_next = NULL;

		printf("Queue _head:%p\n",_head);
	}

public:							//普通操作，一会在外面调用时，使用加锁
	void push(const ElemType &e)					//尾插法
	{
		struct qnode<ElemType> *p = new qnode<ElemType>;
		p->_data = e;
		p->_next = NULL;

		printf("push i:%d\n", e);

		struct qnode<ElemType> *t = _tail;
		t->_next = p;
		_tail = p;
	}

	bool pop(ElemType &e)
	{
		struct qnode<ElemType> *p = _head;
		struct qnode<ElemType> *n = _head->_next;	//由于前面保持一个头节点，所以这个才是数据节点

		if(!n)										//没有数据
		{
			return false;
		}

		e = n->_data;
		_head->_next = n->_next;
		delete n;
		return true;
	}

public:						//无锁操作，使用原子操作CAS
	void push2(const ElemType &e)
	{
		struct qnode<ElemType> *p = new qnode<ElemType>;

		p->_next = NULL;
		p->_data = e;

		struct qnode<ElemType> *t = _tail;
		struct qnode<ElemType> *old_t = _tail;

		int count = 0;				//记录执行了多少次自旋
		do
		{
			while(t->_next != NULL)	//非空的时候要去更新t->_next
			{
				t = t->_next;		//去找最后的节点
			}

			if(count++ >= 1)
			{
				printf("push count:%d,t->next:%p\n", count,t->_next);
			}
		//将null置为p,加入节点
		}while(!__sync_bool_compare_and_swap(&t->_next,NULL,p));	//如果有其他节点执行了，则返回false，重新进入循环

		//将最后的节点标识_tail置为p节点
		__sync_bool_compare_and_swap(&_tail,old_t,p);			//置为最新插入的，感觉不对！！！！
	}

	bool pop2(ElemType &e)
	{
		struct qnode<ElemType> *p = NULL;
		struct qnode<ElemType> *n = NULL;

		int count = 0;
		do
		{
			p = _head;				//头节点，不存放数据
			if(p->_next == NULL)	//无数据
			{
				return false;
			}

			if(count++ >= 1)
			{
				printf("pop count:%d,p->_next:%p\n", count,p->_next);
			}
		//更新头节点位置
		}while(!__sync_bool_compare_and_swap(&_head,p,p->_next));	//这里和上面的pop不同，更新了头节点位置

		e = p->_next->_data;
		delete p;
		return true;
	}

	~Queue()
	{
		struct qnode<ElemType> *volatile tmp;
		while(_head)
		{
			tmp = _head->_next;
			printf("_head:%p\n",_head);
			delete _head;
			_head = tmp;
		}
	}
};

static std::list<int> s_list;


void *mutex_thread_push(void *argv)
{
  for (int i = 0; i < FOR_LOOP_COUNT; i++)
  {
	pthread_mutex_lock(&mutex);
    s_count_push++;
    s_list.push_back(i);
	pthread_mutex_unlock(&mutex);
  }
  return NULL;
}

void *mutex_thread_pop(void *argv)
{
  while (true)
  {
    int value = 0;
	pthread_mutex_lock(&mutex);
    if (s_list.size() > 0)
    {
      value = s_list.front();
      s_list.pop_front();
      s_count_pop++;
    }
	pthread_mutex_unlock(&mutex);
    if (s_count_pop >= FOR_LOOP_COUNT * MAX_THREAD_NUM)
    {
      printf("%s dequeue:%d\n", __FUNCTION__, value);
      break;
    }
  }
  printf("%s exit\n", __FUNCTION__);
  return NULL;
}

static Queue<int> s_queue;

//============测试加锁===========
void *mutex_thread_push1(void* argv)
{
	for(int i=0;i<FOR_LOOP_COUNT;i++)
	{
		pthread_mutex_lock(&mutex);

		s_count_push++;
		s_queue.push(i);

		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

void *mutex_thread_pop1(void *argv)
{
	while(true)	//也可以写为上面的for
	{
		int value = 0;
		pthread_mutex_lock(&mutex);

		if(s_queue.pop(value))
		{
			s_count_pop++;
		}

		pthread_mutex_unlock(&mutex);
		if(s_count_pop >= FOR_LOOP_COUNT*MAX_THREAD_NUM)
		{
			printf("%s dequeue:%d\n",__FUNCTION__,value);
			break;
		}
	}

	printf("mutex push:%d, pop:%d\n",s_count_push,s_count_pop);
	return NULL;
}

//============测试无锁===========
void *queue_free_thread_push(void *argv)
{
	for(int i=0;i<FOR_LOOP_COUNT;i++)
	{
		s_queue.push2(i);
		atomic_add(&s_count_push,1);
	}
	printf("free mutex push:%d\n",s_count_push);
	return NULL;
}


void *queue_free_thread_pop(void *argv)
{
	int last_value;
	while(true)
	{
		int value = 0;
		if(s_queue.pop2(value))
		{	
			last_value = value;
			s_count_pop++;
		}

		if(s_count_pop >= FOR_LOOP_COUNT*MAX_THREAD_NUM)
		{
			printf("%s dequeue:%d\n",__FUNCTION__,last_value);
			break;
		}
	}

	printf("free mutex push:%d, pop:%d\n",s_count_push,s_count_pop);
	return NULL;
}

int test_queue(thread_func_t func_push,thread_func_t func_pop,char **argv)
{
	clock_t start = clock();
	pthread_t tidPushArr[MAX_THREAD_NUM] = {0};

	for(int i=0;i<MAX_THREAD_NUM;i++)
	{
		int ret = pthread_create(&tidPushArr[i],NULL,func_push,argv);
		if(ret != 0)
		{
			printf("create push thread failed!\n");
		}
	}

	pthread_t tidPopArr[MAX_THREAD_NUM] = {0};
	for(int i=0;i<MAX_THREAD_NUM;i++)
	{
		int ret = pthread_create(&tidPopArr[i],NULL,func_pop,argv);
		if(ret != 0)
		{
			printf("create pop thread failed!\n");
		}
	}

	//回收线程
	for(int i=0;i<MAX_THREAD_NUM;i++)
	{
		pthread_join(tidPushArr[i],NULL);
	}

	for(int i=0;i<MAX_THREAD_NUM;i++)
	{
		pthread_join(tidPopArr[i],NULL);
	}

	clock_t end = clock();
	printf("spend clock :%ld,push:%d, pop:%d\n",(end-start)/CLOCKS_PER_SEC,
		s_count_push,s_count_pop);
	return 0;
}

int main(int argc,char* argv[])
{
	printf("Thread nums:%d\n\n",MAX_THREAD_NUM);

	for(int i=0;i<100;i++)
	{
		s_count_push = 0;
		s_count_pop = 0;
		printf("\n\n------------->i:%d\n\n",i);
		printf("use mutex queue ----------->\n");
		test_queue(mutex_thread_push,mutex_thread_pop,NULL);

		s_count_push = s_count_pop = 0;
		printf("\n\n------------->i:%d\n\n",i);
		printf("use free queue ----------->\n");
		test_queue(queue_free_thread_push,queue_free_thread_pop,NULL);
	}

	printf("finish\n");
	return 0;
}