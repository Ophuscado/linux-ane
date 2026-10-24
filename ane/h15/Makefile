# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module: not part of "make all", not in dkms.conf,
# not autoloaded (no MODULE_DEVICE_TABLE). Build by hand for bring-up:
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 modules
#   make check                 (host ADT walker self test)
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h15.o
	ane_h15-objs := ane_h15_main.o ane_h15_soc.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

check: ane_h15_adt_test
	./ane_h15_adt_test

ane_h15_adt_test: ane_h15_adt_test.c ane_h15_adt.h
	$(CC) $(CFLAGS) -I. ane_h15_adt_test.c -o $@

# ---- userspace sim (PMP vehicle pattern; NOT part of make all) ----
# Compiles the REAL module sources against sim/shim.h and drives every
# stage. sim-test: positive controls, exit 0 required. The named
# sim-negative-* targets are the discriminating controls: each exits 0
# iff the module fails the induced fault correctly, and 1 (BAD) if the
# module could pass a broken environment. sim-negative-nopsguard builds
# a copy of the source with the PS guard call deleted and REQUIRES the
# harness to fail it (PMP sim-negative semantics).
SIM_CC ?= cc
SIM_CFLAGS ?= -g -O0 -Wall -Wextra -std=gnu11
SIM_HDRS := ane_h15.h ane_h15_adt.h sim/shim.h \
	$(wildcard sim/inc/linux/*.h) sim/inc/crypto/sha2.h

sim/sim_h15: sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS)
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ sim/sim_harness.c

# P7 build: the fact table is substituted (synthetic, fully filled) so
# the real probe + stage_boot run the every-fact-filled fall-through.
sim/sim_h15_filled: sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS)
	$(SIM_CC) $(SIM_CFLAGS) -DANE15_SIM_FILL_FACTS -I sim -I sim/inc -o $@ sim/sim_harness.c

sim-probe-filled: sim/sim_h15_filled
	./sim/sim_h15_filled pos-filled

sim-test: sim/sim_h15
	./sim/sim_h15 pos

sim-negative-ps-stuck: sim/sim_h15
	./sim/sim_h15 neg-ps-stuck

sim-negative-wrong-stage: sim/sim_h15
	./sim/sim_h15 neg-wrong-stage

sim-negative-truncated-dt: sim/sim_h15
	./sim/sim_h15 neg-truncated-dt

sim-negative-rpm-fail: sim/sim_h15
	./sim/sim_h15 neg-rpm-fail

# Multiple power-domains (attach by id + runtime-PM device links).
sim-negative-pd-attach-fail: sim/sim_h15
	./sim/sim_h15 neg-pd-attach-fail

sim-negative-pd-link-fail: sim/sim_h15
	./sim/sim_h15 neg-pd-link-fail

sim-negative-pd-rpm-err: sim/sim_h15
	./sim/sim_h15 neg-pd-rpm-err

sim-negative-pd-eacces-alone: sim/sim_h15
	./sim/sim_h15 neg-pd-eacces-alone

# Stage 2 (confirm_read=1): the read-only preflight.
sim-negative-read-ps-stuck: sim/sim_h15
	./sim/sim_h15 neg-read-ps-stuck

sim-negative-read-window: sim/sim_h15
	./sim/sim_h15 neg-read-window

sim-negative-read-forbidden: sim/sim_h15
	./sim/sim_h15 neg-read-forbidden

# Mutation controls for the multi-domain code: build a copy of the real
# module with one sed edit and REQUIRE the named harness mode to fail it.
# $(call pd_mutant,<name>,<sed script>,<marker>,<harness mode>)
COMMA := ,
define pd_mutant
	rm -rf /tmp/ane_h15-$(1) && mkdir -p /tmp/ane_h15-$(1)
	cp -r sim /tmp/ane_h15-$(1)/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15.h ane_h15_adt.h /tmp/ane_h15-$(1)/
	sed '$(2)' ane_h15_main.c > /tmp/ane_h15-$(1)/ane_h15_main.c
	grep -q '$(3)' /tmp/ane_h15-$(1)/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-$(1)/sim -I /tmp/ane_h15-$(1)/sim/inc -o /tmp/ane_h15-$(1)/sim_h15 /tmp/ane_h15-$(1)/sim/sim_harness.c
	@/tmp/ane_h15-$(1)/sim_h15 $(4) > /tmp/ane_h15-$(1)/run.log 2>&1; \
	s=$$?; \
	echo "--- $(1) mutant harness tail ---"; tail -4 /tmp/ane_h15-$(1)/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-$(1) BAD: mutant PASSED the harness (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-$(1) OK: mutant FAILS the harness"; fi
endef

# pd_count set only after device_link_add: a failed link leaks its domain
sim-mutation-pd-nolinkdetach:
	$(call pd_mutant,pd-nolinkdetach,s/ane->pd_count = i + 1;/\/* NEG: pd_count set after link *\//,NEG: pd_count set after link,neg-pd-link-fail)

# any get_sync error ignored once domains are attached, not only -EACCES
sim-mutation-pd-anyerrignored:
	$(call pd_mutant,pd-anyerrignored,s/ret == -EACCES/ret < 0 \/* NEG: any error ignored *\//,NEG: any error ignored,neg-pd-rpm-err)

# a failed get_sync takes a usage count that nothing drops
sim-mutation-pd-noidleleak:
	$(call pd_mutant,pd-noidleleak,s/pm_runtime_put_noidle(&pdev->dev);/\/* NEG: usage count leaked *\//,NEG: usage count leaked,neg-pd-rpm-err)

# remove() no longer detaches the domains and deletes the links
sim-mutation-pd-noremovedetach:
	$(call pd_mutant,pd-noremovedetach,/^static void ane_h15_remove/$(COMMA)/^}/ s/ane_h15_detach_pds(ane);/\/* NEG: remove does not detach *\//,NEG: remove does not detach,pos)

# stage 2 reads without the pmgr ACTUAL=0xf gate
sim-mutation-read-noguard:
	$(call pd_mutant,read-noguard,/^static int ane_h15_stage_wrapper/$(COMMA)/^}/ s/ret = ane_h15_ps_wait(ane);/ret = 0; \/* NEG: stage 2 guard skipped *\//,NEG: stage 2 guard skipped,neg-read-ps-stuck)

# stage 2 read allowlist (two ADT windows, no CPU_CONTROL, no fatal range) skipped
sim-mutation-read-nocheck:
	$(call pd_mutant,read-nocheck,s/if (!ane_h15_read_ok(ane->soc$(COMMA) off$(COMMA) 4)) {/if (0 \/* NEG: read allowlist skipped *\/) {/,NEG: read allowlist skipped,neg-read-forbidden)

# stage 2 trusts the SoC row without comparing the live reg[0] and reg[1] resources
sim-mutation-read-nowindow:
	$(call pd_mutant,read-nowindow,s/res.start == pa \&\& resource_size(\&res) == size/1 \/* NEG: window check skipped *\//,NEG: window check skipped,neg-read-window)

# stage 2 runs without confirm_read=1
sim-mutation-read-noconfirm:
	$(call pd_mutant,read-noconfirm,s/if (!confirm_read) {/if (0 \/* NEG: confirm_read ignored *\/) {/,NEG: confirm_read ignored,pos)

# Stage 4 (confirm_run=1): the only stage with writes. neg-run = refusals + the stuck core.
sim-negative-run: sim/sim_h15
	./sim/sim_h15 neg-run

# stage 4 runs without confirm_run=1
sim-mutation-run-noconfirm:
	$(call pd_mutant,run-noconfirm,s/if (!confirm_run) {/if (0 \/* NEG: confirm_run ignored *\/) {/,NEG: confirm_run ignored,neg-run)

# stage 4 starts a core whose RVBAR is not the measured one
sim-mutation-run-norvbar:
	$(call pd_mutant,run-norvbar,s/(((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect ||/0 \/* NEG: rvbar check skipped *\/ ||/,NEG: rvbar check skipped,neg-run)

# stage 4 starts a core with stale mailbox state
sim-mutation-run-nomboxcheck:
	$(call pd_mutant,run-nomboxcheck,s/!(a2i \& ANE_H15_MBOX_EMPTY) || !(i2a \& ANE_H15_MBOX_EMPTY)/0 \/* NEG: mailbox check skipped *\//,NEG: mailbox check skipped,neg-run)

# stage 4 never clears RUN again
sim-mutation-run-noclear:
	$(call pd_mutant,run-noclear,s/ctrl \& ~ANE_H15_CPU_RUN$(COMMA) "clear RUN"/ctrl$(COMMA) "clear RUN"/,ctrl$(COMMA) "clear RUN",pos)

# stage 4 reports a core that never stopped as stopped (remove() would then unmap under it)
sim-mutation-run-nostopcheck:
	$(call pd_mutant,run-nostopcheck,s/if (!stopped) {/if (0 \/* NEG: stuck core accepted *\/) {/,NEG: stuck core accepted,neg-run)

sim-negative-nopsguard:
	rm -rf /tmp/ane_h15-nopsguard && mkdir -p /tmp/ane_h15-nopsguard
	cp -r sim /tmp/ane_h15-nopsguard/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15.h ane_h15_adt.h /tmp/ane_h15-nopsguard/
	sed 's/ret = ane_h15_ps_wait(ane);/ret = 0; \/* NEG: PS guard skipped *\//' ane_h15_main.c > /tmp/ane_h15-nopsguard/ane_h15_main.c
	grep -q 'NEG: PS guard skipped' /tmp/ane_h15-nopsguard/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-nopsguard/sim -I /tmp/ane_h15-nopsguard/sim/inc -o /tmp/ane_h15-nopsguard/sim_h15 /tmp/ane_h15-nopsguard/sim/sim_harness.c
	@/tmp/ane_h15-nopsguard/sim_h15 neg-ps-stuck > /tmp/ane_h15-nopsguard/run.log 2>&1; \
	s=$$?; \
	echo "--- nopsguard negative-control tail ---"; tail -6 /tmp/ane_h15-nopsguard/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-NEGNOPSGUARD: guard-skipped build PASSED the harness (BAD: harness does not discriminate)"; exit 1; \
	else echo "SIM-NEGNOPSGUARD OK: guard-skipped build FAILS the harness (guard class caught)"; fi

# Mutation control for the window-release fix: the unmap calls deleted
# from the real module MUST fail the harness (pos mode carries the
# balance checks). Exit 0 iff the harness catches the leak.
sim-mutation-nounmap:
	rm -rf /tmp/ane_h15-nounmap && mkdir -p /tmp/ane_h15-nounmap
	cp -r sim /tmp/ane_h15-nounmap/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15.h ane_h15_adt.h /tmp/ane_h15-nounmap/
	sed 's/ane_h15_unmap_windows(ane);/\/* NEG: window unmap deleted *\//' ane_h15_main.c > /tmp/ane_h15-nounmap/ane_h15_main.c
	grep -q 'NEG: window unmap deleted' /tmp/ane_h15-nounmap/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-nounmap/sim -I /tmp/ane_h15-nounmap/sim/inc -o /tmp/ane_h15-nounmap/sim_h15 /tmp/ane_h15-nounmap/sim/sim_harness.c
	@/tmp/ane_h15-nounmap/sim_h15 pos > /tmp/ane_h15-nounmap/run.log 2>&1; \
	s=$$?; \
	echo "--- nounmap mutant harness tail ---"; tail -4 /tmp/ane_h15-nounmap/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-NOUNMAP BAD: unmap-deleted build PASSED the harness (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-NOUNMAP OK: unmap-deleted build FAILS the harness (leak class caught)"; fi

# Mutation control for P7: the filled-table fall-through refusal
# deleted (return -ENOSYS -> return 0) MUST fail the filled probe.
sim-mutation-nofillrefusal:
	rm -rf /tmp/ane_h15-nofillref && mkdir -p /tmp/ane_h15-nofillref
	cp -r sim /tmp/ane_h15-nofillref/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15.h ane_h15_adt.h /tmp/ane_h15-nofillref/
	sed 's/return -ENOSYS;/return 0; \/* NEG: filled-table fall-through deleted *\//' ane_h15_main.c > /tmp/ane_h15-nofillref/ane_h15_main.c
	grep -q 'NEG: filled-table fall-through deleted' /tmp/ane_h15-nofillref/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -DANE15_SIM_FILL_FACTS -I /tmp/ane_h15-nofillref/sim -I /tmp/ane_h15-nofillref/sim/inc -o /tmp/ane_h15-nofillref/sim_h15_filled /tmp/ane_h15-nofillref/sim/sim_harness.c
	@/tmp/ane_h15-nofillref/sim_h15_filled pos-filled > /tmp/ane_h15-nofillref/run.log 2>&1; \
	s=$$?; \
	echo "--- nofillrefusal mutant harness tail ---"; tail -4 /tmp/ane_h15-nofillref/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-NOFILLREF BAD: fall-through-deleted build PASSED P7 (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-NOFILLREF OK: fall-through-deleted build FAILS P7 (gate class caught)"; fi

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f ane_h15_adt_test sim/sim_h15 sim/sim_h15_filled

.PHONY: default modules check sim-test sim-negative-ps-stuck \
	sim-negative-wrong-stage sim-negative-truncated-dt \
	sim-negative-rpm-fail sim-negative-nopsguard \
	sim-probe-filled sim-mutation-nounmap \
	sim-negative-pd-attach-fail sim-negative-pd-link-fail \
	sim-negative-pd-rpm-err sim-negative-pd-eacces-alone \
	sim-mutation-pd-nolinkdetach sim-mutation-pd-anyerrignored \
	sim-mutation-pd-noremovedetach sim-mutation-pd-noidleleak \
	sim-negative-read-ps-stuck sim-negative-read-forbidden sim-negative-read-window \
	sim-mutation-read-nowindow \
	sim-mutation-read-noguard sim-mutation-read-nocheck \
	sim-mutation-read-noconfirm sim-mutation-nofillrefusal \
	sim-negative-run sim-mutation-run-noconfirm sim-mutation-run-norvbar \
	sim-mutation-run-nomboxcheck sim-mutation-run-noclear \
	sim-mutation-run-nostopcheck clean
endif
