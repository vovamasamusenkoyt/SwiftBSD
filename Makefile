CC := gcc

RUSTC := rustc
RUSTFLAGS := -C panic=abort -C opt-level=s -C link-args=-nostartfiles
QEMU := qemu-system-x86_64

# Rust module, built unconditionally (rustc works outside sandbox)
RUST_MODULE := build-rust/test_module.a

CFLAGS := -ffreestanding -nostdlib -mno-red-zone -mno-mmx -mno-sse \
           -mcmodel=large -Wall -Wextra -O2 -fno-stack-protector \
           -fno-omit-frame-pointer -I src/kernel -I src/kernel/arch/x86_64 \
           -I src/kernel/hal -I src/kernel/mm -I src/kernel/sched \
            -I src/kernel/user -I src/kernel/modules -I src/kernel/fs \
           -D__is_kernel

ASM := gcc
ASMFLAGS := -ffreestanding -nostdlib -I src/kernel \
            -I src/kernel/arch/x86_64 -x assembler-with-cpp -c

LD := ld
LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000 -Map=build/kernel.map

USERLAND_CC := gcc
USERLAND_CFLAGS := -ffreestanding -nostdlib -static -no-pie -mno-red-zone \
                    -mno-mmx -mno-sse -O2 -I src/userland \
                    -fno-stack-protector -fno-builtin
USERLAND_LDFLAGS := -T src/userland/user.ld -nostdlib -static -no-pie

USERLAND_PROGS := shell ls cat echo
USERLAND_ELFS := $(addprefix build/,$(addsuffix .elf,$(USERLAND_PROGS)))
USERLAND_OBJS := $(addprefix build/,$(addsuffix _elf.o,$(USERLAND_PROGS)))

OBJS := \
	build/bootstrap.o \
	build/isr.o \
	build/idt.o \
	build/switch.o \
	build/syscall.o \
	build/syscall_handler.o \
	build/pit.o \
	build/keyboard.o \
	build/timer.o \
	build/sched.o \
	build/serial.o \
	build/pmm.o \
	build/pmem.o \
	build/kheap.o \
	build/kmain.o \
	build/vmm.o \
	build/tss.o \
	build/user_entry.o \
	build/user.o \
	build/user_prog_bin.o \
	build/pci.o \
	build/ahci.o \
	build/module_loader.o \
	build/string.o \
	build/swiftfs2.o \
	build/elf.o \
	$(RUST_MODULE) \
	$(USERLAND_OBJS)

.PHONY: all iso run clean rust-module

all: build/kernel.elf

build-rust/test_module.a: src/modules/test_module/lib.rs
	mkdir -p build-rust
	$(RUSTC) $(RUSTFLAGS) --crate-type staticlib --target x86_64-unknown-none \
		-o $@ $<

rust-module: build-rust/test_module.a

build/kernel.elf: $(OBJS) linker.ld | build
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

build:
	mkdir -p build

build/%.o: src/kernel/arch/x86_64/%.asm | build
	$(ASM) $(ASMFLAGS) $< -o $@

build/%.o: src/kernel/hal/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/mm/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/sched/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/user/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/modules/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/arch/x86_64/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/kernel/fs/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# Userland ELF programs
build/%.elf: src/userland/%.c src/userland/user.ld $(USERLAND_DEPS) | build
	$(USERLAND_CC) $(USERLAND_CFLAGS) -T src/userland/user.ld -o $@ $<

build/%_elf.o: build/%.elf | build
	ld -r -b binary -o $@ $<

USER_PROG_BIN := build/user_prog.bin

build/user_prog.o: src/kernel/arch/x86_64/user_prog.S | build
	$(ASM) $(ASMFLAGS) $< -o $@

$(USER_PROG_BIN): build/user_prog.o | build
	objcopy --only-section=.user_prog -O binary $< $@

build/user_prog_bin.o: $(USER_PROG_BIN) | build
	ld -r -b binary -o $@ $<

build/iso: build/kernel.elf boot/grub.cfg | build
	mkdir -p build/iso/boot/grub
	cp build/kernel.elf build/iso/boot/
	cp boot/grub.cfg build/iso/boot/grub/

iso: build/iso
	grub-mkrescue -o build/swiftbsd.iso build/iso

tools/mkfs.swiftfs2: tools/mkfs.swiftfs2.c
	gcc -Wall -O2 -o $@ $<

build/disk.img: tools/mkfs.swiftfs2
	./tools/mkfs.swiftfs2 $@ 128

run: iso build/disk.img
	$(QEMU) -machine q35 -cpu max -cdrom build/swiftbsd.iso \
		-drive file=build/disk.img,format=raw,if=none,id=disk \
		-device ahci,id=ahci \
		-device ide-hd,drive=disk,bus=ahci.0 \
		-nographic -m 256M

clean:
	rm -rf build build-rust
