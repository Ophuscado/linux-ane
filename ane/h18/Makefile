# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module for the H18 ANE0 of the T8152 (Mac mini M6). Not part of "make all", not autoloaded.
# Build against the M6 kernel tree on a build host (not the lab CT), by hand:
#   make KERNELDIR=<kernel tree or its out dir> ARCH=arm64 LLVM=1 modules
# The userspace sim needs only a C compiler:
#   make sim-test                    positive controls
#   make sim-negative-psd-never      sim-negative-psd-wrong   sim-negative-dt   sim-negative-args
#   make sim-mutation-noconfirm      sim-mutation-nowindow    sim-mutation-noundo   sim-mutation-noack   sim-mutation-nogate
#   make sim-all                     everything above
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h18.o
	ane_h18-objs := ane_h18_main.o ane_h18_soc.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

SIM_CC ?= cc
SIM_CFLAGS ?= -g -O0 -Wall -Wextra -Werror=return-type -std=gnu11

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

sim/sim_h18: sim/sim_harness.c sim/shim.h ane_h18_main.c ane_h18_soc.c ane_h18.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ sim/sim_harness.c

sim-test: sim/sim_h18
	./sim/sim_h18 pos

sim-negative-psd-never: sim/sim_h18
	./sim/sim_h18 neg-psd-never

sim-negative-psd-wrong: sim/sim_h18
	./sim/sim_h18 neg-psd-wrong

sim-negative-dt: sim/sim_h18
	./sim/sim_h18 neg-dt

sim-negative-args: sim/sim_h18
	./sim/sim_h18 neg-args

sim-negative-boot: sim/sim_h18
	./sim/sim_h18 neg-boot

# Mutation controls: build a copy of the real module with one sed edit and REQUIRE the named harness mode to fail it.
# $(call mutant,<name>,<sed script>,<marker>,<harness mode>)
COMMA := ,
define mutant
	rm -rf /tmp/ane_h18-$(1) && mkdir -p /tmp/ane_h18-$(1)
	cp -r sim /tmp/ane_h18-$(1)/sim
	cp ane_h18_main.c ane_h18_soc.c ane_h18.h /tmp/ane_h18-$(1)/
	sed '$(2)' ane_h18_main.c > /tmp/ane_h18-$(1)/ane_h18_main.c
	grep -q '$(3)' /tmp/ane_h18-$(1)/ane_h18_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h18-$(1)/sim -I /tmp/ane_h18-$(1)/sim/inc -o /tmp/ane_h18-$(1)/sim_h18 /tmp/ane_h18-$(1)/sim/sim_harness.c
	@/tmp/ane_h18-$(1)/sim_h18 $(4) > /tmp/ane_h18-$(1)/run.log 2>&1; \
	s=$$?; \
	echo "--- $(1) mutant harness tail ---"; tail -3 /tmp/ane_h18-$(1)/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-$(1) BAD: mutant PASSED the harness (the harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-$(1) OK: mutant FAILS the harness"; fi
endef

# stage=power runs without confirm_power=1
sim-mutation-noconfirm:
	$(call mutant,noconfirm,s/if (!confirm_power) {/if (0 \/* NEG: confirm_power ignored *\/) {/,NEG: confirm_power ignored,pos)

# the window check against the SoC row is skipped
sim-mutation-nowindow:
	$(call mutant,nowindow,s/if (res.start != pa || resource_size(\&res) != size) {/if (0 \/* NEG: window check skipped *\/) {/,NEG: window check skipped,neg-dt)

# a failed request does not put the acknowledged slots back to state 6
sim-mutation-noundo:
	$(call mutant,noundo,/^static int ane_h18_psd_up/$(COMMA)/^}/ s/ane_h18_psd_off(ane);/\/* NEG: no undo *\//,NEG: no undo,neg-psd-never)

# the acknowledged state is not compared with the requested one
sim-mutation-noack:
	$(call mutant,noack,s/if ((status \& ANE_H18_PSD_STATE_MASK) == state) {/if (1 \/* NEG: state not compared *\/) {/,NEG: state not compared,neg-psd-wrong)

# the request returns right after the write, without waiting for the acknowledge
sim-mutation-nogate:
	$(call mutant,nogate,s/writel(state$(COMMA) ane->psd\[i\] + ANE_H18_PSD_REQUEST);/writel(state$(COMMA) ane->psd[i] + ANE_H18_PSD_REQUEST); return 0; \/* NEG: no wait *\//,NEG: no wait,neg-psd-never)

# stage=boot runs without confirm_boot=1
sim-mutation-noconfirmboot:
	$(call mutant,noconfirmboot,s/if (!confirm_power || !confirm_boot) {/if (!confirm_power) {/,if (!confirm_power) {,neg-boot)

# stage=boot starts the core without comparing IORVBAR with the value iBoot leaves
NOPRE_SED := s/!= s->rvbar_expect) {/!= s->rvbar_expect \&\& 0 \/* NEG: IORVBAR not compared *\/) {/
sim-mutation-nopreflight:
	$(call mutant,nopreflight,$(NOPRE_SED),NEG: IORVBAR not compared,neg-boot)

# stage=boot never clears RUN again
sim-mutation-nohalt:
	$(call mutant,nohalt,s/writel(ctrl \& ~ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);/\/* NEG: RUN never cleared *\//,NEG: RUN never cleared,pos)

# the RTKit reply is written without looking at the inbox credits
sim-mutation-nocredit:
	$(call mutant,nocredit,s/if (!(st \& 0xf00)) {/if (0 \/* NEG: credit ignored *\/) {/,NEG: credit ignored,pos)

# the reply is written to RX, the half the firmware's message came from
sim-mutation-wronghalf:
	$(call mutant,wronghalf,s/ane_h18_po_store16(ane->mbox + chan_off + ANE_H18_PO_TX$(COMMA) r);/ane_h18_po_store16(ane->mbox + chan_off + ANE_H18_PO_RX$(COMMA) r); \/* NEG: wrong half *\//,NEG: wrong half,pos)

# a HELLO whose version range excludes 12 is answered anyway
sim-mutation-nover:
	$(call mutant,nover,s/if (minv > ANE_H18_RTK_VERSION || maxv < ANE_H18_RTK_VERSION) {/if (0 \/* NEG: version not checked *\/) {/,NEG: version not checked,pos)

# read_dart is allowed with one PSD slot
sim-mutation-dartgate:
	$(call mutant,dartgate,s/if (read_dart \&\& !ane->kpsd \&\& psd_slots != ANE_H18_NPSD) {/if (0 \/* NEG: DART gate off *\/) {/,NEG: DART gate off,neg-boot)

# rtkit_hello is allowed without alias_mbox
sim-mutation-hellogate:
	$(call mutant,hellogate,s/if (rtkit_hello \&\& !alias_mbox) {/if (0 \/* NEG: hello gate off *\/) {/,NEG: hello gate off,neg-boot)

# the firmware is mapped without the kernel-powered node (map_fw allowed with the module's own PSD requests)
sim-mutation-mapgate:
	$(call mutant,mapgate,s/if (map_fw \&\& !ane->kpsd) {/if (0 \/* NEG: map gate off *\/) {/,NEG: map gate off,neg-boot)

# the RUN write comes before the firmware map (the maps move behind the RUN comment)
sim-mutation-nofail:
	$(call mutant,nofail,s/ane_h18_result(ane$(COMMA) "boot"$(COMMA) "FAIL"$(COMMA) "fw-map-failed");/\/* NEG: map failure ignored *\/ ret = 0;/,NEG: map failure ignored,neg-boot)

# a failed second map leaves the first mapped, the domain attached
sim-mutation-noundomap:
	$(call mutant,noundomap,s/for (i = 0; i < nmapped; i++)/for (i = 0; i < 0 \&\& nmapped; i++) \/* NEG: no unmap *\//,NEG: no unmap,neg-boot)

# the runtime PM reference is never released after a clean stop
sim-mutation-norpmput:
	$(call mutant,norpmput,s/pm_runtime_put(ane->dev);/\/* NEG: no put *\//,NEG: no put,pos)

# the shutdown handshake is skipped
sim-mutation-nostop:
	$(call mutant,nostop,s/if (rtkit_stop \&\& ane->rtk_ap_on) {/if (0 \/* NEG: no shutdown *\/) {/,NEG: no shutdown,pos)

# AP power ON is sent for any IOP power acknowledgement, whatever its state
sim-mutation-noackstate:
	$(call mutant,noackstate,s/!ane->rtk_ap_sent \&\& (msg0 \& 0xff) == ANE_H18_RTK_PWR_ON/!ane->rtk_ap_sent \&\& 1 \/* NEG: ack state ignored *\//,NEG: ack state ignored,pos)

# a state 6 request that is not acknowledged does not fail the stage
sim-mutation-nostate6:
	$(call mutant,nostate6,s/if (!keep_on \&\& ane_h18_psd_off(ane)) {/if (!keep_on \&\& (ane_h18_psd_off(ane)$(COMMA) 0)) { \/* NEG: state 6 ack ignored *\//,NEG: state 6 ack ignored,neg-psd-never)

# the stage reports PASS although the shutdown was not acknowledged
sim-mutation-noshutfail:
	$(call mutant,noshutfail,s/if (rtkit_stop \&\& !(ane->rtk_ap_on \&\& ane->rtk_ap_acked \&\& ane->rtk_iop_acked)) {/if (0 \/* NEG: shutdown result ignored *\/) {/,NEG: shutdown result ignored,pos)

sim-all: sim-test sim-negative-psd-never sim-negative-psd-wrong sim-negative-dt sim-negative-args sim-negative-boot \
	sim-mutation-noconfirm sim-mutation-nowindow sim-mutation-noundo sim-mutation-noack sim-mutation-nogate \
	sim-mutation-noconfirmboot sim-mutation-nopreflight sim-mutation-nohalt \
	sim-mutation-nocredit sim-mutation-wronghalf sim-mutation-nover sim-mutation-dartgate sim-mutation-hellogate \
	sim-mutation-mapgate sim-mutation-nofail sim-mutation-noundomap sim-mutation-norpmput \
	sim-mutation-nostop sim-mutation-noackstate \
	sim-mutation-nostate6 \
	sim-mutation-noshutfail

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h18

.PHONY: sim-mutation-noshutfail sim-mutation-nostate6 sim-mutation-nostop sim-mutation-noackstate sim-mutation-mapgate sim-mutation-nofail sim-mutation-noundomap sim-mutation-norpmput sim-mutation-nocredit sim-mutation-wronghalf sim-mutation-nover sim-mutation-dartgate sim-mutation-hellogate sim-negative-boot sim-mutation-noconfirmboot sim-mutation-nopreflight sim-mutation-nohalt default modules sim-test sim-negative-psd-never sim-negative-psd-wrong sim-negative-dt sim-negative-args \
	sim-mutation-noconfirm sim-mutation-nowindow sim-mutation-noundo sim-mutation-noack sim-mutation-nogate sim-all clean
endif
