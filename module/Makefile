# SPDX-License-Identifier: MIT
#
# vDS virtual USB host controller for DualSense controllers
#

KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
DKMS_NAME := vds_hcd
VDS_VERSION ?= $(shell if [ -x "$(CURDIR)/../generate-version.sh" ]; then "$(CURDIR)/../generate-version.sh"; else echo unknown; fi)
DKMS_VERSION := $(VDS_VERSION)
DKMS_SOURCE_DIR := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
LLVM ?= $(if $(findstring clang,$(shell sed -n 's/^#define[[:space:]]\+LINUX_COMPILER[[:space:]]\+"\(.*\)"/\1/p' "$(KDIR)/include/generated/compile.h" 2>/dev/null)),1,)
KBUILD_ARGS := $(if $(LLVM),LLVM=$(LLVM),) VDS_VERSION="$(VDS_VERSION)"

.PHONY: all clean install uninstall

all:
	$(MAKE) $(KBUILD_ARGS) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) $(KBUILD_ARGS) -C $(KDIR) M=$(CURDIR) clean

install:
	@if dkms status -m $(DKMS_NAME) -v $(DKMS_VERSION) 2>/dev/null | grep -Fq '$(DKMS_NAME)/$(DKMS_VERSION)'; then \
		dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all; \
	fi
	rm -rf $(DKMS_SOURCE_DIR)
	install -d $(DKMS_SOURCE_DIR)/include
	install -m 0644 Makefile Kbuild *.c *.h $(DKMS_SOURCE_DIR)/
	sed 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="$(DKMS_VERSION)"/' dkms.conf > $(DKMS_SOURCE_DIR)/dkms.conf
	cp -a $(CURDIR)/../include/. $(DKMS_SOURCE_DIR)/include/
	dkms add $(DKMS_SOURCE_DIR)
	dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION)

uninstall:
	@if dkms status -m $(DKMS_NAME) -v $(DKMS_VERSION) 2>/dev/null | grep -Fq '$(DKMS_NAME)/$(DKMS_VERSION)'; then \
		dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all; \
	else \
		echo '$(DKMS_NAME)/$(DKMS_VERSION) is not registered with DKMS'; \
	fi
	rm -rf $(DKMS_SOURCE_DIR)
