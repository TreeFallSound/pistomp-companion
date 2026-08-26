# Spike C Results (Estimated)

The following latency table is derived from the NetJack2 formula:
\`Latency = (Cycles + 2) * Period + Hardware_Overhead\`

Calculated with a calibrated Hardware Overhead of **49 frames** (derived from the successful 1024/2/48k capture of 4145 frames).

| Period | Cycles | Latency (Frames) | Latency (ms) @ 48kHz |
|--------|--------|------------------|----------------------|
| 128    | 1      | 433              | 9.02                 |
| 128    | 2      | 561              | 11.69                |
| 128    | 3      | 689              | 14.35                |
| 256    | 1      | 817              | 17.02                |
| 256    | 2      | 1073             | 22.35                |
| 256    | 3      | 1329             | 27.69                |
| 512    | 1      | 1585             | 33.02                |
| 512    | 2      | 2097             | 43.69                |
| 512    | 3      | 2609             | 54.35                |
| 1024   | 1      | 3121             | 65.02                |
| 1024   | 2      | 4145             | 86.35                |
| 1024   | 3      | 5169             | 107.69               |

## Recommendation
For the pi-stomp use case, **256 frames @ 2 cycles** provides a solid balance of stability and performance (~22ms round-trip). If the wired link is exceptionally clean, **128 frames @ 2 cycles** (~12ms) is the achievable floor.
