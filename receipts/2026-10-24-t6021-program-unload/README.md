# T6021 firmware program unload: what the selene 13.5 firmware does

Source: the firmware image `selene-13v5.macho` (the 13.5 image the T6021 driver loads, sha256 a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc). Disassembled statically. Nothing was run. All addresses are firmware virtual addresses. File offset is the address plus 0x4000.

## Dispatch

The host command handler at 0x270dc reads the u16 command id at buffer +4. It subtracts 0x200 and checks the result against 0xe. A jump table of 15 offsets at 0x2a658 routes the ids. Id 0x201 goes to 0x28348, 0x202 to 0x28374, 0x203 to 0x283a0 and 0x204 to 0x283cc. Each of these only logs the id when a debug flag is set, then joins the work block: 0x28eb8 for 0x201, 0x28f88 for 0x202, 0x29090 for 0x203 and 0x29170 for 0x204.

## Wire layout

Both commands leave +0 unused. The u16 at +4 is the id and the u16 at +6 is the completion code.

- 0x201 is 12 bytes. It reads one u32, the programId at +8. It calls destroyProgram at 0x36adc.
- 0x203 is 16 bytes. It reads the programId at +8 and the processId at +0xc (the log string at 0xb41fa names them). It calls the program manager's terminateProcess at 0x41a24.

Neither command writes a result field back.

## Completion

The firmware zeroes the u16 at +6 when it queues a command (0x27140). The completion routine at 0x2a694 leaves it at 0 for success and writes 2 for a failure. Only 0x200 (load) and 0x202 (create process) have a failure path that completes with 2. For 0x201 and 0x203 a precondition violation is an assert that halts the firmware, and no completion is delivered at all.

## What each command does

- 0x203 sets the process state from 1 to 2, destroys its members, frees its L2 spill buffers (log string at 0xb9523), frees the process slot and decrements the program's process count. It is a per-process terminate, not a program unload.
- 0x201 asserts the program is in use, removes its five section lists (procBuf, kernelSection, textSection, operationSection, genericSection), asserts the program's process count is 0 (a nonzero count halts the firmware, string "nbr of proc=%d" at 0xb6dac), clears the used flag of its slot and destroys the program image.

So the acknowledged unload of a program is 0x203 for every process of it, then 0x201.

## Not proven

The firmware unmaps the section buffers when the program image is destroyed. That edge, from 0x36adc to the unmap call at 0x4746c (strings "Unmap genericSection failed ...", 0xbaeee), goes through virtual calls built at run time, so it is not visible statically. The driver therefore treats the buffers as free only after both commands were acknowledged, and a board run has to confirm that no later fault follows.

## The free request (found by the first board run)

The first run of the unload on a 14-inch M2 failed at the first 0x203. The driver returned -95 (EOPNOTSUPP) from its own check of the firmware's shared-memory ring. The firmware did not return it. A decode of the same image explains it (selene 13v5, sha256 a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc).

- When 0x203 drains the L2 spill buffer pool, the firmware calls CSharedMemory::Free (0x21658). It is reached from freeUnusedL2SpillBufferPool (0x42164) inside TerminateProcess (0x41a24).
- Free unmaps the buffer on the firmware side and posts one slot on the SHAREDMALLOC ring. The header is the IOVA it gives back (bit 0 clear, not 0). Size and mode are 0. The slot writer is at 0x2580c and the send at 0x21780.
- The acknowledgement of 0x203 is already written by then. The request lands while the host still waits for the command.
- The firmware waits for the host's answer with no timeout (semaphore wait at 0x2178c). It holds its shared-memory lock and the program manager lock meanwhile.
- With no answer, 0x203 looks finished on the wire and the program manager stays wedged. The answer is the one a MALLOC gets: body words 0, header 1, doorbell bit 5.
- The driver took the slot for a malformed MALLOC and aborted. That is why the program stayed held and the device was quarantined.
- Any pool shrink in the firmware can post the same request. It is not limited to 0x203.

The driver now answers it. `ane_rtclient_malloc_service` classifies every ring slot with `ane_t6021_mring_classify` and answers a FREE without allocating. A host test covers the classifier (`tools/test_t6021_mring.c`). The ring is served before each command is sent, while the driver waits, and for 10 ms of quiet after 0x203 and after 0x201. The driver matches the buffer the firmware gave back to its table entry and frees it once the answer is out.

Proven by the decode: the slot shape, the wait, the lock span and the call chain. Inferred: that the doorbell write on the firmware side is the interrupt the host sees. Also inferred: that the SHAREDMALLOC channel is the ring the driver already uses (checked against the firmware-built table at every boot). Not run: any device. `unload_on_close` stays 0 until the proof passes twice on an M2.

## Board results on an M2 (three rounds)

- Round 1: the first 0x203 aborted in the driver's own ring check. Cause: the firmware's FREE request, now answered. The device was quarantined and a second session could not open it.
- Round 2 (free request answered): three decode sessions passed. The fourth and fifth failed in the middle of a LOAD. Cause: the table of firmware buffers kept freed entries. A FREE now releases its entry.
- Round 3 (both fixes, `unload_on_close=1`, six decode sets as separate processes): every set returned 0 and the logits were byte identical to the first set. The step time stayed within 0.1 percent (0.1592 to 0.1594 s). `bo_total_bytes` after a pool drain was 50.6, 53.3, 53.7, 53.9, 53.9 and 53.9 MB. The table of firmware buffers stayed at 17 slots and 475,136 bytes at every returned buffer, against a limit of 512 MiB. There were no fault lines.

One M2 and one decode workload. A different workload, a different number of programs per session or a firmware that frees in another pattern may behave differently. Not run on an M1 or M1 Max.
