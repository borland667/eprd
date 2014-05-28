#define BLKSIZE 4096
#define EPRD_NAME_SIZE     64  /* Max lenght of the filenames*/
#define EPRD_SET_DTAFD     0xFE00
#define EPRD_SET_DEVSZ     0xFE03
#define EPRD_REGISTER      0xFE04
#define EPRD_DEREGISTER    0xFE05
#define EPRD_SET_COMMIT_INTERVAL 0xFE06
#define EPRD_INIT          0xFE07
#define EPRD_BARRIER       0xFE08
#define EPRD_CACHESIZE     0xFE09
#define EPRD_SET_SECTORSIZE  0xFE0A

#define RANDOM 0x01
#define SEQUENTIAL 0x02
#define KERNEL_SECTORSIZE 512

#ifdef __KERNEL__
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/file.h>

#define EPRDCRIT(f, arg...) \
        printk(KERN_CRIT "EPRD : " f "\n", ## arg)
#define EPRDERR(f, arg...) \
        printk(KERN_NOTICE "EPRD : " f "\n", ## arg)
#define EPRDINFO(f, arg...) \
        printk(KERN_INFO "EPRD : " f "\n", ## arg)

struct rbtree {
        spinlock_t rbtree_lock;
        struct rb_root rbtree_root;
};

typedef struct {
    struct file *fp;
    mm_segment_t fs;
} file_info_t;

struct rbdata {
   struct rb_node node;
   unsigned long blocknr;
   char data[BLKSIZE];
   u_char valid;
   int dirty;
   unsigned int hitcount;
   unsigned int pass;
};

struct eprd_device {
        struct list_head list;
        int                     major_num;
        int                     eprd_device_number;
        int                     active;
        int                     (*ioctl)(struct eprd_device *, int cmd, 
                                     unsigned long arg); 
        unsigned long           nsectors;
        int                     logical_block_size;
        struct file *           eprd_dta_file;
        struct block_device     *eprd_device;
        struct mutex            eprd_ctl_mutex;
        unsigned long           size;
        spinlock_t              lock;
        struct gendisk          *gd;
        struct rbtree           rb_tree;
        struct workqueue_struct *wqueue;
        struct task_struct      *eprd_thread;
        struct bio_list         eprd_bio_list;
        struct request_queue    *rqueue;
        char                    *devname;
        atomic_t                flush; 
        atomic_t                wcachelock; 
        atomic_t                commit; 
        unsigned int            commit_interval;
        int                     barrier;
        int                     stop;
/*Holds the type of IO random or sequential*/
        int                     iotype;
/*Last blocknr written or read*/
        unsigned long           lastblocknr;  
/*Incremented if current blocknr == lastblocknr -1 or +1 */
        unsigned int            insequence;  
        unsigned long           cachesize;
        unsigned long           cacheentries;
        struct mutex            cachelock;
        wait_queue_head_t       eprd_event;
        wait_queue_head_t       cache_event;
        struct timer_list       flush_timer;
};

typedef struct {
        struct work_struct work;
        struct eprd_device *device;
} cache_worker_t;

static loff_t eprd_get_size(loff_t , loff_t , struct file *);
static int eprd_file_write(struct file *, u8 *, const int, loff_t);
int eprd_sync(struct eprd_device *);
int is_continues(u_char, int *, int *);

#endif
