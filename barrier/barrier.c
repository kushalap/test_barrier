#include <linux/kernel.h>


// typedef struct barrier{
// 	unsigned int barrier_id;
// 	unsigned int count;
// 	signed int timeout;
// 	spinlock_t lock_barrier;
// 	struct semaphore sem_barrier;
// 	struct mutex mutex_barrier;
// 	struct list_head list_barrier;
// 	pid_t tgid;
// }barrier_t;

// LIST_HEAD(barrier_list_global);

// unsigned int barrier_id_global = 0;

// DEFINE_SPINLOCK(global_lock_barrier);
// DEFINE_SPINLOCK(global_lock_barrier_id);

asmlinkage long sys_barrier_init(unsigned int count, unsigned int *barrier_id, signed int timeout)
{
	printk("sys_barrier_init Entered\n");
	return 0;
}

asmlinkage long sys_barrier_wait(unsigned int barrier_id)
{
	printk("sys_barrier_wait Entered\n");
	return 0;
}

asmlinkage long sys_barrier_destroy(unsigned int barrier_id)
{
	printk("sys_barrier_destroy Entered\n");
	return 0;

}
