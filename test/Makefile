ROOT_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
WORKDIR ?= /tmp/netifd_test
SOURCEDIR := $(ROOT_DIR)/sources
PREFIX := /opt/netifd

IS_CONTAINER := $(shell [ -f /etc/netifd-test-container ] && echo 1 || echo 0)

COMMON_CMAKE_FLAGS := -G Ninja \
						-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
						-DCMAKE_PREFIX_PATH=$(PREFIX) \
						-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=on

LIB_CMAKE_FLAGS := $(COMMON_CMAKE_FLAGS) \
					-DABIVERSION=1 \
					-DUNIT_TESTING=off \
					-DBUILD_LUA=off

LIBUBOX_SOURCE := https://github.com/Elektrobit/libubox.git
LIBUBOX_BRANCH := BaseOS
LIBUBOX_CMAKE_FLAGS := $(LIB_CMAKE_FLAGS) -DBUILD_EXAMPLES=off

UBUS_SOURCE := https://github.com/Elektrobit/ubus.git
UBUS_BRANCH := BaseOS
UBUS_CMAKE_FLAGS := $(LIB_CMAKE_FLAGS) -DBUILD_EXAMPLES=off
UBUS_DEPENDENCIES := libubox

UCI_SOURCE := https://github.com/Elektrobit/uci.git
UCI_BRANCH := BaseOS
UCI_CMAKE_FLAGS := $(LIB_CMAKE_FLAGS)
UCI_DEPENDENCIES := libubox

# UDEBUG_SOURCE := https://git.openwrt.org/project/udebug.git
# UDEBUG_BRANCH := master
# UDEBUG_CMAKE_FLAGS := $(LIB_CMAKE_FLAGS)
# UDEBUG_DEPENDENCIES := libubox ubus ucode

UCODE_SOURCE := https://github.com/jow-/ucode.git
UCODE_BRANCH := master
UCODE_CMAKE_FLAGS := $(LIB_CMAKE_FLAGS)

BUSYBOX_SOURCE := https://busybox.net/downloads/busybox-1.36.0.tar.bz2

GIT_SOURCES := libubox ubus uci ucode

uc = $(shell echo '$(1)' | tr '[:lower:]' '[:upper:]')
build_git_source = $(call _build_git_source,$(1),$(call uc,$(1)))

.PHONY: help
help:
	@echo "Usage:"
	@echo "  1. run make fetch on the host"
	@echo "  2. run make container-build on the host to build the container"
	@echo ""
	@echo "  3. run make container-run on the host, to start the container"
	@echo "  4. run make build in the container"
	@echo "  5. run make run-tests or python3 run_tests.py in the container"
	@echo "  OR"
	@echo "  2. run make container-run-tests on the host,"
	@echo "     to build and run tests in the container"

define _build_git_source
$(SOURCEDIR)/$1: | $(SOURCEDIR)
	@echo Checking out $(1)
	@git clone -qb $(value $(2)_BRANCH) $(value $(2)_SOURCE) $$@

fetch: $(SOURCEDIR)/$1

ifeq ($(IS_CONTAINER), 1)
.PHONY: build-$(1)
build-$(1): $(SOURCEDIR)/$(1) $(addprefix build-,$(value $(2)_DEPENDENCIES)) | $(WORKDIR)/build-$(1)
	@echo Building $(1)
	cmake $(value $(2)_CMAKE_FLAGS) \
		-B $(WORKDIR)/build-$(1) -S $(SOURCEDIR)/$(1)
	cmake --build $(WORKDIR)/build-$(1)
	cmake --install $(WORKDIR)/build-$(1)

build: build-$(1)
endif
endef

$(foreach src,$(GIT_SOURCES),$(eval $(call build_git_source,$(src))))
.PHONY: fetch

$(SOURCEDIR):
	@mkdir -p $@

$(SOURCEDIR)/busybox:
	@mkdir -p $(SOURCEDIR)/busybox
	wget "$(BUSYBOX_SOURCE)" -O - | tar -xjf - --strip-components=1 -C $(SOURCEDIR)/busybox
fetch: $(SOURCEDIR)/busybox

ifeq ($(IS_CONTAINER), 1)
$(WORKDIR)/build-%:
	@mkdir -p $@

$(WORKDIR)/build-busybox/.config: $(ROOT_DIR)/busybox.cfg | $(WORKDIR)/build-busybox
	cp $< $@
	make -C $(SOURCEDIR)/busybox O=${WORKDIR}/build-busybox oldconfig

.PHONY: build-busybox
build-busybox: $(SOURCEDIR)/busybox $(WORKDIR)/build-busybox/.config
	@echo Building busybox
	make -j5 -C $< O=${WORKDIR}/build-busybox

.PHONY: build-udhcpc6
build-udhcpc6: build-busybox
	cp ${WORKDIR}/build-busybox/busybox /usr/bin/udhcpc6
build: build-udhcpc6

.PHONY: build-zcip
build-zcip: build-busybox
	cp ${WORKDIR}/build-busybox/busybox /usr/sbin/zcip
build: build-zcip

# Build netifd
# NOTE: we have do define LIBNL_LIBS and the include path to libnl
#       due to a bug in netifd's CMakeLists, that prevents incremental builds
.PHONY: build-netifd
build-netifd: build-libubox build-ubus build-uci build-ucode  | $(WORKDIR)/build-netifd
	@echo Building netifd
	cmake $(COMMON_CMAKE_FLAGS) \
		-DENABLE_PROTO_SHELL=OFF \
		-DCMAKE_C_FLAGS=$(shell pkg-config --cflags-only-I libnl-3.0) \
		-DLIBNL_LIBS=$(shell pkg-config --libs libnl-3.0) \
		-B $(WORKDIR)/build-netifd -S ..
	cmake --build $(WORKDIR)/build-netifd
	cmake --install $(WORKDIR)/build-netifd
	ln -sf /opt/netifd/libexec/netifd /usr/libexec/netifd

.PHONY: build
build: build-netifd

.PHONY: run-tests
run-tests:
	python3 run_tests.py

.PHONY: clean
clean:
	rm -rf $(WORKDIR)
	rm -rf $(PREFIX)
	rm -f /usr/libexec/netifd

else
.PHONY:
container-build:
	podman build -t netifd/test .

.PHONY: container-run
container-run:
	podman run -it --rm --privileged --name netifd -v ..:/data netifd/test

.PHONY: container-run-tests
container-run-tests: fetch
	podman run --rm --privileged --name netifd -v ..:/data netifd/test /bin/bash -c "make -j5 build && make run-tests"

.PHONY: clean
clean:
	rm -rf $(SOURCEDIR)
endif
