CC := gcc
CFLAGS := -ffreestanding -nostdlib -mno-red-zone -mno-mmx -mno-sse \
           -mcmodel=large -Wall -Wextra -O2 -fno-stack-protector \
           -fno-omit-frame-pointer -I src/kernel -I src/kernel/arch/x86_64 \
           -I src/kernel/hal -I src/kernel/mm -I src/kernel/sched \
           -I src/kernel/user -I src/kernel/modules -D__is_kernel \
           -DNO_RUST_MODULE

ASM := gcc
ASMFLAGS := -ffreestanding -nostdlib -I src/kernel \
            -I src/kernel/arch/x86_64 -x assembler-with-cpp -c

LD := ld
LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000 -Map=build/kernel.map

RUSTC := rustc
QEMU := qemu-system-x86_64

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
	build/string.o

.PHONY: all iso run clean rust-module

all: build/kernel.elf

build/test_module.a: src/modules/test_module/lib.rs | build
	$(RUSTC) --crate-type staticlib --target x86_64-unknown-none \
		-o $@ $<

rust-module: build/test_module.a

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

build/disk.img:
	dd if=/dev/zero of=$@ bs=1M count=64

run: iso build/disk.img
	$(QEMU) -machine q35 -cpu max -cdrom build/swiftbsd.iso \
		-device ahci,id=ahci \
		-drive file=build/disk.img,format=raw,if=none,id=disk \
		-device ide-hd,drive=disk,bus=ahci.0 \
		-serial stdio -m 256M

clean:
	rm -rf build
