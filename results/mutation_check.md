Difftest run per mutant: every scenario x 1 seed x 20000 requests, built with `-std=c++20 -O2 -DNDEBUG -static` (assertions off). Source a42698084176, g++ (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r1) 16.2.0, Windows.

Caught: 9 / 9, of which 9 by a result mismatch (the rest, if any, by a crash).

| mutation | injected bug | caught | how | first report |
|---|---|---|---|---|
| limit_boundary | a buy order does not trade at exactly its limit price | yes | mismatch | MISMATCH (exec report) scenario=balanced seed=1 op#57: add id=39 side=buy type=Limit price=9996 qty=61 |
| lifo_queue | newest order at a price trades first (LIFO instead of FIFO) | yes | mismatch | MISMATCH (trades) scenario=balanced seed=1 op#84: add id=57 side=buy type=IOC price=10007 qty=36 |
| modify_same_qty_requeues | modify to the same quantity sends the order to the back of the queue | yes | mismatch | MISMATCH (level updates) scenario=balanced seed=1 op#5620: modify id=2974 side=buy type=Limit price=9992 qty=10 |
| modify_down_no_event | modify-down changes the book but emits no level update | yes | mismatch | MISMATCH (level updates) scenario=balanced seed=1 op#27: modify id=11 side=buy type=Limit price=9980 qty=63 |
| fok_exact_liquidity_killed | FOK is killed when available liquidity exactly equals its quantity | yes | mismatch | MISMATCH (exec report) scenario=balanced seed=1 op#4305: add id=2724 side=buy type=FOK price=10003 qty=15 |
| level_total_not_reduced_on_erase | removing an order does not subtract its quantity from the level total | yes | mismatch | MISMATCH (level updates) scenario=balanced seed=1 op#11: add id=7 side=sell type=Limit price=10036 qty=76 |
| map_best_bid_is_lowest | map book: best bid is the lowest bid instead of the highest | yes | mismatch | MISMATCH (exec report) scenario=balanced seed=1 op#32: add id=22 side=sell type=Limit price=9999 qty=53 |
| flat_best_not_improved | flat book: a new better price does not become the best level | yes | mismatch | MISMATCH (best bid/ask) scenario=balanced seed=1 op#13: add id=9 side=buy type=Limit price=9999 qty=51 |
| idmap_no_backward_shift | hash map deletion leaves holes that cut probe chains | yes | mismatch | MISMATCH (exec report) scenario=balanced seed=1 op#646: modify id=222 side=buy type=Limit price=9999 qty=38 |
