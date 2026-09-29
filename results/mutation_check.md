Difftest run per mutant: 4 scenarios x 1 seed x 20000 requests, built with -O2 and assertions enabled (no NDEBUG).

| mutation | injected bug | caught | first report |
|---|---|---|---|
| limit_boundary | a buy order does not trade at exactly its limit price | yes | MISMATCH (exec report) scenario=balanced seed=1 op#57: add id=39 side=buy type=Limit price=9996 qty=61 |
| lifo_queue | newest order at a price trades first (LIFO instead of FIFO) | yes | MISMATCH (trades) scenario=balanced seed=1 op#84: add id=57 side=buy type=IOC price=10007 qty=36 |
| modify_same_qty_requeues | modify to the same quantity sends the order to the back of the queue | yes | MISMATCH (level updates) scenario=balanced seed=1 op#5620: modify id=2974 side=buy type=Limit price=9992 qty=10 |
| modify_down_no_event | modify-down changes the book but emits no level update | yes | MISMATCH (level updates) scenario=balanced seed=1 op#27: modify id=11 side=buy type=Limit price=9980 qty=63 |
| fok_exact_liquidity_killed | FOK is killed when available liquidity exactly equals its quantity | yes | MISMATCH (exec report) scenario=balanced seed=1 op#4305: add id=2724 side=buy type=FOK price=10003 qty=15 |
| level_total_not_reduced_on_erase | removing an order does not subtract its quantity from the level total | yes | MISMATCH (level updates) scenario=balanced seed=1 op#11: add id=7 side=sell type=Limit price=10036 qty=76 |
| map_best_bid_is_lowest | map book: best bid is the lowest bid instead of the highest | yes | MISMATCH (exec report) scenario=balanced seed=1 op#32: add id=22 side=sell type=Limit price=9999 qty=53 |
| flat_best_not_improved | flat book: a new better price does not become the best level | yes | Assertion failed: order->level == this && by > 0 && by < order->qty, file <tmp>\include/lob/price_level.hpp, line 83 |
| idmap_no_backward_shift | hash map deletion leaves holes that cut probe chains | yes | MISMATCH (exec report) scenario=balanced seed=1 op#646: modify id=222 side=buy type=Limit price=9999 qty=38 |
