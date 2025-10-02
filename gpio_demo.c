// gpio_demo.c - 简单的 GPIO 字符设备驱动（/dev/gpio_demo）
// 特性：insmod 时通过模块参数 gpio=<num> 指定 GPIO 号
//   - 读(read)：返回当前电平("0\n"/"1\n")
//   - 写(write)：写入 '0' 或 '1' 改变输出电平
//   - ioctl：切换输入/输出方向
//
// 说明：示例使用 legacy GPIO number API，便于入门与在多数内核上编译。
//       生产环境更推荐基于设备树 + gpiod(descriptor) 写法。

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChatGPT");
MODULE_DESCRIPTION("A simple GPIO misc char driver creating /dev/gpio_demo");
MODULE_VERSION("1.0");

// --------- 模块参数 ----------
static int gpio = -1;              // 传入的 GPIO 号（必须）
//module_param是一个宏定义，是在sudo insmod gpio_demo.ko gpio=21要传入的参数
//参数：gpio-给这个变量传值，int-传入是的是一个整型，0644-对应 /sys/module/<模块名>/parameters/ 目录下的接口文件权限。
module_param(gpio, int, 0644);
//MODULE_PARM_DESC-模型的参数描述，使用效果：modinfo gpio_demo.ko
//显示：parm:           gpio:GPIO number to control (required) (int)
MODULE_PARM_DESC(gpio, "GPIO number to control (required)");

static bool initial_is_output = true; // 初始是否设为输出
module_param(initial_is_output, bool, 0644);
MODULE_PARM_DESC(initial_is_output, "Set initial direction to output (default true)");

static int initial_value = 0;      // 如果是输出，初始电平
module_param(initial_value, int, 0644);
MODULE_PARM_DESC(initial_value, "Initial output value (0 or 1)");

// --------- IOCTL 定义 ----------
#define GPIODEMO_IOC_MAGIC   'G'
#define GPIODEMO_SET_DIR_OUT _IO (GPIODEMO_IOC_MAGIC, 0)
#define GPIODEMO_SET_DIR_IN  _IO (GPIODEMO_IOC_MAGIC, 1)

// --------- 设备状态 ----------
static DEFINE_MUTEX(gpio_lock);
static bool is_output = false;

// --------- 工具函数 ----------
//把 GPIO 设置为 输出模式，并且写入初始电平值（0 或 1）。
//      val，表示希望输出的电平值（0 表示低电平，非 0 表示高电平）。
static int set_dir_output_locked(int val)
{
    int ret;
    if (!gpio_is_valid(gpio))
        return -EINVAL;

    //设置输出并同时设置高电平或低电平，成功返回0
    //gpio子系统
    ret = gpio_direction_output(gpio, val ? 1 : 0);
    if (!ret)
        is_output = true;
    return ret;
}

//把 GPIO 设置为 输入模式
static int set_dir_input_locked(void)
{
    int ret;
    //防止对无效 GPIO 设置方向。
    if (!gpio_is_valid(gpio))
        return -EINVAL;

    //成功后，把全局变量 is_output 置为 false
    ret = gpio_direction_input(gpio);
    if (!ret)
        is_output = false;
    return ret;
}

// --------- file_operations ----------
//实现从 /dev/gpio_demo 读取 GPIO 的当前电平值。
//  filp → 文件指针，表示当前打开的设备文件
//  buf → 用户空间缓冲区地址，用来接收数据。
//  count → 用户请求读取的字节数。
//  ppos → 文件偏移量，用来控制多次读取的行为。
static ssize_t gpio_demo_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    //内核缓冲区，用来存放格式化后的字符串结果（例如 "0\n" 或 "1\n"）
    char kbuf[3];
    //val-保存 gpio_get_value() 获取到的 GPIO 电平（0 或 1）。
    //len-保存 scnprintf() 生成字符串的实际长度。
    int val, len;

    //检查全局变量 gpio 是否为有效的 GPIO 号
    if (!gpio_is_valid(gpio))
        return -ENODEV;

    // 只读一次，如果大于0说明已经读到文件尾部了
    //因为cat /dev/gpio_demo会一直等EOF，也就是这个0，否则会一直读
    //那如果想在再读就再open()一次，也就自动ppos清o了
    if (*ppos > 0)
        return 0;

    mutex_lock(&gpio_lock);
    //读取当前 GPIO 的电平值。
    //GPIO子系统
    val = gpio_get_value(gpio);
    mutex_unlock(&gpio_lock);

    //把 GPIO 电平转为字符串（带换行，例如 "1\n"）
    len = scnprintf(kbuf, sizeof(kbuf), "%d\n", !!val);
    //把 kbuf 的内容拷贝到用户空间的 buf
    if (copy_to_user(buf, kbuf, len))
        return -EFAULT;

    *ppos += len;
    return len;
}
//buf第一位控制gpio
static ssize_t gpio_demo_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[8];//局部内核缓冲区，用来存放从用户空间拷贝过来的字符串
    int val;//用来保存解析出来的电平值（0 或 1）。

    if (!gpio_is_valid(gpio))
        return -ENODEV;

    if (count == 0)
        return 0;

    if (count > sizeof(kbuf) - 1)
        count = sizeof(kbuf) - 1;

    //把用户空间传进来的数据拷贝到内核缓冲区 kbuf
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    // 解析用户输入的内容，只接受 '0' 或 '1'
    if (kbuf[0] == '0')
        val = 0;
    else if (kbuf[0] == '1')
        val = 1;
    else
        return -EINVAL;

    mutex_lock(&gpio_lock);
    if (!is_output) {
        // 如果当前 GPIO 是输入模式，则自动切换到输出，并写入电平 val
        if (set_dir_output_locked(val)) {
            mutex_unlock(&gpio_lock);
            return -EIO;
        }
    } else {
        //如果当前已经是输出模式，直接设置 GPIO 的电平值。
        gpio_set_value(gpio, val);
    }
    mutex_unlock(&gpio_lock);

    //告诉用户空间，写操作成功，写入了多少字节
    return count;
}
//提供 ioctl(I/O Control) 接口，让用户空间通过 ioctl(fd, cmd, arg) 的方式控制 GPIO 的方向（输入/输出）。
static long gpio_demo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    if (!gpio_is_valid(gpio))
        return -ENODEV;

    mutex_lock(&gpio_lock);
    switch (cmd) {
    case GPIODEMO_SET_DIR_OUT:
        ret = set_dir_output_locked(gpio_get_value(gpio));
        break;
    case GPIODEMO_SET_DIR_IN:
        ret = set_dir_input_locked();
        break;
    default:
        ret = -ENOTTY;
        break;
    }
    mutex_unlock(&gpio_lock);

    return ret;
}
//inode-index node索引号“/dev/gpio_demo”
//filp:打开的文件时系统创建的struct file结构体的指针
static int gpio_demo_open(struct inode *inode, struct file *filp)
{
    if (!gpio_is_valid(gpio))
        return -ENODEV;
    return 0;
}

static const struct file_operations gpio_demo_fops = {
    .owner          = THIS_MODULE,
    .read           = gpio_demo_read,//定义的回调函数
    .write          = gpio_demo_write,
    .unlocked_ioctl = gpio_demo_ioctl,
    .open           = gpio_demo_open,
    .llseek         = no_llseek,//long long seek移动文件读写指针
};

// --------- misc 设备 ----------
static struct miscdevice gpio_demo_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,//次设备号，杂项系统_动态_次设备号
    .name  = "gpio_demo",     // => /dev/gpio_demo
    .fops  = &gpio_demo_fops,//function operation
    .mode  = 0666,            // 方便测试（生产环境请收紧权限）
};

// --------- 模块加载/卸载 ----------
static int __init gpio_demo_init(void)
{
    int ret;
//配置GPIO口
    //GPIO子系统，检测GPIO是否有效 bool gpio_is_valid(int number);
    if (!gpio_is_valid(gpio)) {
        pr_err("gpio_demo: invalid gpio=%d, please insmod with gpio=<num>\n", gpio);
        return -EINVAL;
    }
    //GPIO子系统，int gpio_request(unsigned gpio, const char *label);
    //申请一个 GPIO，防止被其他驱动占用。成功返回0
    ret = gpio_request(gpio, "gpio_demo");
    if (ret) {
        pr_err("gpio_demo: gpio_request(%d) failed: %d\n", gpio, ret);
        return ret;
    }

    mutex_lock(&gpio_lock);
    if (initial_is_output)
        //gpoi_demo.c 中的static int set_dir_output_locked(int val)
        //把 GPIO 设置为输出，并赋值电平
        ret = set_dir_output_locked(initial_value);
    else
        //static int set_dir_input_locked(void)
        //把 GPIO 设置为输入模式,成功返回0
        ret = set_dir_input_locked();
    mutex_unlock(&gpio_lock);

    if (ret) {
        pr_err("gpio_demo: set initial direction failed: %d\n", ret);
        gpio_free(gpio);
        return ret;
    }
//配置misc子系统，其实就是文件系统
    //misc 子系统,int misc_register(struct miscdevice *misc);
    //注册一个 misc 设备，自动生成 /dev/<name> 节点。输入为结构体struct miscdevice
    ret = misc_register(&gpio_demo_miscdev);
    if (ret) {
        pr_err("gpio_demo: misc_register failed: %d\n", ret);
        gpio_free(gpio);
        return ret;
    }

    pr_info("gpio_demo: loaded. gpio=%d, dir=%s, /dev/%s ready\n",
            gpio, is_output ? "out" : "in", gpio_demo_miscdev.name);
    return 0;
}

static void __exit gpio_demo_exit(void)
{
    misc_deregister(&gpio_demo_miscdev);
    if (gpio_is_valid(gpio))
        gpio_free(gpio);
    pr_info("gpio_demo: unloaded\n");
}

module_init(gpio_demo_init);
module_exit(gpio_demo_exit);

