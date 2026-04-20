obj-m += monitor.o
ccflags-y += -Wno-error

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
# If you want host-built workload binaries to run directly inside an Alpine
# rootfs, you can override this with WORKLOAD_LDFLAGS=-static when your
# toolchain supports it.
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

all: $(USER_TARGETS) module

ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

module: monitor.ko

engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -o engine engine.c -lpthread

memory_hog: memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

monitor.ko: monitor.c monitor_ioctl.hCC      = gcc
CFLAGS  = -Wall -Wextra -g -O0 -pthread
LDFLAGS = -pthread

obj-m   := monitor.o

KDIR    := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

BINS    = engine cpu_hog io_pulse memory_hog

.PHONY: all user kernel clean ci

all: user kernel

user: $(BINS)

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# CI target: user-space only, no kernel headers needed
ci: engine

clean:
	rm -f $(BINS) *.o *.ko *.mod.c *.mod.o .*.cmd modules.order Module.symvers
	rm -rf .tmp_versions
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS) *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all ci module clean
