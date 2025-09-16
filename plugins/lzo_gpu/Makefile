PLUGIN_NAME := lzo_gpu_plugin
PLUGIN_SOBJ := lzo_gpu_plugin.so

DEPS_LZO_GPU := $(PLUGIN_SOBJ)

PLUGIN_INCLUDE  	:= -iquote../../include
PLUGIN_INCLUDE  	+= -iquote../../criu/include
PLUGIN_INCLUDE  	+= -iquote../../criu/arch/$(ARCH)/include/
PLUGIN_INCLUDE  	+= -iquote../../

COMPEL := ../../compel/compel-host

PLUGIN_CFLAGS := -g -Wall -Werror -shared -nostartfiles -fPIC
PLUGIN_LDFLAGS := -lOpenCL

__nmk_dir ?= ../../scripts/nmk/scripts/
include $(__nmk_dir)msg.mk

all: $(DEPS_LZO_GPU)

lzo_gpu_plugin.so: lzo_gpu_plugin.c
	$(call msg-gen, $@)
	$(Q) $(CC) $(PLUGIN_CFLAGS) $(shell $(COMPEL) includes) $^ -o $@ $(PLUGIN_INCLUDE) $(PLUGIN_LDFLAGS)

clean:
	$(call msg-clean, $@)
	$(Q) $(RM) $(PLUGIN_SOBJ)
.PHONY: clean

mrproper: clean

install:
	$(Q) mkdir -p $(DESTDIR)$(PLUGINDIR)
	$(E) "  INSTALL " $(PLUGIN_NAME)
	$(Q) install -m 755 $(PLUGIN_SOBJ) $(DESTDIR)$(PLUGINDIR)
.PHONY: install

uninstall:
	$(E) " UNINSTALL" $(PLUGIN_NAME)
	$(Q) $(RM) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN_SOBJ)
.PHONY: uninstall