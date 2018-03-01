obj-m       := usbmouse.o
KERN_SRC    := /root/linux-2.6.0
PWD         := $(shell pwd)

modules:
	    make -C $(KERN_SRC) M=$(PWD) modules

install:
	    make -C $(KERN_SRC) M=$(PWD) modules_install
		    depmod -a

clean:
	    make -C $(KERN_SRC) M=$(PWD) clean

