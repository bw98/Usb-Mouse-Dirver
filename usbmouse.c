#include<linux/kernel.h>
#include<linux/slab.h>
#include<linux/input.h>
#include<linux/module.h>
#include<linux/init.h>
#include<include/linux/usb.h>
#include<linux/types.h>

//模块声明与描述
#define DRIVER_VERSION "did by bw98 on kernel v2.6.0"
#define DRIVER_AUTHOR "bw98"
#define DRIVER_DESC "usb mouse driver"
#define DRIVER_LICENSE "GPL"

MODULE_DESCRIPTION(DRIVER_DEC);
MODULE_AUTHOR(DRIVER_AUTHOR);
//许可权限申明
MODULE_LICENSE(DRIVER_LICENSE);

//usb鼠标设备
struct usb_mouse {
    char name[128]; //驱动名
    char phys[64]; //设备结点,存储usb设备路径
    struct usb_device *usbdev; //继承usb_device,描述其usb属性
    struct input_dev *dev; //继承input_dev,描述其输入设备属性
    struct urb *irq; //继承urb, 即usb请求块,用于传输数据
    signed char *data; //普通传输的缓冲区
    dma_addr_t data_dma; //用于dma传输的缓冲区
};

//构造id_table ,用于 usb core 的 match
static struct usb_device_id usb_mouse_id_table[] = {
    //mouse 是标准设备，有对应的宏来填写
    { USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID, USB_INTERFACE_SUBCLASS_BOOT,
                    USB_INTERFACE_PROTOCOL_MOUSE) },
        { } /* Terminating entry */
};

//urb结束处理回调函数
//当这个函数被调用时, USB core 就完成了这个urb, 并将它的控制权返回给设备驱动
static void completion (struct urb* urb) {
    struct usb_mouse *mouse = urb->context;
    signed char *buf = mouse->data;
    struct input_dev *dev = mouse->dev;

    int status;
    /*
     * urb->status 值为 0 表示 urb 成功返回，直接跳出循环把鼠标事件报告给输入子系统
     * ECONNRESET 出错信息表示 urb 被 usb_unlink_urb 函数给 unlink 了，ENOENT 出错信息表示 urb 被
     * usb_kill_urb 函数给 kill 了。usb_kill_urb 表示彻底结束 urb 的生命周期，而 usb_unlink_urb 则
     * 是停止 urb，这个函数不等 urb 完全终止就会返回给回调函数。这在运行中断处理程序时或者等待某自旋锁
     * 时非常有用，在这两种情况下是不能睡眠的，而等待一个 urb 完全停止很可能会出现睡眠的情况
     * ESHUTDOWN 这种错误表示 USB 主控制器驱动程序发生了严重的错误，或者提交完 urb 的一瞬间设备被拔出
     * 遇见除了以上三种错误以外的错误，将申请重传 urb
     */
    switch(urb->status) {
        case 0 :
            break;
        case ECONNRESET :
        case ENOENT :
        case ESHUTDOWN :
            return;
        default :
            goto resubmit;
    }

    //向输入子系统汇报鼠标事件
    input_report_key(dev, BTN_LEFT, buf[0] & 0x01);
    input_report_key(dev, BTN_RIGHT, buf[0] & 0x02);
    input_report_key(dev, BTN_MIDDLE, buf[0] & 0x04);
    input_report_key(dev, BTN_SIDE, buf[0] & 0x08);
    input_report_key(dev, BTN_EXTRA, buf[0] & 0x10);

    input_report_rel(dev, REL_X, buf[1]);
    input_report_rel(dev, REL_Y, buf[2]);
    input_report_rel(dev, REL_WHEEL, buf[3]);

    input_sync(dev); //事件同步

    /* 系统需要周期性不断地获取鼠标的事件信息，因此在 urb 回调函数的末尾再次提交 urb 请求块，这样又会
     * 调用新的回调函数，周而复始
     * 在回调函数中提交 urb 一定只能是 GFP_ATOMIC 优先级的，因为 urb 回调函数运行于中断上下文中，在提
     * 交 urb 过程中可能会需要申请内存、保持信号量，这些操作或许会导致 USB core 睡眠，一切导致睡眠的
     * 行为都是不允许的
     */
resubmit:
    status = usb_submit_urb (urb, GFP_ATOMIC);
        if (status)
            err ("can't resubmit intr, %s-%s/input0, status %d",
                    mouse->usbdev->bus->bus_name,
                    mouse->usbdev->devpath, status
                );
}


//当有一个接口可以由 usb_mouse_driver 处理时，调用 probe 方法
//将设备信息保存到接口，并向 USB core 注册设备，构建urb
static int usb_mouse_probe (struct usb_interface *intf, const struct usb_device_id *id ) {

    /* usb_device 继承 usb_interface ，通过父类方法 interface_to_usbdev 获得 usb_device 对象
     * usb_host_interface 继承 usb_interface , 用于描述接口设置
     * usb_endpoint_descriptor 是端点描述符类，继承 usb_host_endpoint，而 usb_host_endpoint
     * 是 usb_host_interface 的父类
     */

    /***********第一阶段:定义资源**********/
    struct usb_device *dev = interface_to_usbdev(intf); //获得该接口所在的设备对象dev
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_mouse *mouse;
    struct input_dev *input_dev;
    int pipe, maxp;

    /*********第二阶段:利用遍历各端点以及接口所在设备的信息,初始化 mouse,input_dev 中的字段*********/

    interface = intf->cur_altsetting;
    //鼠标端点只有一个,设备不满足此条件均报错
    if (interface->desc.bNumEndpoints != 1)
        return -ENODEV;
    endpoint = &interface->endpoint[0].desc; //获取端点描述符
    //鼠标的唯一端点是中断输入端点
    if (!usb_endpoint_is_int_in(endpoint))
        return -ENODEV;
    //产生中断管道，usb_mouse_driver 与端点的虚拟通道
    pipe = usb_rcvintpipe (dev, endpoint->bEndpointAddress);
    //返回该端点能够传输的最大的数据包长度，鼠标返回的最大数据包为4字节
    //数据包详细信息在 urb 结束处理回调函数中有描述
    maxp = usb_maxpacket (dev, pipe, usb_pipeout(pipe));
    //为mouse分配内存
    mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);
    //创建输入设备
    input_dev = input_allocate_device();
    if (!mouse || !input_dev)
        goto fail1;

    /* 申请内存空间用于数据传输，data 为指向该空间的地址，data_dma 则是这块内存空间的 dma 映射,
     * 即这块内存空间对应的 dma 地址。在使用 dma 传输的情况下，则使用 data_dma 指向的 dma 区域,
     * 否则使用 data 指向的普通内存区域进行传输。
     * GFP_ATOMIC 表示不等待，GFP_KERNEL 是普通的优先级，可以睡眠等待，由于鼠标使用中断传输方式,
     * 不允许睡眠状态，data 又是周期性获取鼠标事件的存储区，因此使用 GFP_ATOMIC 优先级，如果不能
     * 分配到内存则立即返回 0.
     */
    mouse->data = usb_alloc_coherent(dev, 8, GFP_ATOMIC, &mouse->data_dma);
    if (!mouse->data)
        goto fail1;

    /* 为 urb 结构体申请内存空间，第一个参数表示等时传输时需要传送包的数量，其它传输方式则为0
     * 申请的内存将通过下面即将见到的 usb_fill_int_urb 函数进行填充
     */
    mouse->irq = usb_alloc_urb (0, GFP_KERNEL); //创建urb
    if (!mouse->irq)
        goto fail2;

    //填充鼠标设备中的usb设备属性与输入设备属性
    mouse->usbdev = dev;
    mouse->dev = input_dev;

    //设置鼠标设备名称
    if (dev->manufacturer) //获取到有效的产商名
        strlcpy(mouse->name, dev->manufacturer, sizeof(mouse->name));
    if (dev->product) { //获取到有效的产品名
        if (dev->manufacturer)
            strlcat(mouse->name, " ", sizeof(mouse->name));
        strlcat(mouse->name, dev->product, sizeof(mouse->name));
    }
    if (!strlen(mouse->name)) {
        snprintf(mouse->name, sizeof(mouse->name),
                    "USB HID BP Mouse %04x:%04x",
                    le16_to_cpu(dev->descriptor.ID_VENDOR)
                    le16_to_cpu(dev->descriptor.ID_PRODUCT));
    }

    //设置鼠标设备路径
    char path[64];
    usb_make_path (dev, path, sizeof(path));
    sprintf(mouse->phys, "%s/input0", path);

    //初始化输入设备
    input_dev->name = mouse->name;
    input_dev->phys = mouse->phys;
    usb_to_input_id(dev, &input_dev->id);
    input_dev->cdev.dev = &intf->dev;
    input_dev->evbit[0] = BIT(EV_KEY) | BIT(EV_REL);
    input_dev->keybit[LONG(BTN_MOUSE)] = BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_MIDDLE);
    input_dev->relbit[0] = BIT(REL_X) | BIT(REL_Y);
    input_dev->keybit[LONG(BTN_MOUSE)] |= BIT(BTN_SIDE) | BIT(BTN_EXTRA);
    input_dev->relbit[0] |= BIT(REL_WHEEL);
    input_dev->private = mouse;   //input_dev的 private 成员相当于是设备类型，这里指 mouse
    input_dev->open = usb_mouse_open;   //填充输入设备的 open 函数指针
    input_dev->close = usb_mouse_close;   //填充输入设备的 close 函数指针

    /*********第三阶段:构建 urb 、设备信息保存到接口并向内核注册设备************/

    /* 填充构建 urb，将刚才填充好的 usb_mouse 结构体的数据填充进 urb 结构体中，在 open 方法中
     * 实现向 usb core 递交 urb
     * 当 urb 包含一个即将传输的 DMA 缓冲区时应该设置 URB_NO_TRANSFER_DMA_MAP。USB core 使用
     * transfer_dma变量所指向的缓冲区，而不是transfer_buffer变量所指向的
     * URB_NO_SETUP_DMA_MAP 用于 Setup 包，URB_NO_TRANSFER_DMA_MAP 用于所有 Data 包
     */
    usb_fill_int_urb (mouse->irq, dev, pipe, mouse->data,
                        (maxp > 8 ? 8 : maxp),
                        completion, mouse, endpoint->bInterval );
    mouse->irq->transfer_dma = mouse->data_dma;
    mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    input_register_device(mouse->dev); //向内核注册输入设备
    printk(KERN_INFO "input: %s on %s/n", mouse->name, path);

    /* 一般在 probe 函数中，都需要将设备相关信息保存在一个 usb_interface 结构体中，以便
     * 以后通过 usb_get_intfdata 获取使用 (实现多态，通过接口获取设备)
     */
    usb_set_intfdata(intf, mouse);

    return 0;

fail1:
    input_free_device(input_dev);
    kfree(mouse);
    return -ENOMEM;

fail2:
    usb_buffer_free(dev, 8, mouse->data, mouse->data_dma);
}

//当一个USB接口从系统移除时，调用 disconnect 方法
//清空接口数据，清空 urb 数据,注销设备
static void usb_mouse_disconnect (struct usb_interface *intf) {
    struct usb_mouse *mouse = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    if (mouse) {
        //结束 urb 周期
        usb_kill_urb(mouse->irq);
        //释放 urb 存储空间
        usb_free_urb(mouse->irq);
        //从输入子系统中注销设备
        input_unregister_device(mouse->dev);
        // 释放存放鼠标设备 data 存储空间
        usb_buffer_free(interface_to_usbdev(intf), 8, mouse->data, mouse->data_dma);
        //释放 usb_mouse 对象
        kfree(mouse);
    }
}

//当应用层打开鼠标设备时，调用 usb_mouse_open 方法
//向USB core 递交 probe中构建的 urb
static int usb_mouse_open (struct intput_dev *dev) {
    struct usb_mouse *mouse = dev->private;
    mouse->irq->dev = mouse->usbdev;
    if (usb_submit_urb(mouse->irq, GFP_KERNEL))
        return -EIO;

    return 0;
}

//当应用层关闭鼠标设备时，调用 usb_mouse_close 方法
//调用 usb_kill_urb 函数,终止 urb 的生命周期
static void usb_mouse_close (struct input_dev *dev) {
    struct usb_mouse *mouse = dev->private;
    usb_kill_urb(mouse->irq);
}

//构造usb鼠标驱动并注册到内核
struct usb_driver usb_mouse_driver = {
    //驱动名字
    .name = "my_usb_mouse";
    //探测方法
    .probe = usb_mouse_probe;
    //断开方法
    .disconnect = usb_mouse_disconnect;
    //描述该USB驱动所支持设备的id列表
    .id_table = use_mouse_id_table;
};

//加载驱动, 向 USB core 注册该驱动
static int __init usb_mouse_init(void) {
    int ret = usb_register(&usb_mouse_driver); //注册鼠标驱动
    if (ret == 0)
         info(DRIVER_VERSION ":" DRIVER_DESC);
    return ret;
}

//向 USB core 删除该驱动
static void __exit usb_mouse_exit(void) {
    usb_deregister(&usb_mouse_driver);
}

module_init(usb_mouse_init);
module_exit(usb_mouse_exit);

