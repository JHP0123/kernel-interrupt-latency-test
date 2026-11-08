# obj-m += workqueue_button_driver.o
# obj-m += threaded_irq_button_driver_v2.o
obj-m += tasklet_button_driver.o

KDIR ?= /home/jhparkk1023/kernel_src/linux

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
