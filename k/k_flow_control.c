#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/ip.h>

#include <net/netfilter/nf_queue.h>
#include <linux/proc_fs.h>

#include "../scheduler.h"

/*
 * k_flow_control: Kernel flow control
 * ===================================
 */

/* module parameters */
static u32 target_ip = 0;
static char *target = "192.168.10.101";
module_param(target, charp, S_IRUGO);
MODULE_PARM_DESC(target, 
                  "The IP of Mobile station for traffic sharping");

/**
 *Record the head of the traffic data.
 */
static tfc_t headt = {
    .list = {&headt.list, &headt.list},
};

/**
 * Flow control flag.
 */
static int flow_control = 0;

static long long time_delta = 0;

/**
 * Flow control pointer, which points to current traffic data.
 */
static tfc_t *fcp = &headt;

/**
 * Queue lookup pointer 
 */
static tfc_t *qlp = &headt;

/**
 * A thread to decrease the buffered packets queue and reinject packets.
 */
struct task_struct *dequeue_thread;

/**
 *head of the list of the nf_queue 
 */
static struct nf_queue_entry head_entry = { 
    .list = LIST_HEAD_INIT(head_entry.list)
};

#ifndef CONFIG_DEBUG_SPINLOCK
#define CONFIG_DEBUG_SPINLOCK
#endif
DEFINE_SPINLOCK(q_lock);


/**
 * Utility function, convert string format of IP to 32 bit unsigned.
 */
static inline u32 k_v4pton(char *ipv4)
{
    u32 ip;
    unsigned char *p = (unsigned char *) &ip;
    if((sscanf(ipv4, "%hhu.%hhu.%hhu.%hhu", p, p+1, p+2, p+3)) != 4)
        return 0;
    return ip;
}

#define cal_rtime(otime) \
    ((long long)(otime) - time_delta)

#define tv2ms(tv) \
    ((tv)->tv_sec * 1000 + (tv)->tv_usec / 1000)

/**
 * Proc command code
 * This command tells the kernel to stop accepting traffic data and
 * start the actual packet delaying.
 */
#define CMD_FLOW_CONTROL 723


/* Proc related
   ========================================================================= */
#define PROC_DIR            "sch_80211"
#define PROC_F_PREDICTION   "prediction"
#define PROC_PERMS          0644


/**
 * Proc folder and file under this folder
 */
static struct proc_dir_entry *proc_dir, *prediction_file;

/**
 * The Proc file operation: open
 * Simply increase the reference count
 */
static int procfs_open(struct inode *inode, struct file* file)
{
    try_module_get(THIS_MODULE);
    return 0;
}

/**
 * The Proc file operation: close
 * Decrease the reference count
 */
static int procfs_close(struct inode *inode, struct file *file)
{
    module_put(THIS_MODULE);
    return 0;
}

/**
 * The Proc file operation: write
 * This write function enables user to input traffic data from the user space
 * to the kernel. The traffic data format is "%llu %u %d", which stands for 
 * packet sending time, packet size (in bytes) and packet priority respectively.
 * Multiple packets should be handled one by one.
 *
 * This function also allow user to tell the kernel that all traffic data have
 * been sent and flow control can start to go. This functionality is implemented
 * by write different command code (an integer number) to this file.
 * Refer to macros start with "CMD_".
 */
static ssize_t
write_prediction(struct file *file, 
                 const char *buffer, 
                 size_t len, 
                 loff_t *off)
{
    const int BUF_SIZE = 512;
    char procfs_buffer[BUF_SIZE];
    unsigned long procfs_buffer_size = 0;

    unsigned long id;
    unsigned long long time;
    unsigned int size;
    int priority;
    int cmd_code;
    int n, count;
    tfc_t *tp;
    struct lnode *ln;

    if (len > BUF_SIZE) {
        procfs_buffer_size = BUF_SIZE;
    }
    else {
        procfs_buffer_size = len;
    }

    if (copy_from_user(procfs_buffer, buffer, procfs_buffer_size)) {
        return -EFAULT;
    }
    n = sscanf(procfs_buffer, "%lu %llu %u %d", &id, &time, &size, &priority);
    if (n == 4){
        tp = (tfc_t *) kmalloc(sizeof(tfc_t), GFP_KERNEL);
        tp->id = id;
        tp->time = time;
        tp->size = size;
        tp->priority = priority;
        dclist_add(&tp->list, &headt.list);
    } else {
        n = sscanf(procfs_buffer, "%d", &cmd_code);
        if (n == 1) {
            switch(cmd_code) {
                case CMD_FLOW_CONTROL:
                    fcp = &headt;
                    flow_control = 1;
                    count = 0;
                    dclist_foreach(ln, &headt.list) {
                        tp = dclist_outer(ln, tfc_t, list);
#ifdef DEBUG
                        printk(KERN_INFO "|%lu  %llu  %u  %d\n", 
                                         tp->id, tp->time, 
                                         tp->size, tp->priority);
#endif
                        count++;
                    }
                    printk(KERN_INFO "%d receiverd, flow_control open\n", count);
                break;
            }
        }
    }
    
    return procfs_buffer_size;
}

/**
 * The Proc file operation: read
 * Currently this function is not needed.
 */
static ssize_t read_prediction(struct file *filp, 
                               char *buffer,
                               size_t length,     
                               loff_t * offset)
{   
    /*
    static int finished = 0;

    printk(KERN_INFO "@read::buffer_length = %lu\n", length);
    if (finished) {
            printk(KERN_INFO "procfs_read: END\n");
            finished = 0;
            return 0;
    }
    
    finished = 1;
            
    if (copy_to_user(buffer, procfs_buffer, procfs_buffer_size)) {
            return -EFAULT;
    }

    printk(KERN_INFO "procfs_read: read %lu bytes\n", procfs_buffer_size);

    return procfs_buffer_size;
    */
    return 0;
 }

/* Proc file operations */
static struct file_operations prediction_ops = {
    .read     = read_prediction,
    .write    = write_prediction,
    .open     = procfs_open,
    .release  = procfs_close,
};

/**
 * NF_HOOK
 =============================================================================*/
static unsigned int queued_counter = 0;
/**
 * NF_HOOK call back function
 * In this function, we queue all packets with destination to the target ip.
 * For other packets, we just accept them, without any changes.
 * The target ip a configable module paramter.
 */
static unsigned int
traffic_sharp(unsigned int hook,
    struct sk_buff *skb,
    const struct net_device *in,
    const struct net_device *out,
    int (*okfn)(struct sk_buff *skb))
{
    struct iphdr *iph = ip_hdr(skb);
    tfc_t *tp = NULL;
    struct lnode *ln;
    struct timeval timenow;

    //printk(KERN_INFO "::FC_D::%pI4 > %pI4\n", &iph->saddr, &iph->daddr);
    //printk(KERN_INFO "::FC_D::%08X | %08X | %d\n", 
    //                    ntohl(iph->daddr), target_ip, flow_control);

    if (iph->daddr == target_ip && flow_control) {
        printk(KERN_INFO "::FC::%pI4 > %pI4\n", &iph->saddr, &iph->daddr);

        dclist_foreach(ln, &fcp->list) {
            tp = dclist_outer(ln, tfc_t, list);
            if (tp->id == iph->check) 
                break;
        }

        if (tp == &headt) {
            //If we run to here, it should be a bug.
            //NOT queue this packet
            printk(KERN_ERR "traffic_sharp, cannot match traffic data id\n");
            return NF_DROP;
        }

        fcp = tp;

        /* stop flow control if we finish the iteration */
        tp = dclist_outer(fcp->list.next, tfc_t, list);
        if (tp == &headt) {
            flow_control = 0;
            printk(KERN_INFO "::::::flow_control closed\n"); 
        }

        /* Queue the matched packet */
        printk(KERN_INFO "::::::QUEUE:[%04X]\n", (unsigned int)fcp->id);

        if (++queued_counter == 1) {
            do_gettimeofday(&timenow);
            time_delta = fcp->time - tv2ms(&timenow);
        }
        return NF_QUEUE;
    } else {
        return NF_ACCEPT;
    }
}

/**
 * NF_HOOK registration information
 */
static struct nf_hook_ops pkt_ops = {
    .hook = traffic_sharp,
    .pf = NFPROTO_IPV4,
    .hooknum = NF_INET_POST_ROUTING,
    .priority = -1
};

/**
 * NF_QUEUE
 ============================================================================*/
#define MAX_BURST_GAP 5

static unsigned int entry_id = 1;

/* Queue call back function */
static int
queue_callback(struct nf_queue_entry *entry, 
                unsigned int queuenum) 
{
    //struct iphdr *iph = ip_hdr(entry->skb);
    //struct nf_queue_entry *q, *qnext;
    //int reinject_pkts = 0; /* flag */
    //tfc_t *tfc_curr, *tfc_next;
    //struct timeval timenow;
    
    spin_lock(&q_lock);
    entry->id = entry_id++;
    printk(KERN_INFO "::::::ID in queue: %d\n", entry->id);
    list_add_tail(&entry->list, &head_entry.list);
    spin_unlock(&q_lock);  
/*
    for (;;) {
        tfc_curr = dclist_outer(qlp->list.next, tfc_t, list);
        if (tfc_curr == &headt || tfc_curr->id == iph->check)
            break;
    }

    if (tfc_curr == &headt) {
        //TODO it will be a bug if we run to here...
        printk(KERN_ERR "queue_callback, qlp_curr reaches head!!\n");
        return 1;
    }
    qlp = tfc_curr;

    do_gettimeofday(&timenow);
    printk(KERN_INFO "real_sending_time = %llu | %llu\n", tfc_curr->time - time_delta, timenow.tv_sec*1000 + timenow.tv_usec/1000);
    
    tfc_next = dclist_outer(tfc_curr->list.next, tfc_t, list);
    if (tfc_next != &headt 
            && (tfc_next->time - tfc_curr->time) > MAX_BURST_GAP)
        reinject_pkts = 1;

    if (tfc_next == &headt)
        reinject_pkts = 1;

    if (reinject_pkts) {
        list_for_each_entry_safe(q, qnext, &head_entry.list, list) {
            printk(KERN_INFO "::::::Reinject: %d\n", q->id);
            nf_reinject(q, NF_ACCEPT);
        }
        INIT_LIST_HEAD(&head_entry.list);
        reinject_pkts = 0;
    }
*/
    return 1;
}


static struct nf_queue_handler queuehandler = {
    .name = "TrafficSchedulerQueue",
    .outfn = &queue_callback
};

/**
 * Dequeue_thread
 ==========================================================================*/
int dequeue_func(void *data) {
    struct nf_queue_entry *q, *qnext;
    tfc_t *tfc_curr;
    unsigned int pkid;
    long long time_diff;
    struct timeval tn;
    unsigned int sleep_time;

    for (;;) {
        sleep_time = 2;

        spin_lock(&q_lock);
        
        list_for_each_entry_safe(q, qnext, &head_entry.list, list) {
            pkid = ip_hdr(q->skb)->check;

            tfc_curr = qlp;
            for (;;) {
                tfc_curr = dclist_outer(tfc_curr->list.next, tfc_t, list);
                if (tfc_curr == &headt || tfc_curr->id == pkid)
                    break;
            }
            if (tfc_curr == &headt) {
                printk(KERN_ERR "cannot find a matched traffic info for: %u\n",
                        pkid);
                break;
            }
            qlp = tfc_curr;
            
            //printk(KERN_INFO "::DEQUEUE_TH::pkid %04x\n", pkid);
            
            do_gettimeofday(&tn);
            printk(KERN_INFO "::DEQUEUE_TH::[%4X]c %lu | delta %lld | tfc %llu | tfc_R %llu\n",
                        pkid,
                        tv2ms(&tn), time_delta, 
                        tfc_curr->time, cal_rtime(tfc_curr->time));
            time_diff = tv2ms(&tn) - cal_rtime(tfc_curr->time);
            printk(KERN_INFO "::DEQUEUE_TH::time_diff=%lld\n", time_diff);
            if (time_diff >= 0) {
                list_del(&q->list);
                printk(KERN_INFO "::::::Reinject: %d[%04x]\n", q->id, pkid);
                nf_reinject(q, NF_ACCEPT);
            } else {
                sleep_time = -time_diff;
                break;
            }
            
        }

        spin_unlock(&q_lock);
        
        //if (sleep_time > 1000) 
        //    sleep_time = 1000;
        //if (sleep_time != 5)
        printk(KERN_INFO "::DEQUEUE_TH::sleeptime = %u\n", sleep_time);
        msleep_interruptible(sleep_time);
        if (kthread_should_stop())
            break;
    }
    return 0;
}
 

/**
 * KERNEL MODULE related
 ============================================================================*/

/**
 * module init
 */
static int __init pkts_init(void) {
    int ret;

    if((target_ip = k_v4pton(target)) == 0)
        return -1;

    printk(KERN_INFO "%s > 0x%08X\n", target, target_ip);

    printk(KERN_INFO "pkt_scheduler starts\n");
    /* Register NF hook */
    ret = nf_register_hook(&pkt_ops);
    if (ret < 0 ) {
        printk(KERN_INFO "Fail to register NF hook: %d\n", ret);
        return -1;
    }
    /* Register NF_queue handler */
    ret = nf_register_queue_handler(PF_INET, &queuehandler);
    if (ret < 0) {
        printk(KERN_INFO "Reg queue failed with %d\n", ret);
        return -1;
    }
    /* Setup proc */
    proc_dir = proc_mkdir(PROC_DIR, NULL);
    if (proc_dir == NULL)
        return -ENOMEM;

    prediction_file = create_proc_entry(PROC_F_PREDICTION, 
                                         PROC_PERMS, proc_dir);
    if (prediction_file == NULL) {
        remove_proc_entry(PROC_DIR, NULL);
        return -ENOMEM;
    }
    prediction_file->proc_fops = &prediction_ops;
    prediction_file->mode = S_IFREG | S_IRUGO | S_IWUSR;
    prediction_file->uid = 0;
    prediction_file->gid = 0;
    prediction_file->size = 80;
    
    /* Start dequeue thread */
    dequeue_thread = kthread_run(dequeue_func, NULL, "sch_dqueue");
    return 0;
}

/**
 * module exit
 */
static void __exit pkts_exit(void) {
    tfc_t *tp;
    struct lnode *ln, *ltemp;
    /* Stop thread */
    kthread_stop(dequeue_thread);
    
    /* Remove callback */
    nf_unregister_queue_handlers(&queuehandler);
    nf_unregister_hook(&pkt_ops);
    
    /* Remove proc */
    remove_proc_entry(PROC_F_PREDICTION, proc_dir);
    remove_proc_entry(PROC_DIR, NULL);

    /* free memory */
    dclist_foreach_safe(ln, ltemp, &headt.list) {
        tp = dclist_outer(ln, tfc_t, list);
        kfree(tp);
    }
    printk(KERN_INFO "pkt_scheduler ends\n");
}

module_init(pkts_init);
module_exit(pkts_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sharp traffic for wireless station");

