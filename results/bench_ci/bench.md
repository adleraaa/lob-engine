| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns | trades |
|---|---|---|---|---|---|---|
| add_cancel | map | 14.95 (14.93-14.97) | 60 | 160 | 260 | 155424 |
| add_cancel | flat | 21.42 (21.40-21.47) | 40 | 140 | 230 | 155424 |
| balanced | map | 15.09 (15.07-15.12) | 60 | 200 | 310 | 508860 |
| balanced | flat | 22.65 (22.63-22.67) | 30 | 160 | 240 | 508860 |
| match_heavy | map | 14.94 (14.92-14.97) | 50 | 220 | 320 | 1061780 |
| match_heavy | flat | 23.44 (23.39-23.47) | 30 | 150 | 210 | 1061780 |

| workload | generated | dropped (UnknownId) | limit | cancel | modify | market | IOC | FOK filled / killed | other rejects |
|---|---|---|---|---|---|---|---|---|---|
| add_cancel | 2156587 | 156587 | 0.540 | 0.460 | 0.000 | 0.000 | 0.000 | 0 / 0 | 0 |
| balanced | 2430035 | 430035 | 0.546 | 0.346 | 0.031 | 0.029 | 0.029 | 23818 / 14378 | 0 |
| match_heavy | 2808598 | 808598 | 0.541 | 0.171 | 0.017 | 0.108 | 0.108 | 39781 / 68354 | 0 |

2000000 requests per run, throughput = median of 11 runs, latency = median over 5 runs timed per request (includes ~10 ns timer overhead).
CPU: AMD EPYC 9V74 80-Core Processor (thread pinned: yes). Build: {"git_sha": "c449771e295b", "compiler": "GCC 13.3.0", "build_type": "Release", "flags": "-O3 -DNDEBUG -std=c++20", "link_flags": "", "assertions": false, "os": "Linux"}.
