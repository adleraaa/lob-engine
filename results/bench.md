| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns | trades |
|---|---|---|---|---|---|---|
| add_cancel | map | 7.16 (5.68-11.85) | 75 | 277 | 1180 | 144648 |
| add_cancel | flat | 11.38 (8.87-19.62) | 55 | 200 | 804 | 144648 |
| balanced | map | 11.31 (10.05-13.36) | 69 | 316 | 1053 | 420544 |
| balanced | flat | 15.20 (12.25-19.06) | 51 | 224 | 884 | 420544 |
| match_heavy | map | 10.40 (7.86-12.17) | 67 | 382 | 1254 | 755940 |
| match_heavy | flat | 13.95 (8.07-18.72) | 50 | 260 | 1012 | 755940 |

2000000 requests per run, throughput = median of 11 runs, latency = one run timed per request (includes ~22 ns timer overhead).
CPU: Intel(R) Core(TM) i9-14900HX (Windows, thread pinned: yes). Compiler: GCC 16.2.0. Flags: -O3 -DNDEBUG -std=c++20.
