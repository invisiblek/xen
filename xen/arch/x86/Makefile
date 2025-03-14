obj-y += acpi/
obj-y += cpu/
obj-y += genapic/
obj-$(CONFIG_GUEST) += guest/
obj-$(CONFIG_HVM) += hvm/
obj-y += mm/
obj-$(CONFIG_XENOPROF) += oprofile/
obj-$(CONFIG_PV) += pv/
obj-y += x86_64/

alternative-y := alternative.init.o
alternative-$(CONFIG_LIVEPATCH) :=
obj-bin-y += $(alternative-y)
obj-y += apic.o
obj-y += bitops.o
obj-bin-y += bzimage.init.o
obj-bin-y += clear_page.o
obj-bin-y += copy_page.o
obj-y += cpuid.o
obj-$(CONFIG_PV) += compat.o
obj-$(CONFIG_PV32) += x86_64/compat.o
obj-$(CONFIG_KEXEC) += crash.o
obj-$(CONFIG_GDBSX) += debug.o
obj-y += delay.o
obj-y += desc.o
obj-bin-y += dmi_scan.init.o
obj-y += domain.o
obj-bin-y += dom0_build.init.o
obj-y += domain_page.o
obj-y += e820.o
obj-y += emul-i8254.o
obj-y += extable.o
obj-y += flushtlb.o
obj-$(CONFIG_CRASH_DEBUG) += gdbstub.o
obj-y += hypercall.o
obj-y += i387.o
obj-y += i8259.o
obj-y += io_apic.o
obj-$(CONFIG_LIVEPATCH) += alternative.o livepatch.o
obj-y += msi.o
obj-y += msr.o
obj-$(CONFIG_INDIRECT_THUNK) += indirect-thunk.o
obj-y += ioport_emulate.o
obj-y += irq.o
obj-$(CONFIG_KEXEC) += machine_kexec.o
obj-y += mm.o x86_64/mm.o
obj-$(CONFIG_HVM) += monitor.o
obj-y += mpparse.o
obj-y += nmi.o
obj-y += numa.o
obj-y += pci.o
obj-y += percpu.o
obj-y += physdev.o
obj-$(CONFIG_COMPAT) += x86_64/physdev.o
obj-y += psr.o
obj-y += setup.o
obj-y += shutdown.o
obj-y += smp.o
obj-y += smpboot.o
obj-y += spec_ctrl.o
obj-y += srat.o
obj-y += string.o
obj-y += time.o
obj-y += trace.o
obj-y += traps.o
obj-y += tsx.o
obj-y += usercopy.o
obj-y += x86_emulate.o
obj-$(CONFIG_TBOOT) += tboot.o
obj-y += hpet.o
obj-y += vm_event.o
obj-y += xstate.o

ifneq ($(CONFIG_PV_SHIM_EXCLUSIVE),y)
obj-y += domctl.o
obj-y += platform_hypercall.o
obj-$(CONFIG_COMPAT) += x86_64/platform_hypercall.o
obj-y += sysctl.o
endif

extra-y += asm-macros.i

ifneq ($(CONFIG_HVM),y)
x86_emulate.o: CFLAGS-y += -Wno-unused-label
endif

efi-y := $(shell if [ ! -r $(BASEDIR)/include/xen/compile.h -o \
                      -O $(BASEDIR)/include/xen/compile.h ]; then \
                         echo '$(TARGET).efi'; fi) \
         $(space)
efi-$(CONFIG_PV_SHIM_EXCLUSIVE) :=

ifneq ($(build_id_linker),)
notes_phdrs = --notes
else
ifeq ($(CONFIG_PVH_GUEST),y)
notes_phdrs = --notes
endif
endif

ifdef CONFIG_LIVEPATCH
all_symbols = --all-symbols
ifdef CONFIG_FAST_SYMBOL_LOOKUP
all_symbols = --all-symbols --sort-by-name
endif
else
all_symbols =
endif

syms-warn-dup-y := --warn-dup
syms-warn-dup-$(CONFIG_SUPPRESS_DUPLICATE_SYMBOL_WARNINGS) :=
syms-warn-dup-$(CONFIG_ENFORCE_UNIQUE_SYMBOLS) := --error-dup

$(TARGET): TMP = $(@D)/.$(@F).elf32
$(TARGET): $(TARGET)-syms $(efi-y) boot/mkelf32
	./boot/mkelf32 $(notes_phdrs) $(TARGET)-syms $(TMP) $(XEN_IMG_OFFSET) \
	               `$(NM) $(TARGET)-syms | sed -ne 's/^\([^ ]*\) . __2M_rwdata_end$$/0x\1/p'`
	od -t x4 -N 8192 $(TMP)  | grep 1badb002 > /dev/null || \
		{ echo "No Multiboot1 header found" >&2; false; }
	od -t x4 -N 32768 $(TMP) | grep e85250d6 > /dev/null || \
		{ echo "No Multiboot2 header found" >&2; false; }
	mv $(TMP) $(TARGET)

ifneq ($(efi-y),)

# Check if the compiler supports the MS ABI.
export XEN_BUILD_EFI := $(shell $(CC) $(XEN_CFLAGS) -c efi/check.c -o efi/check.o 2>/dev/null && echo y)
CFLAGS-$(XEN_BUILD_EFI) += -DXEN_BUILD_EFI

# Check if the linker supports PE.
EFI_LDFLAGS = $(patsubst -m%,-mi386pep,$(XEN_LDFLAGS)) --subsystem=10
XEN_BUILD_PE := $(if $(XEN_BUILD_EFI),$(call ld-option,$(EFI_LDFLAGS) --image-base=0x100000000 -o efi/check.efi efi/check.o))
# If the above failed, it may be merely because of the linker not dealing well
# with debug info. Try again with stripping it.
ifeq ($(CONFIG_DEBUG_INFO)-$(XEN_BUILD_PE),y-n)
EFI_LDFLAGS += --strip-debug
XEN_BUILD_PE := $(call ld-option,$(EFI_LDFLAGS) --image-base=0x100000000 -o efi/check.efi efi/check.o)
endif

ifeq ($(XEN_BUILD_PE),y)

# Check if the linker produces fixups in PE by default
nr-fixups := $(shell $(OBJDUMP) -p efi/check.efi | grep '^[[:blank:]]*reloc[[:blank:]]*[0-9][[:blank:]].*DIR64$$' | wc -l)
ifeq ($(nr-fixups),2)
MKRELOC := :
relocs-dummy :=
else
MKRELOC := efi/mkreloc
relocs-dummy := efi/relocs-dummy.o
# If the linker produced fixups but not precisely two of them, we need to
# disable it doing so.  But if it didn't produce any fixups, it also wouldn't
# recognize the option.
ifneq ($(nr-fixups),0)
EFI_LDFLAGS += --disable-reloc-section
endif
endif

endif # $(XEN_BUILD_PE)

endif # $(efi-y)

ALL_OBJS := $(BASEDIR)/arch/x86/boot/built_in.o $(BASEDIR)/arch/x86/efi/built_in.o $(ALL_OBJS)

ifeq ($(CONFIG_LTO),y)
# Gather all LTO objects together
prelink_lto.o: $(ALL_OBJS) $(ALL_LIBS)
	$(LD_LTO) -r -o $@ $(filter-out %.a,$^) --start-group $(filter %.a,$^) --end-group

# Link it with all the binary objects
prelink.o: $(patsubst %/built_in.o,%/built_in_bin.o,$(ALL_OBJS)) prelink_lto.o FORCE
	$(call if_changed,ld)
else
prelink.o: $(ALL_OBJS) $(ALL_LIBS) FORCE
	$(call if_changed,ld)
endif

targets += prelink.o

$(TARGET)-syms: prelink.o xen.lds
	$(LD) $(XEN_LDFLAGS) -T xen.lds -N prelink.o $(build_id_linker) \
	    $(BASEDIR)/common/symbols-dummy.o -o $(@D)/.$(@F).0
	$(NM) -pa --format=sysv $(@D)/.$(@F).0 \
		| $(BASEDIR)/tools/symbols $(all_symbols) --sysv --sort \
		>$(@D)/.$(@F).0.S
	$(MAKE) -f $(BASEDIR)/Rules.mk $(@D)/.$(@F).0.o
	$(LD) $(XEN_LDFLAGS) -T xen.lds -N prelink.o $(build_id_linker) \
	    $(@D)/.$(@F).0.o -o $(@D)/.$(@F).1
	$(NM) -pa --format=sysv $(@D)/.$(@F).1 \
		| $(BASEDIR)/tools/symbols $(all_symbols) --sysv --sort $(syms-warn-dup-y) \
		>$(@D)/.$(@F).1.S
	$(MAKE) -f $(BASEDIR)/Rules.mk $(@D)/.$(@F).1.o
	$(LD) $(XEN_LDFLAGS) -T xen.lds -N prelink.o $(build_id_linker) \
	    $(@D)/.$(@F).1.o -o $@
	$(NM) -pa --format=sysv $(@D)/$(@F) \
		| $(BASEDIR)/tools/symbols --all-symbols --xensyms --sysv --sort \
		>$(@D)/$(@F).map
	rm -f $(@D)/.$(@F).[0-9]* $(@D)/..$(@F).[0-9]*

note.o: $(TARGET)-syms
	$(OBJCOPY) -O binary --only-section=.note.gnu.build-id $< $@.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section=.data=.note.gnu.build-id -S $@.bin $@
	rm -f $@.bin

EFI_LDFLAGS += --image-base=$(1) --stack=0,0 --heap=0,0
EFI_LDFLAGS += --section-alignment=0x200000 --file-alignment=0x20
EFI_LDFLAGS += --major-image-version=$(XEN_VERSION)
EFI_LDFLAGS += --minor-image-version=$(XEN_SUBVERSION)
EFI_LDFLAGS += --major-os-version=2 --minor-os-version=0
EFI_LDFLAGS += --major-subsystem-version=2 --minor-subsystem-version=0
# It seems ld unfortunately can't set a custom timestamp, so add a zero value
# for the timestamp (option --no-insert-timestamp) if SOURCE_DATE_EPOCH is
# defined to make reproducible builds possible.
ifdef SOURCE_DATE_EPOCH
EFI_LDFLAGS += --no-insert-timestamp
endif

$(TARGET).efi: VIRT_BASE = 0x$(shell $(NM) efi/relocs-dummy.o | sed -n 's, A VIRT_START$$,,p')
ifeq ($(MKRELOC),:)
$(TARGET).efi: ALT_BASE :=
else
$(TARGET).efi: ALT_BASE = 0x$(shell $(NM) efi/relocs-dummy.o | sed -n 's, A ALT_START$$,,p')
endif

ifneq ($(build_id_linker),)
ifeq ($(call ld-ver-build-id,$(LD) $(filter -m%,$(EFI_LDFLAGS))),y)
CFLAGS-y += -DBUILD_ID_EFI
EFI_LDFLAGS += $(build_id_linker)
note_file := efi/buildid.o
# NB: this must be the last input in the linker call, because inputs following
# the -b option will all be treated as being in the specified format.
note_file_option := -b pe-x86-64 $(note_file)
else
note_file := note.o
endif
else
note_file :=
endif
note_file_option ?= $(note_file)

ifeq ($(XEN_BUILD_PE),y)
$(TARGET).efi: prelink.o $(note_file) efi.lds efi/relocs-dummy.o efi/mkreloc
ifeq ($(CONFIG_DEBUG_INFO),y)
	$(if $(filter --strip-debug,$(EFI_LDFLAGS)),echo,:) "Will strip debug info from $(@F)"
endif
	$(foreach base, $(VIRT_BASE) $(ALT_BASE), \
	          $(LD) $(call EFI_LDFLAGS,$(base)) -T efi.lds -N $< $(relocs-dummy) \
	                $(BASEDIR)/common/symbols-dummy.o $(note_file_option) -o $(@D)/.$(@F).$(base).0 &&) :
	$(MKRELOC) $(foreach base,$(VIRT_BASE) $(ALT_BASE),$(@D)/.$(@F).$(base).0) >$(@D)/.$(@F).0r.S
	$(NM) -pa --format=sysv $(@D)/.$(@F).$(VIRT_BASE).0 \
		| $(BASEDIR)/tools/symbols $(all_symbols) --sysv --sort >$(@D)/.$(@F).0s.S
	$(MAKE) -f $(BASEDIR)/Rules.mk $(@D)/.$(@F).0r.o $(@D)/.$(@F).0s.o
	$(foreach base, $(VIRT_BASE) $(ALT_BASE), \
	          $(LD) $(call EFI_LDFLAGS,$(base)) -T efi.lds -N $< \
	                $(@D)/.$(@F).0r.o $(@D)/.$(@F).0s.o $(note_file_option) -o $(@D)/.$(@F).$(base).1 &&) :
	$(MKRELOC) $(foreach base,$(VIRT_BASE) $(ALT_BASE),$(@D)/.$(@F).$(base).1) >$(@D)/.$(@F).1r.S
	$(NM) -pa --format=sysv $(@D)/.$(@F).$(VIRT_BASE).1 \
		| $(BASEDIR)/tools/symbols $(all_symbols) --sysv --sort >$(@D)/.$(@F).1s.S
	$(MAKE) -f $(BASEDIR)/Rules.mk $(@D)/.$(@F).1r.o $(@D)/.$(@F).1s.o
	$(LD) $(call EFI_LDFLAGS,$(VIRT_BASE)) -T efi.lds -N $< \
	                $(@D)/.$(@F).1r.o $(@D)/.$(@F).1s.o $(note_file_option) -o $@
	$(NM) -pa --format=sysv $(@D)/$(@F) \
		| $(BASEDIR)/tools/symbols --all-symbols --xensyms --sysv --sort >$(@D)/$(@F).map
	rm -f $(@D)/.$(@F).[0-9]* $(@D)/..$(@F).[0-9]*
else
$(TARGET).efi: FORCE
	rm -f $@
	echo '$(if $(filter y,$(XEN_BUILD_EFI)),xen.efi generation,EFI support) disabled'
endif

efi/buildid.o efi/relocs-dummy.o: $(BASEDIR)/arch/x86/efi/built_in.o
efi/buildid.o efi/relocs-dummy.o: ;

.PHONY: include
include: $(BASEDIR)/include/asm-x86/asm-macros.h

asm-macros.i: CFLAGS-y += -D__ASSEMBLY__ -P

$(BASEDIR)/include/asm-x86/asm-macros.h: asm-macros.i Makefile
	echo '#if 0' >$@.new
	echo '.if 0' >>$@.new
	echo '#endif' >>$@.new
	echo '#ifndef __ASM_MACROS_H__' >>$@.new
	echo '#define __ASM_MACROS_H__' >>$@.new
	echo 'asm ( ".include \"$@\"" );' >>$@.new
	echo '#endif /* __ASM_MACROS_H__ */' >>$@.new
	echo '#if 0' >>$@.new
	echo '.endif' >>$@.new
	cat $< >>$@.new
	echo '#endif' >>$@.new
	$(call move-if-changed,$@.new,$@)

efi.lds: AFLAGS-y += -DEFI
xen.lds efi.lds: xen.lds.S
	$(CPP) -P $(call cpp_flags,$(a_flags)) -MQ $@ -o $@ $<

boot/mkelf32: boot/mkelf32.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

efi/mkreloc: efi/mkreloc.c
	$(HOSTCC) $(HOSTCFLAGS) -g -o $@ $<

.PHONY: clean
clean::
	rm -f *.lds *.new boot/*.o boot/*~ boot/core boot/mkelf32
	rm -f asm-macros.i $(BASEDIR)/include/asm-x86/asm-macros.*
	rm -f $(BASEDIR)/.xen-syms.[0-9]* boot/.*.d $(BASEDIR)/.xen.elf32
	rm -f $(BASEDIR)/.xen.efi.[0-9]* efi/*.efi efi/mkreloc
	rm -f boot/cmdline.S boot/reloc.S boot/*.lnk boot/*.bin
	rm -f note.o

# Suppress loading of DEPS files for internal, temporary target files.  This
# then also suppresses re-generation of the respective .*.d2 files.
ifeq ($(filter-out .xen%.o,$(notdir $(MAKECMDGOALS))),)
DEPS_INCLUDE:=
endif
