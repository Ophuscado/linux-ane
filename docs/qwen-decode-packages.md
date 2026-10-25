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
| int8 with GPTQ weights, mixed calibration | M1 (H13) | `829b43ab2f42e962d85617a8ce47ac08d17286c6c035c842f5a29f1fce4d1bf5` |
| int8 with GPTQ weights, mixed calibration | M2 Max (H14) | `539b3c73d09e187474d6777f95b9068c9234f8cef92abc3e7e4542daac31a116` |
| int8 with GPTQ weights, mixed calibration, attention in fp16 | M1 (H13) | `3fbb7a306a1a515aff14574165ac6ad9bad912bbbbe7466c52a5092494ab91d5` |
| int8 with GPTQ weights, mixed calibration, attention in fp16 | M2 Max (H14) | `d628ebc486deb0bddc84817a09dc16404a49047cb4c5ef3229527249c062de92` |

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

## Mixed calibration

The first GPTQ package was calibrated on prose and multilingual prompts. The mixed package adds code: its calibration is 20 prose, 20 multilingual and 24 code prompts (the code prompts come from 12 of the 25 source documents of the code set) at 128 forced positions, 9,136 captured rows per tensor (the 64 prompts give 8,192 forced positions and 944 prompt-token rows). The 13 other source documents (26 prompts) stay unseen as the code test. The 13-inch M1 ran all three sets with the fp16 package and both GPTQ packages. KL is against the fp32 reference. The decode uses the older runner without the Q6_K head, so the numbers differ from the tables above.

| Set | fp16 | int8 GPTQ | int8 GPTQ, mixed |
|---|---|---|---|
| technical, 50 prompts of 128 positions: KL, top-1 | 0.000267 | 0.000397, 0.9958 | 0.000390, 0.9956 |
| code, the 26 unseen prompts: KL, top-1 | 0.000254, 0.9958 | 0.000407 (1.60 times), 0.9934 | 0.000351 (1.38 times), 0.9952 |
| long position, 50 prompts of 170 positions: KL, top-1 | 0.000293, 0.9935 | 0.000605 (2.06 times), 0.9896 | 0.000663 (2.26 times), 0.9898 |

The mixed package passes the same rule as before on the code prompts it never saw (at most 2 times the fp16 KL and top-1 within 0.001). The first package failed it on top-1 there. On the long-position prompts neither package passes, and the mixed one is 10 percent worse. Those prompts are the old prompt set. Neither calibration contains them. Against the first GPTQ package the mixed package costs nothing on the technical set (0.000390 against 0.000397; fp16 is 0.000267). The selective package below replaces both as the speed tier. The mixed H14 package for the M2 Max has not been run for quality. Its step on the 13-inch M1 was 29.4 percent lower than fp16 in one run under GPU load. The weight bytes are the same as the first package.

## Selective int8, the speed tier

The mixed package keeps the quality cost of int8 on long text. A scan with a CPU model showed where it comes from. It put the largest share on the early attention programs. So the selective package keeps the five attention programs in fp16 (programs 6, 12, 18, 25 and 31) and keeps the ten MLP programs in int8. It uses the mixed calibration. It gives back about half of the int8 weight-byte saving. It replaces the mixed package as the opt-in speed tier.

Quality against the fp32 reference, on the same three sets as above. The 13-inch M1 used the older runner without the Q6_K head. The 14-inch M2 Max used the staged decode with 50 prompts per set. The shipped numbers are in the fp16 column of the same chip:

| Chip | Set | fp16: KL, top-1 | Selective: KL, top-1 | KL ratio | Top-1 gap |
|---|---|---|---|---|---|
| 13-inch M1 | technical, 50 prompts of 128 positions | 0.000267, 0.9955 | 0.000330, 0.9955 | 1.24 | 0.0000 |
| 13-inch M1 | code, the 26 unseen prompts | 0.000254, 0.9958 | 0.000299, 0.9949 | 1.18 | 0.0009 |
| 13-inch M1 | long position, 50 prompts of 170 positions | 0.000293, 0.9935 | 0.000453, 0.9912 | 1.54 | 0.0024 |
| 14-inch M2 Max | technical | 0.000269, 0.9955 | 0.000333, 0.9956 | 1.24 | -0.0001 |
| 14-inch M2 Max | code, the 26 unseen prompts | 0.000256, 0.9958 | 0.000303, 0.9952 | 1.18 | 0.0006 |
| 14-inch M2 Max | long position | 0.000293, 0.9926 | 0.000450, 0.9911 | 1.54 | 0.0015 |

The selective package passes the technical gate and the code rule on both chips. On the long-position prompts the KL is 1.54 times the fp16 KL on both chips, which is under the limit of 2 times. The top-1 limit is 0.001. The package misses it on both chips: the gap is 0.0024 on the 13-inch M1 and 0.0015 on the 14-inch M2 Max. The limit was set before the runs and stays. So the package is a speed tier and the fp16 package stays the default. The mixed package read 2.26 times on the long-position prompts of the 13-inch M1, and the first GPTQ package 2.06 times.

Step time, p50. The shipped package and the selective package were interleaved in one session, with the arms of each pair within 1 percent of each other:

| Chip | fp16 | Selective | Change | First GPTQ package |
|---|---|---|---|---|
| 13-inch M1, under GPU load | 0.0863 s | 0.0732 s | 14.5 percent lower (median of 4 runs, from 13.6 to 15.4; the values shown are from the quietest run) | 29.4 percent lower |
| 14-inch M2 Max | 0.1580 s | 0.1401 s | 11.3 percent lower | 21.3 percent lower |
| 16-inch M1 Max, level driver | 0.1189 s | 0.1086 s | 8.7 percent lower (median of 3 runs, from 7.9 to 8.8; the values shown are from one of them) | 16.6 percent lower |

The selective package keeps about half of the step gain of the first GPTQ package. The 16-inch M1 Max has not run the quality sets.

## What this does not show

The 16-inch M1 Max has not run the int8 GPTQ package for quality. The fp16 KL on the M2 Max comes from an earlier run. Plain int8 is not measured on the code and long-position sets, so the split of the cost between int8 and GPTQ is open.
