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
