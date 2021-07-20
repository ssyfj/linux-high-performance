/*
*先去了解nginx内存池：https://www.cnblogs.com/shuqin/p/13837898.html
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>


//https://www.cnblogs.com/shuqin/p/13837898.html
#define MP_ALIGNMENT			32
#define MP_PAGE_SIZE			4096										//正好一页内存大小
#define MP_MAX_ALLOC_FROM_POOL	(MP_PAGE_SIZE - 1)							//当小于4096时候，小块内存分配；当大于等于4096为大块内存分配
//疑惑：为啥是4095，而不是4096---因为只有分配的空间小于一页的时候才有缓存的必要（放入内存池）
//注意：4096不代表小块内存必须小于4096，而是说，当获取的内存大于等于4096时没有必要去内存池中申请空间，还不如直接利用系统接口直接向系统申请！！！！！！


//内存对齐:https://blog.csdn.net/supperwangli/article/details/5142956
//内存对齐，位取反和与操作即可
#define mp_align(n,alignment) (((n) + (alignment - 1)) & ~(alignment - 1))	//返回对齐后的空间大小
/*
也是用于对齐操作（或者说地址对齐）:寻址更快
比如一块内存大小1024字节，第一个程序占了501字节，那么第二个程序需要空间时从哪个地址开始？
先对第一块地址进行对齐操作到512，然后再从512开始分配新的地址给另外一个程序
*/
#define mp_align_ptr(p,alignment) (void *)((((size_t)p) + (alignment - 1)) & ~(alignment - 1))



//===============================开始定义内存池结构体===============================
//定义大块内存结构体，整体按照链表结构关联
struct mp_large_node {
	struct mp_large_node *next;
	void *alloc;															//后面使用posix_memalign分配大块内存
};

//定义小块内存结构体，也是按照链表结构管理所有的节点
struct mp_small_node {
	unsigned char *last;													//标识当前节点空闲内存位置,会随着空间的的分配不断变化
	unsigned char *end;														//标识当前节点内存的结束位置，不会变化。end-current可以用于标识空间的大小

	struct mp_small_node *next;												//同样使用链表管理
	size_t failed;															//用于标识这块内存分配失败的次数，如果分配失败次数过多，后面再分配大概率是不会成功的，所以可以直接跳过
};

//定义内存池
struct mp_pool_s {
	size_t max;																//用于标识界限，在小块内存和大块内存分配时使用

	struct mp_small_node *current;											//（使用尾插法）小块内存节点指针，current指向当前应该分配的节点。如果当前节点无法继续分配空间，则在生成一个新的节点去分配内存，同时移动current指针到这个节点
	struct mp_large_node *large;											//（使用头插法）大块内存指针，始终指向最新的内存块

	//https://blog.csdn.net/gatieme/article/details/64131322
	struct mp_small_node head[0];											//柔性数组：可以保证内存连续性，减少内存碎片。
};

//===============================开始声明内存池分配、释放、重置函数===============================
struct mp_pool_s *mp_create_pool(size_t size);								//内存池构建
void mp_destory_pool(struct mp_pool_s *pool);								//内存池销毁

void mp_reset_pool(struct mp_pool_s *pool);									//内存池状态重置，但是保留了分配小块内存（last指针被置为内存的起始位置）

//===============================开始声明内存分配和释放函数===============================
static void *mp_alloc_block(struct mp_pool_s *pool,size_t size);			//分配小块节点
static void *mp_alloc_large(struct mp_pool_s *pool,size_t size);			//分配大块节点
static void *mp_memalign_large(struct mp_pool_s *pool,size_t size);			//分配大块节点（包含对齐操作）

void *mp_alloc(struct mp_pool_s *pool,size_t size);							//内存分配,会判断分配大块还是小块内存,包含对齐操作
void *mp_nalloc(struct mp_pool_s *pool,size_t size);						//内存分配,会判断分配大块还是小块内存
void *mp_calloc(struct mp_pool_s *pool,size_t size);						//内部调用mp_alloc,会对内存进行置0操作
void mp_free(struct mp_pool_s *pool,void *p);								//内存释放，释放大块内存，内存地址为p则释放
//===============================开始定义内存池分配和释放函数===============================
//内存池构建
//注意：我们需要严格控制内存分配，尽量避免出现跨页现象，对于size的理解尤为重要！！！
struct mp_pool_s *mp_create_pool(size_t size){
	struct mp_pool_s *p;
	//注意：在分配内存池空间的时候，我们会一道将第一个小内存节点空间分配了，可以用柔性数组进行标识查找！！！
	//使用posix_memalign专门分配大块内存
	int ret = posix_memalign((void**)&p,MP_ALIGNMENT,size + sizeof(struct mp_pool_s) + sizeof(struct mp_small_node));					

	if(ret){														
		return NULL;
	}

	p->max = (size < MP_MAX_ALLOC_FROM_POOL) ? size : MP_MAX_ALLOC_FROM_POOL;						//获取内存块界限
	p->current = p->head;													//第一个小内存节点，和我们的内存池结构体是相连的内存
	p->large = NULL;														//大块内存还没有开始分配

	p->head->last = (unsigned char *)p + sizeof(struct mp_pool_s) + sizeof(struct mp_small_node);	//指针指向小块内存起始位置（可以正式分配的位置）
	p->head->end = p->head->last + size;

	p->head->failed = 0;

	return p;
}

//内存池销毁,回收所有的内存空间
void mp_destory_pool(struct mp_pool_s *pool){
	struct mp_small_node *h,*n;												//用于小块内存的销毁
	struct mp_large_node *l;												//用于大块内存销毁

	//由于大块内存是头插法，所以遍历方便，容易销毁
	for(l = pool->large;l;l=l->next){
		if(l->alloc){														//如果内存被分配了，则可以进行释放
			free(l->alloc);
		}
	}
	//注意：上面只是释放了内存空间，对于大块内存节点还没有释放，但是由于节点是存放在小块内存中的，所以后面释放小块内存时，会进行释放

	//开始释放小块内存
	h = pool->head->next;													//释放小块内存，从第二块开始释放，第一块与内存池结构体柔性数组相连，在释放内存池结构体时被释放！！！

	while(h){
		n = h->next;
		free(h);
		h = n;
	}

	//好了，最后释放内存池结构体
	free(pool);
}

//内存池状态重置，但是保留了分配小块内存（last指针被置为内存的起始位置）
void mp_reset_pool(struct mp_pool_s *pool){
	struct mp_small_node *h;
	struct mp_large_node *l;

	//对于大块内存全部释放
	for(l=pool->large;l;l=l->next){
		if(l->alloc){
			free(l->alloc);
		}
	}

	pool->large = NULL;

	//对于小块内存，将last指针置为初始位置---即内存空间都可以重新分配！！！
	for(h = pool->head;h;h=h->next){
		h->last = (unsigned char *)h + sizeof(struct mp_small_node);
	}
}


//===============================开始定义内存分配和释放函数===============================
//分配小块节点，并在节点中分配空间返回空间首地址
static void *mp_alloc_block(struct mp_pool_s *pool,size_t size){
	unsigned char *m;												//要分配的内存空间
	struct mp_small_node *h = pool->head;							//根据首块节点，获取后面每块节点的空间大小（与max无关）
	size_t psize = (size_t)(h->end - (unsigned char*)h);			//每块节点大小都要一致！！！

	int ret = posix_memalign((void **)&m,MP_ALIGNMENT,psize);		//成功则返回0
	if(ret){
		return NULL;
	}

	struct mp_small_node *p, *new_node, *current;				

	new_node = (struct mp_small_node*)m;
	new_node->end = m + psize;
	new_node->next = NULL;
	new_node->failed = 0;

	//下面开始改变m,分配空间
	m += sizeof(struct mp_small_node);
	m = mp_align_ptr(m, MP_ALIGNMENT);
	new_node->last = m + size;										//前面size部分被分配了

	current = pool->current;										//遍历结点，修改failed字段
	for(p = current;p->next;p=p->next){
		if(p->failed++ > 4){										//允许分配出错6次
			current = p->next;
		}
	}

	p->next = new_node;												//尾插法

	pool->current = current ? current : new_node;					//修改current指针
}

//分配大块节点,直接分配，然后返回指针即可
static void *mp_alloc_large(struct mp_pool_s *pool,size_t size){
	void *p = malloc(size);											//malloc也可以用于大块内存分配，只是少了对齐操作，但是大块内存分配不需要对齐，所以使用malloc正好
	if(p == NULL){
		return NULL;
	}

	//下面遍历所有大块节点的alloc指针，如果为NULL，则可以直接将内存挂上去
	size_t n = 0;
	struct mp_large_node *large;
	for(large = pool->large; large; large=large->next){
		if(large->alloc == NULL){
			large->alloc = p;
			return p;
		}
		if(n++ > 3){												//如果查找5次节点都没有找到空alloc指针，则直接头插法
			break;
		}
	}

	//开始头插法插入大块内存
	//1.先把结构体空间分配到小块空间中去
	large = mp_alloc(pool,sizeof(struct mp_large_node));
	if(large == NULL){
		free(p);													//结构体结点分配失败，则没有必要继续了
		return NULL;
	}

	//2.头插法处理
	large->alloc = p;
	large->next = pool->large;
	pool->large = large;

	return p;
}

//分配大块节点,对齐分配，然后返回指针即可
static void *mp_memalign_large(struct mp_pool_s *pool,size_t size){
	void *p;

	int ret = posix_memalign(&p,MP_ALIGNMENT,size);					//这里继续对齐操作
	if(ret){
		return NULL;
	}

	//下面遍历所有大块节点的alloc指针，如果为NULL，则可以直接将内存挂上去
	size_t n = 0;
	struct mp_large_node *large;
	for(large = pool->large; large; large=large->next){
		if(large->alloc == NULL){
			large->alloc = p;
			return p;
		}
		if(n++ > 3){												//如果查找5次节点都没有找到空alloc指针，则直接头插法
			break;
		}
	}

	//开始头插法插入大块内存
	//1.先把结构体空间分配到小块空间中去
	large = mp_alloc(pool,sizeof(struct mp_large_node));
	if(large == NULL){
		free(p);													//结构体结点分配失败，则没有必要继续了
		return NULL;
	}

	//2.头插法处理
	large->alloc = p;
	large->next = pool->large;
	pool->large = large;

	return p;
}

//内存分配,会判断分配大块还是小块内存,包含对齐操作
void *mp_alloc(struct mp_pool_s *pool,size_t size){
	unsigned char *m;
	struct mp_small_node *p;

	if(size <= pool->max){											//可以放入小块内存中，开始去遍历小块内存
		p = pool->current;

		do {
			m = mp_align_ptr(p->last,MP_ALIGNMENT);					//地址对齐，方便后面寻址，提高效率！！！！
			if((size_t)(p->end - m) >= size){
				p->last = m + size;									//如果current块空间足够，则直接分配空间
				return m;
			}

			p = p->next;											//如果current块空间不够分配，则去找下一块空间
		}while(p);

		return mp_alloc_block(pool,size);							//分配小块节点
	}

	return mp_memalign_large(pool,size);							//分配大块节点
}							

//内存分配,会判断分配大块还是小块内存
void *mp_nalloc(struct mp_pool_s *pool,size_t size){
	unsigned char *m;
	struct mp_small_node *p;

	if(size <= pool->max){
		p = pool->current;											//小块内存分配

		do {
			m = p->last;
			if((size_t)(p->end - m) >= size){						//空间足够
				p->last = m + size;
				return m;
			}

			p = p->next;
		}while(p);

		return mp_alloc_block(pool,size);
	}

	return mp_alloc_large(pool,size);
}
						
//内部调用mp_alloc,会对内存进行置0操作						
void *mp_calloc(struct mp_pool_s *pool,size_t size){
	void *p = mp_alloc(pool,size);									//调用上面方法，含对齐,方便寻址！！！！但是会造成部分空间未被使用
	if(p){
		memset(p,0,size);
	}

	return p;
}

//内存释放，释放大块内存，内存地址为p则释放
void mp_free(struct mp_pool_s *pool,void *p){
	struct mp_large_node *l;

	for(l = pool->large; l; l=l->next){
		if(p == l->alloc){											//找到要释放的节点，直接释放了
			free(l->alloc);
			l->alloc = NULL;
			return;
		}
	}
}

//下面main方法开始测试上面实现的函数的正确性
int main(int argc,char *argv[]){
	int size = 1 << 12;												//4096

	printf("start to test memory pool!\n");
	struct mp_pool_s *p = mp_create_pool(size);						//之所以不放在全局，是因为内存池本质是避免内存碎片出现。--->就是避免内存增加，而全局变量增加，必然占用更多的内存
	printf("mp_create_pool:%ld!\n",p->max);

	printf("mp_alloc\n");
	int i = 0;
	for(i = 0; i < 10; i++){
		void *mp = mp_alloc(p,512);									//分配小节点
	}

	mp_alloc(p,8192);												//分配大块节点

	printf("mp_reset_pool\n");
	mp_reset_pool(p);												//重置内存池

	printf("mp_nalloc and mp_free\n");
	for(i = 0; i < 5;i++){
		void *l = mp_nalloc(p,8192);
		mp_free(p,l);
	}

	printf("mp_calloc\n");
	int j = 0;
	for(i = 0;i<2;i++){
		char *pc = mp_calloc(p,16);
		for(j=0;j<16;j++){											//遍历里面每一个空间是否被置为0
			if(pc[j]){
				printf("mp_calloc wrong\n");
			}else{
				printf("mp_calloc success\n");
			}
		}
	}

	printf("mp_destory_pool\n");
	for(i = 0;i<56;i++){
		mp_alloc(p,256);
	}

	mp_destory_pool(p);

	return 0;
}