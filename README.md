# A-simple-Linux-character-device-driver-for-GPIO-control
## 知识点拓展
### 内核模块机制与模块参数
内核模块加载机制：insmod/rmmod 背后的调用链（sys_init_module() → load_module() → do_init_module()） 对应的内核文件：kernel/module.c（模块加载/卸载机制）
模块参数机制：参数是如何注册到 /sys/module/xxx/parameters 目录下的。 对应的内核文件：include/linux/moduleparam.h

##　字符设备框架ｃｄｅｖ／ｍｉｓｃ
字符设备注册流程：cdev_init() / cdev_add() 和 misc_register() 的区别。

为什么你的 demo 用的是 misc 设备？ → 因为它能自动分配 minor number，适合 demo。

内核是如何把 /dev/gpio_demo 这个节点和你的驱动 fops 挂钩的？

fs/char_dev.c（cdev 核心逻辑，字符设备注册）

drivers/char/misc.c（misc 框架逻辑）

include/linux/fs.h（file_operations 结构体定义）
