#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>								//可以用于处理变长参数

#include <pthread.h>

#define MAX_THREADS_COUNT	80					//定义线程池最大线程数量
#define MAX_JOBS_COUNT		1000				//定义最大任务数量

//宏定义：链表插入，头插法;写成do...while可以防止宏定义导致的问题，使得插入代码块
//注意：虽然是双向链表，但是我们这里先不设置list->prev,因为可能list为空，会出错。
//具体设置在
#define LL_ADD(item,list) do {					\
	item->prev = NULL;							\
	item->next = list;							\
	if(list != NULL) list->prev = item;			\
	list = item;								\
}while(0)

//宏定义：链表list中移除节点item，实际上就是从头部移除
#define LL_REMOVE(item,list) do {							\
	if(item->prev != NULL) item->prev->next = item->next;	\
	if(item->next != NULL) item->next->prev = item->prev;	\
	if(list == item) list = item->next;						\
	item->prev = item->next = NULL;							\
}while(0)

//=========================定义线程和任务=========================

//定义线程信息,用于工作
typedef struct NWORKER {
	pthread_t thread;							//类似于线程id
	int terminate;								//线程通过这个标识来决定是否退出
	struct NWORKQUEUE *workqueue;				//线程所属的线程池信息
	struct NWORKER *prev;						//链表前指针
	struct NWORKER *next;						//链表后指针
} nWorker;

//定义job任务，线程通过获取job链表中的任务进行执行
typedef struct NJOB {
	void (*job_function)(struct NJOB *job);		//JOB任务要去执行的函数,之所以传入NJOB参数，因为NJOB中包含了函数想要的数据
	void *user_data;
	struct NJOB *prev;
	struct NJOB *next;
} nJob;

//=========================定义线程池=========================
typedef struct NWORKQUEUE {
	struct NWORKER *workers;					//线程池中线程链表
	struct NJOB *waiting_jobs;					//待处理的任务链表
	pthread_mutex_t jobs_mtx;					//线程锁，只有一个线程去读取任务，不允许多个线程读取到一个任务
	pthread_cond_t jobs_cond;					//条件变量，用于通知任务产生
} nWorkQueue;

typedef nWorkQueue nThreadPool;					//线程池

//=========================线程池的实现：包括线程池创建、线程执行方法、job任务添加=========================

//线程工作方法：线程创建之后会开始执行该函数
//在这个方法中：主要实现对任务的处理，在线程池中会一直循环去获取任务
static void *workerThread(void *ptr){
	nWorker *worker = (nWorker *)ptr;			//传递的参数，是nWorker类型

	while(1){
		//要读取任务先进行加锁
		pthread_mutex_lock(&worker->workqueue->jobs_mtx);

		while(worker->workqueue->waiting_jobs == NULL){				//任务为空，则一直循环读取
			if(worker->terminate)									//判断是否应该退出,线程结束
				break;

			//条件变量，会先进行解锁操作，然后等待信号量到达，之后进行加锁操作
			pthread_cond_wait(&worker->workqueue->jobs_cond,&worker->workqueue->jobs_mtx);
		}

		//退出循环，标识有信号量到达，有新的任务被加入
		//还是需要判断退出标识
		if(worker->terminate){
			pthread_mutex_unlock(&worker->workqueue->jobs_mtx);		//先进行解锁
			break;													//退出循环,线程结束
		}

		//下面开始获取任务，是在加锁（前面实现）的情况下进行的
		nJob *job = worker->workqueue->waiting_jobs;
		if(job != NULL){
			LL_REMOVE(job,worker->workqueue->waiting_jobs);			//移除job
		}
		
		//开始解锁
		pthread_mutex_unlock(&worker->workqueue->jobs_mtx);

		//注意：尽可能保持加锁的粒度足够小。所以任务的执行放在外面即可
		if(job == NULL)
			continue;

		job->job_function(job);										//传入job数据,给执行任务		
	}

	//开始释放资源
	free(worker);
	pthread_exit(NULL);												//线程退出
}

/*
线程池的创建
参数1：由调用该函数的方法传入，参数实际存放在栈中，所以不需要我们去释放
*/
int threadPoolCreate(nThreadPool *workqueue, int numWorkers){
	if(numWorkers < 1){
		numWorkers = 1;
	}
	
	memset(workqueue,0,sizeof(nThreadPool));	//初始化线程池

	pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
	memcpy(&workqueue->jobs_cond,&blank_cond,sizeof(workqueue->jobs_cond));

	pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
	memcpy(&workqueue->jobs_mtx,&blank_mutex,sizeof(workqueue->jobs_mtx));

	for(int i = 0;i < numWorkers;i++){
		//初始化线程worker空间
		nWorker *worker = (nWorker*)malloc(sizeof(nWorker));
		if(worker == NULL){
			perror("malloc error!\n");
			return -1;
		}
		memset(worker,0,sizeof(nWorker));

		//初始化worker数据结构
		worker->workqueue = workqueue;

		/*
		线程创建：pthread_create
		参数1：新创建的线程ID指向的内存单元。
		参数2：线程属性，默认为NULL。比如可以设置线程分离。
		参数3：新创建的线程从参数3函数的地址开始运行。
		参数4：默认为NULL。若上述函数需要参数，将参数放入结构中并将地址作为arg传入。
		*/

		int ret = pthread_create(&worker->thread,NULL,workerThread,(void *)worker);
		if(ret){
			perror("pthread_create error!\n");
			free(worker);						//对于其他线程worker结构体的释放由线程内部退出时，释放
			return -1;
		}

		LL_ADD(worker,worker->workqueue->workers);
	}

	return 0;
}

//为线程池中添加任务
void threadPoolQueue(nThreadPool *workQueue,nJob *job){
	pthread_mutex_lock(&workQueue->jobs_mtx);	//先进行加锁操作

	LL_ADD(job,workQueue->waiting_jobs);		//添加任务

	pthread_cond_signal(&workQueue->jobs_cond);	//通知其他线程，有新的任务到达，可以读取执行了

	pthread_mutex_unlock(&workQueue->jobs_mtx);	//进行解锁操作	
}

//线程池关闭退出
void threadPoolShutdown(nThreadPool *workQueue){
	nWorker *worker = NULL;

	//遍历所有的线程worker，设置标识变量terminate
	for(worker = workQueue->workers;worker!=NULL;worker=worker->next){
		worker->terminate = 1;
	}

	pthread_mutex_lock(&workQueue->jobs_mtx);		//加锁，清空任务
	workQueue->workers = NULL;
	workQueue->waiting_jobs = NULL;

	pthread_cond_broadcast(&workQueue->jobs_cond);	//广播通知所有等待条件变量的线程
	pthread_mutex_unlock(&workQueue->jobs_mtx);		//解锁
}

//=========================进行测试=========================
//线程执行的任务，简单写一个，可以写多个，只要符合要求即可
void job_count(nJob *job){
	int index = *(int*)job->user_data;
	printf("thread[%lu]---data index: %d\n",pthread_self(),index);	//其中pthread_self获取线程id
	//进行资源释放
	free(job->user_data);
	free(job);
}

int main(int argc,char *argv[]){
	nThreadPool pool;
	int i = 0;

	threadPoolCreate(&pool,MAX_THREADS_COUNT);

	for(;i<MAX_JOBS_COUNT;i++){
		//初始化job任务
		nJob *job = (nJob *)malloc(sizeof(nJob));
		if(job == NULL){
			perror("job malloc error!\n");
			continue;
		}

		job->job_function = job_count;
		job->user_data = malloc(sizeof(int));
		*(int*)job->user_data = i;

		//开始加入任务到链表中去
		threadPoolQueue(&pool,job);
	}

	threadPoolShutdown(&pool);

	getchar();
	return 0;
}