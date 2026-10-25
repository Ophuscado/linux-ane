# Qwen decode packages

This page lists the compiled Qwen3.8-2B decode packages that the staged decode runs. It gives their seals and measured numbers. The packages are not in this repo. The compiler builds them. A package seal is the sha256 of the package's `SHA256SUMS` file.

The staged decode keeps the Q6_K head on the CPU. It runs the 38 layer programs on the ANE. Every number below uses 50 held-out prompts of 128 positions. The decode is teacher forced on fixed ids. The logits are compared against an fp32 reference. The quality gate is KL at most 0.000534 and top-1 at least 0.9945. The KL limit is twice that of the fp16 package.

## Packages

| Package | Chip family | Seal |
|---|---|---|
| fp16 | M1 (H13) | `bf54d842c05cd5a5e88e85b5f97bf0228d7fb57ef9fcb711c3a7a4980915fd4a` |
| fp16 | M2 Max (H14) | `f04e973afdce67b0671e726926464ce1821861738ddad63d8454732def580116` |
| int8 with GPTQ weights | M1 (H13) | `91ff7b47b50ac6014f35278e9c23a60a2f148d743ec0735835f8e0ce181e3a3c` |
| int8 with GPTQ weights | M2 Max (H14) | `de3d4dab6cea2fe8997f83207266c77902d64ed9b4ec1535833fbb60be4cbcf8` |

The int8 package replaces the weights of 15 of the 17 large programs with int8 per output row. Two programs stay fp16, because plain int8 cost them too much quality. The weights come from GPTQ. Each row is rounded to minimise the error on the layer's real activations. The 80 calibration prompts are not in the test set.

## Results

Step time, p50 over the prompts, measured in one session on one boot with the arms interleaved:

| Chip | Pair | First | Second | Change |
|---|---|---|---|---|
| 13-inch M1 | fp16, int8 GPTQ | 0.0863 s | 0.0610 s | 29.4 percent lower (means of both arms of each) |
| 14-inch M2 Max | fp16, int8 GPTQ | 0.1579 s | 0.1243 s | 21.3 percent lower |
| 13-inch M1 | int8 plain rounding, int8 GPTQ | 0.0678 s | 0.0680 s | equal (a different harness, so not comparable with the rows above) |
| 14-inch M2 Max | int8 plain rounding, int8 GPTQ | 0.1234 s | 0.1232 s | equal |

Quality against the fp32 reference, from the quality harness:

| Chip | Package | KL | Top-1 |
|---|---|---|---|
| 13-inch M1 | fp16 | 0.000267 | 0.9955 |
| 13-inch M1 | int8, plain rounding | 0.000540 | 0.9944 |
| 13-inch M1 | int8, GPTQ | 0.000397 | 0.9958 |
| 14-inch M2 Max | fp16 | 0.000269 | 0.9955 |
| 14-inch M2 Max | int8, plain rounding | 0.000547 | 0.9945 |
| 14-inch M2 Max | int8, GPTQ | 0.000400 | 0.9958 |

On the 13-inch the GPTQ package decodes 16.4 tokens per second against 11.6 for the fp16 package. With two decode streams in one process it reaches 18.05 scored steps per second against 12.12, which is 48.9 percent more.

The fp16 package is the default. The GPTQ package is an opt-in speed tier. It passes the gate on both chips, on the technical prompts it was measured on. Plain int8 missed it on both. Paired by prompt on the M2 Max, GPTQ lowers the KL by 27 percent against plain int8. It is lower on 45 of 50 prompts. A second GPTQ run with other damping choices gave the same KL to 0.3 percent. The first set is the package.

## Other prompt sets

The gate above uses technical text. The GPTQ calibration used prose and multilingual prompts. On the 14-inch M2 Max the package was also run on two sets it never saw, with the fp16 package in the same session:

| Set | Package | Step p50 | KL | Top-1 |
|---|---|---|---|---|
| code, 50 prompts of 128 positions | fp16 | 0.1575 s | 0.000289 | 0.9966 |
| code | int8, GPTQ | 0.1234 s | 0.000454 | 0.9953 |
| long position, 50 prompts of 170 positions | fp16 | 0.1574 s | 0.000293 | 0.9926 |
| long position | int8, GPTQ | 0.1243 s | 0.000604 | 0.9898 |

The step is 21.7 percent lower on the code set and 21.0 percent lower on the long-position set. The KL is 1.57 times the fp16 KL on code and 2.06 times on the long-position set. Top-1 is lower by 0.0013 and 0.0028. The rule set before the run asked for at most 2 times the fp16 KL and a top-1 within 0.001. Both sets miss it. So the int8 GPTQ package is a speed tier: it passes the technical gate and costs more quality on text unlike its calibration. Its KL is higher than the fp16 KL on 48 of 50 code prompts and on 49 of 50 long-position prompts. The 13-inch M1 has not run these two sets.

The recipe scripts and the calibration notes are in the compiler repo: `receipts/2026-10-24-int8-gptq-qwen38-h13-h14` in [mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler).

## What this does not show

The 16-inch M1 Max has not run the int8 GPTQ package for quality. The fp16 KL on the M2 Max comes from an earlier run. Plain int8 is not measured on the code and long-position sets, so the split of the cost between int8 and GPTQ is open.
