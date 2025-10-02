//字符设备驱动框架
void cdev_init(struct cdev *cdev, const struct file_operations *fops);
//字符设备注册流程（标准方式）
static dev_t dev_num;
static struct cdev my_cdev;
//分配一个设备号->设置一个cdev结构体->将cdev结构体挂在到分配好的设备位号上
static int __init my_init(void)
{
    // 1. 申请设备号（主/次）
    //&dev_num-存放分配到的第一个设备号（包括主、次），0-起始设备号，1-需要设备数，名称用于在/proc/devices 里显示
    alloc_chrdev_region(&dev_num, 0, 1, "mychardev");

    // 2. 初始化 cdev，挂上 fops
    //my_cdev-要初始化的字符设备对象,my_fops-指向 struct file_operations
    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;

    // 3. 添加 cdev 到内核
    //my_cdev-指向 struct cdev，即你的设备对象。dev_num-起始设备号（主设备号 + 次设备号）,1-要注册的设备数量
    cdev_add(&my_cdev, dev_num, 1);

    return 0;
}
//mknod /dev/mychardev c <major>  需要手动设置/dev节点（bash中）

static void __exit my_exit(void)
{
    cdev_del(&my_cdev);                    // 删除 cdev
    unregister_chrdev_region(dev_num, 1);  // 释放设备号
}


//杂项设备注册流程
static struct miscdevice my_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,  // 动态分配次设备号
    .name  = "my_miscdev",        // /dev/my_miscdev
    .fops  = &my_fops,
};

static int __init my_init(void)
{
    return misc_register(&my_miscdev);
}

static void __exit my_exit(void)
{
    misc_deregister(&my_miscdev);
}



//位置：include/linux/fs.h
//作用：告诉内核，当用户对 /dev/xxx 做操作时，要调用你实现的函数。
struct file_operations {
    struct module *owner;  
    loff_t (*llseek) (struct file *, loff_t, int);
    ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
    long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
    int (*open) (struct inode *, struct file *);
    int (*release) (struct inode *, struct file *);
    // 还有 poll/mmap 等等
};

//位置：include/linux/cdev.h
//作用：cdev 就是内核用来管理“字符设备”的核心对象。它和 fops 绑定，然后注册到内核。
struct cdev {
    struct kobject kobj;//驱动都会挂在 sysfs 下，需要 kobject 来支撑。
    struct module *owner;//属于哪个模块，通常是 THIS_MODULE。
    const struct file_operations *ops; // 指向 fops
    struct list_head list;   // 注意：这是个双向链表结点
    dev_t dev;  // 包含主设备号和次设备号
    unsigned int count;//表示这个 cdev 管理的次设备号数量。
};
//include/linux/list.h
struct list_head {
    struct list_head *next, *prev;
};

//include/linux/miscdevice.h
struct miscdevice {
    int minor;
    const char *name;
    const struct file_operations *fops;
    struct device *parent;

    struct device *this_device;
    struct list_head list;

    struct cdev cdev;   // 注意！这里确实嵌入了一个 cdev
};

//动态申请一段字符设备号（主设备号 + count 个次设备号），并返回第一个设备号给调用者。
//dev → 输出参数，返回分配到的第一个设备号（dev_t 类型）,baseminor → 起始次设备号。通常为 0。
//count → 次设备号数量（一般为 1）。name → 设备名，显示在 /proc/devices 中。
//成功 → 0
int alloc_chrdev_region(dev_t *dev, unsigned baseminor,
                        unsigned count, const char *name)
{
    struct char_device_struct *cd;

    //在全局字符设备号表中登记一段设备号。
    //static struct char_device_struct *
    //__register_chrdev_region(unsigned int major, unsigned int baseminor,
    //                     int count, const char *name)
    //major-主设备号.如果为 0，表示让内核分配一个空闲主设备号。baseminor-起始次设备号
    //count-需要分配的次设备号数量,name-设备名称字符串
    //成功返回指向 struct char_device_struct 的指针（描述这段设备号的结构体）
    cd = __register_chrdev_region(0, baseminor, count, name);

    if (IS_ERR(cd))
        return PTR_ERR(cd);

    //把主设备号和次设备号打包成一个 dev_t 类型（内部是一个整数）
    *dev = MKDEV(cd->major, cd->baseminor);
    return 0;
}


//初始化一个 struct cdev 结构体，把它清零、设置好链表和 kobject，再将其挂上文件操作表。
//cdev → 指向一个还未初始化的 struct cdev 对象（通常是全局变量或 kmalloc 分配的内存）。
//fops → 指向 struct file_operations，包含驱动实现的 open/read/write/ioctl 等函数表。
void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
    //void *memset(void *s, int c, size_t n);
    //把整个 cdev 结构体清零，确保内部所有字段初始为 0
    //s-目标内存地址,c-要填充的字节值（这里是 0）,n-填充的大小（这里是整个 cdev 的大小）
    memset(cdev, 0, sizeof *cdev);

    //初始化 list_head，使 链表头 的 next 和 prev 都指向自己
    INIT_LIST_HEAD(&cdev->list);//&cdev->list=&(cdev->list)

    //void kobject_init(struct kobject *kobj, struct kobj_type *ktype);
    //初始化 cdev 的 kobject，使其纳入 Linux 设备模型，能被 sysfs 管理。
    //kobj → 要初始化的 kobject,ktype → kobject 类型描述符，定义了释放方法、sysfs 操作等
    kobject_init(&cdev->kobj, &ktype_cdev_default);

    //把传入的文件操作表 fops 挂到 cdev 上
    cdev->ops = fops;
}

//把一个已经初始化好的 cdev 注册到内核，并和设备号绑定
//p：指向 struct cdev 的指针（已经用 cdev_init() 初始化过，并挂了 fops）
//dev：起始设备号（由 alloc_chrdev_region() 或 MKDEV() 得到，包含主设备号和次设备号）
//count：需要注册的设备数量（一般 1，表示只注册一个次设备）。
//成功返回 0。
int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
    int error;

    //把传入的设备号保存到 cdev 里。这样 cdev 就知道自己对应哪个 (主设备号, 次设备号)。
    p->dev = dev;
    //记录这个 cdev 管理的设备数量（连续的次设备号个数）。
    p->count = count;

    //这是关键一步，把设备号范围映射到这个 cdev 对象。
    // int kobj_map(struct kobj_map *domain, dev_t dev, unsigned long range,
    //              struct module *owner, kobj_probe_t *probe,
    //              int (*lock)(dev_t, void *), void *data);
    //domain：映射表,dev：起始设备号,count：数量,data：一般是 NULL，用于特殊用途。
    //probe：回调函数，用于匹配设备号时确认是否有效,lock：回调函数，用于加锁（这里传 exact_lock）,p：传入的私有数据，这里就是 struct cdev *p
    error = kobj_map(cdev_map, dev, count, NULL,
                     exact_match, exact_lock, p);

    if (error)
        return error;
        
    //增加 cdev 所属父对象的引用计数。保证父对象在 cdev 存活期间不会被释放。
    kobject_get(p->kobj.parent);

    return 0;
}

//drivers/char/misc.c

//把一个 struct miscdevice 注册到内核，使其成为 /dev/<name> 下的字符设备。
//杂项设备的主设备号就是10,然后都是从设备
int misc_register(struct miscdevice *misc)
{
    int err;
    dev_t dev;

    if (misc->minor == MISC_DYNAMIC_MINOR) {
        //在位图中查找第一个空闲位置（用于动态分配 minor）
        //输入：位图指针，长度。
        //输出：第一个空闲 bit 的下标。
        int i = find_first_zero_bit(misc_minors, DYNAMIC_MINORS);
        if (i >= DYNAMIC_MINORS)
            return -EBUSY;
        misc->minor = DYNAMIC_MINOR_BASE + i;
        //在位图中设置某一位（表示该 minor 已被占用）
        set_bit(i, misc_minors);
    }

    //组合主设备号（MISC_MAJOR=10）和次设备号。
    dev = MKDEV(MISC_MAJOR, misc->minor);

    //初始化 cdev，绑定到 misc_fops（一个通用 fops，内部会调用 misc->fops）
    cdev_init(&misc->cdev, &misc_fops);
    misc->cdev.owner = misc->fops->owner;
    misc->cdev.ops = misc->fops;

    //把 cdev 注册到内核，使设备号和 fops 建立联系。
    err = cdev_add(&misc->cdev, dev, 1);
    if (err)
        goto out;

    //在 sysfs (/sys/class/misc/) 和 /dev/ 下创建设备节点。
    misc->this_device = device_create(misc_class, misc->parent,
                        dev, misc, "%s", misc->name);
    if (IS_ERR(misc->this_device)) {
        cdev_del(&misc->cdev);
        err = PTR_ERR(misc->this_device);
        goto out;
    }

out:
    return err;
}

int misc_deregister(struct miscdevice *misc)
{
    dev_t dev = MKDEV(MISC_MAJOR, misc->minor);

    device_destroy(misc_class, dev);
    cdev_del(&misc->cdev);

    if (misc->minor >= DYNAMIC_MINOR_BASE)
        clear_bit(misc->minor - DYNAMIC_MINOR_BASE, misc_minors);

    return 0;
}