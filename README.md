# Linux Kernel 2.6.0 ：实现一个鼠标驱动
### Q：如何使用该驱动?
 #### 1. 安装去掉自带HID USB驱动程序的 linux-2.6.0 内核
######      具体操作 (位于 linux-2.6.0 内核目录)：
               make clean
               make menuconfig
              -> Device Drivers
               -> HID Devices
               < > USB Human Interface Device (full HID) support     # 取消HID的USB支持
              make bzImage
               make modules
               make modules_install
               make install
#### 2. 加载该鼠标驱动模块
######       具体操作 (位于 usbmouse 内核模块目录)：
               make modules
               insmod usbmouse.ko
               lsmod | grep usbmouse     # 查看是否加载该模块
               dmesg -c     # 查看输出
### Q：如何移除该驱动？
####   A：卸载该驱动模块即可
######       具体操作：
               rmmod usbmouse.ko
              dmesg -c     # 查看输出
    
