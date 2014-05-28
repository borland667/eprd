obj-m := eprd.o 
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC := gcc -O2
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
	$(CC) eprd_setup.c -o eprd_setup
	$(CC) tools/writetest.c -o tools/writetest
clean:
	rm -f *.c~ *.o *.ko eprd.mod.c modules.order Module.symvers .*.cmd 
	rm -rf .tmp_versions
	rm -f tools/writetest
	rm -f eprd.ko.unsigned
pretty: 
	indent -npro -kr -i8 -ts8 -sob -l80 -ss -ncs -cp1 *.c
	rm -f *.c~
