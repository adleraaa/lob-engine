| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns | trades |
|---|---|---|---|---|---|---|
| add_cancel | map | 19.16 (18.74-20.06) | 51 | 122 | 203 | 155424 |
| add_cancel | flat | 28.88 (27.61-29.12) | 38 | 105 | 169 | 155424 |
| balanced | map | 20.07 (19.42-20.78) | 49 | 168 | 268 | 508860 |
| balanced | flat | 30.56 (29.76-31.48) | 32 | 126 | 198 | 508860 |
| match_heavy | map | 19.46 (17.59-20.24) | 49 | 184 | 278 | 1061780 |
| match_heavy | flat | 31.10 (28.50-31.97) | 33 | 125 | 184 | 1061780 |

| workload | generated | dropped (UnknownId) | limit | cancel | modify | market | IOC | FOK filled / killed | other rejects |
|---|---|---|---|---|---|---|---|---|---|
| add_cancel | 2156587 | 156587 | 0.540 | 0.460 | 0.000 | 0.000 | 0.000 | 0 / 0 | 0 |
| balanced | 2430035 | 430035 | 0.546 | 0.346 | 0.031 | 0.029 | 0.029 | 23818 / 14378 | 0 |
| match_heavy | 2808598 | 808598 | 0.541 | 0.171 | 0.017 | 0.108 | 0.108 | 39781 / 68354 | 0 |

2000000 requests per run, throughput = median of 11 runs, latency = median over 5 runs timed per request (includes ~7 ns timer overhead).
CPU: Intel(R) Core(TM) i9-14900HX (thread pinned: yes). Build: {"git_sha": "57a3ddc3e131", "compiler": "GCC 16.2.0", "build_type": "Release", "flags": "-O3 -DNDEBUG -std=c++20", "link_flags": "-static", "assertions": false, "os": "Windows"}.
