obj-m += ep800.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -Wall

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order; \
		rm -rf .tmp_versions; \
	fi

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
