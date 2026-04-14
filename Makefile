CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c11 -g
TARGET := mini_unionfs
SRC := mini_unionfs.c
DEMO_MOUNT ?= $(HOME)/mini_unionfs_mnt
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null)

.PHONY: all clean test check-fuse demo-setup demo-run demo-unmount demo-hard-clean

all: check-fuse $(TARGET)

check-fuse:
	@$(PKG_CONFIG) --exists fuse3 || (echo "libfuse3 development files are required (Ubuntu: sudo apt install libfuse3-dev)"; exit 1)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

test: all
	bash ./test_unionfs.sh

demo-setup: demo-unmount
	bash ./demo/reset_demo.sh

demo-run: all
	@mkdir -p "$(DEMO_MOUNT)"
	@echo "Mountpoint: $(DEMO_MOUNT)"
	./mini_unionfs ./demo/lower ./demo/upper "$(DEMO_MOUNT)" -f -o max_idle_threads=10 -o max_threads=16

demo-unmount:
	@fusermount3 -u "$(DEMO_MOUNT)" 2>/dev/null || \
	 fusermount -u "$(DEMO_MOUNT)" 2>/dev/null || \
	 umount "$(DEMO_MOUNT)" 2>/dev/null || true

demo-hard-clean: demo-unmount
	@rm -f "$(TARGET)"
	@rm -rf unionfs_test_env
	@rm -rf ./demo/lower ./demo/upper ./demo/mnt
	@rm -rf "$(DEMO_MOUNT)"
	@mkdir -p ./demo/lower ./demo/upper ./demo/mnt
	@touch ./demo/lower/.gitkeep ./demo/upper/.gitkeep ./demo/mnt/.gitkeep
	@echo "Hard cleanup complete. Demo layers are now empty placeholders."

clean:
	rm -f $(TARGET)
