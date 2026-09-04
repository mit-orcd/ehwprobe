# ehwprobe: standalone port of Slurm's xcpuinfo topology detection.
#
#   make            build with hwloc support if available (like Slurm)
#   make nohwloc    force the /proc/cpuinfo fallback parser
#   make clean
#
# To build against a non-system hwloc (e.g. the hwloc v2 install your
# Slurm was built with):
#   make HWLOC_CFLAGS=-I/opt/hwloc/include "HWLOC_LIBS=-L/opt/hwloc/lib -lhwloc"
# or
#   make PKG_CONFIG_PATH=/opt/hwloc/lib/pkgconfig
#
# To statically embed hwloc (portable binary you can scp to nodes that
# have no hwloc installed), pointing at a static libhwloc build:
#   make static HWLOC_CFLAGS=-I/opt/hwloc/include HWLOC_STATIC=/opt/hwloc/lib/libhwloc.a

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99

# Honor PKG_CONFIG_PATH given either as environment or make variable
PKG_CONFIG := $(if $(PKG_CONFIG_PATH),PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config,pkg-config)

# Explicit HWLOC_CFLAGS/HWLOC_LIBS (command line or environment) win;
# otherwise auto-detect via pkg-config or /usr/include/hwloc.h
ifneq ($(filter command line environment,$(origin HWLOC_CFLAGS))$(filter command line environment,$(origin HWLOC_LIBS)),)
HAVE_HWLOC   := 1
HWLOC_LIBS   ?= -lhwloc
else ifeq ($(shell $(PKG_CONFIG) --exists hwloc 2>/dev/null && echo yes),yes)
HAVE_HWLOC   := 1
HWLOC_CFLAGS := $(shell $(PKG_CONFIG) --cflags hwloc)
HWLOC_LIBS   := $(shell $(PKG_CONFIG) --libs hwloc)
else ifneq ($(wildcard /usr/include/hwloc.h),)
HAVE_HWLOC   := 1
HWLOC_LIBS   := -lhwloc
endif

ehwprobe: ehwprobe.c
ifeq ($(HAVE_HWLOC),1)
	$(CC) $(CFLAGS) -DHAVE_HWLOC $(HWLOC_CFLAGS) -o $@ $< $(HWLOC_LIBS)
else
	@echo "NOTE: hwloc not found, building /proc/cpuinfo fallback version"
	$(CC) $(CFLAGS) -o $@ $<
endif

nohwloc: ehwprobe.c
	$(CC) $(CFLAGS) -o ehwprobe $<

static: ehwprobe.c
	@test -n "$(HWLOC_STATIC)" || { \
		echo "usage: make static HWLOC_CFLAGS=-I<hwloc>/include HWLOC_STATIC=<hwloc>/lib/libhwloc.a"; \
		exit 1; }
	$(CC) $(CFLAGS) -DHAVE_HWLOC $(HWLOC_CFLAGS) -o ehwprobe $< $(HWLOC_STATIC) -lm

clean:
	rm -f ehwprobe

.PHONY: nohwloc static clean
