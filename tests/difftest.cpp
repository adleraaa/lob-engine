// Randomized differential test.
//
// Feeds the same generated request stream to three books - the simple
// ReferenceBook oracle, MapOrderBook and FlatOrderBook - and checks after
// every request that all three returned the same result and emitted exactly
// the same trades and level updates, in the same order. Full book snapshots
// (every order, level and queue position) are compared periodically and at
// the end. Any difference stops the run with a description of the request.
//
// Usage: lob_difftest [--ops N] [--seeds K] [--first-seed S] [--out file.json]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "order_flow.hpp"
#include "reference_book.hpp"

namespace {

using namespace lob;

struct Scenario {
    const char* name;
    flow::Mix mix;
};

// Different mixes stress different paths: deep passive books with lots of
// cancels, aggressive flow that sweeps several levels, and a stream with
// deliberately invalid requests mixed in.
std::vector<Scenario> scenarios() {
    std::vector<Scenario> list;
    list.push_back({"balanced", flow::Mix{}});
    flow::Mix aggressive;
    aggressive.cross = 0.45;
    aggressive.market = 0.08;
    aggressive.ioc = 0.08;
    aggressive.fok = 0.08;
    list.push_back({"aggressive", aggressive});
    flow::Mix passive;
    passive.cancel = 0.40;
    passive.modify = 0.15;
    passive.cross = 0.05;
    list.push_back({"passive_modify_heavy", passive});
    flow::Mix invalid;
    invalid.invalid = 0.05;
    list.push_back({"with_invalid_requests", invalid});
    return list;
}

struct Totals {
    std::uint64_t ops = 0;
    std::uint64_t trades = 0;
    std::uint64_t level_updates = 0;
    std::uint64_t rejected = 0;
    std::uint64_t snapshot_checks = 0;
    std::size_t max_resting_orders = 0;
};

std::string describe(const flow::Op& op) {
    const char* kind =
        op.kind == flow::Op::Kind::Add ? "add" : (op.kind == flow::Op::Kind::Cancel ? "cancel" : "modify");
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s id=%llu side=%s type=%s price=%lld qty=%lld", kind,
                  static_cast<unsigned long long>(op.id), op.side == Side::Buy ? "buy" : "sell",
                  std::string(to_string(op.type)).c_str(), static_cast<long long>(op.price),
                  static_cast<long long>(op.qty));
    return buf;
}

template <typename Book>
ExecReport apply(Book& book, const flow::Op& op) {
    switch (op.kind) {
        case flow::Op::Kind::Add: return book.add(op.id, op.side, op.type, op.price, op.qty);
        case flow::Op::Kind::Cancel: return ExecReport{book.cancel(op.id), 0, 0, 0};
        case flow::Op::Kind::Modify: return book.modify(op.id, op.price, op.qty);
    }
    return {};
}

// Returns false (after printing why) on the first divergence.
bool run(const Scenario& scenario, std::uint64_t seed, std::size_t op_count, Totals& totals) {
    flow::Config cfg;
    cfg.seed = seed;
    cfg.mix = scenario.mix;
    cfg.max_live_ids = 1000;  // keeps the O(n) reference fast enough for millions of requests
    flow::Generator gen(cfg);

    RecordingListener ref_events, map_events, flat_events;
    ref::ReferenceBook reference(ref_events, {cfg.min_price, cfg.max_price});
    MapOrderBook map_book(map_events);
    FlatOrderBook flat_book(flat_events, {cfg.min_price, cfg.max_price});

    constexpr std::size_t kSnapshotEvery = 1000;
    for (std::size_t i = 0; i < op_count; ++i) {
        const flow::Op op = gen.next();
        const ExecReport want = apply(reference, op);
        const ExecReport got_map = apply(map_book, op);
        const ExecReport got_flat = apply(flat_book, op);

        const char* problem = nullptr;
        if (got_map != want || got_flat != want) {
            problem = "exec report";
        } else if (map_events.trades != ref_events.trades || flat_events.trades != ref_events.trades) {
            problem = "trades";
        } else if (map_events.updates != ref_events.updates || flat_events.updates != ref_events.updates) {
            problem = "level updates";
        } else if ((i + 1) % kSnapshotEvery == 0 || i + 1 == op_count) {
            const BookSnapshot want_snap = reference.snapshot();
            ++totals.snapshot_checks;
            if (map_book.snapshot() != want_snap || flat_book.snapshot() != want_snap) {
                problem = "book snapshot";
            }
            if (reference.order_count() > totals.max_resting_orders) {
                totals.max_resting_orders = reference.order_count();
            }
        }
        if (problem != nullptr) {
            std::fprintf(stderr, "MISMATCH (%s) scenario=%s seed=%llu op#%zu: %s\n", problem, scenario.name,
                         static_cast<unsigned long long>(seed), i, describe(op).c_str());
            return false;
        }

        ++totals.ops;
        totals.trades += ref_events.trades.size();
        totals.level_updates += ref_events.updates.size();
        totals.rejected += want.status != Status::Ok ? 1 : 0;
        ref_events.clear();
        map_events.clear();
        flat_events.clear();
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 20'000;
    std::uint64_t seeds = 2;
    std::uint64_t first_seed = 1;
    const char* out_path = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--ops") == 0)
            ops = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--seeds") == 0)
            seeds = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--first-seed") == 0)
            first_seed = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--out") == 0)
            out_path = argv[i + 1];
        else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    Totals totals;
    std::size_t runs = 0;
    for (const Scenario& scenario : scenarios()) {
        for (std::uint64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
            if (!run(scenario, seed, ops, totals)) {
                return 1;
            }
            ++runs;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf(
        "OK: %zu runs (%zu scenarios x %llu seeds), %llu requests, %llu trades, %llu level updates, "
        "%llu rejected requests, %llu snapshot comparisons, max %zu resting orders, %.1f s\n",
        runs, scenarios().size(), static_cast<unsigned long long>(seeds), static_cast<unsigned long long>(totals.ops),
        static_cast<unsigned long long>(totals.trades), static_cast<unsigned long long>(totals.level_updates),
        static_cast<unsigned long long>(totals.rejected), static_cast<unsigned long long>(totals.snapshot_checks),
        totals.max_resting_orders, seconds);

    if (out_path != nullptr) {
        std::ofstream out(out_path);
        out << "{\n"
            << "  \"result\": \"all books identical\",\n"
            << "  \"scenarios\": " << scenarios().size() << ",\n"
            << "  \"seeds_per_scenario\": " << seeds << ",\n"
            << "  \"first_seed\": " << first_seed << ",\n"
            << "  \"requests_per_run\": " << ops << ",\n"
            << "  \"total_requests\": " << totals.ops << ",\n"
            << "  \"trades_compared\": " << totals.trades << ",\n"
            << "  \"level_updates_compared\": " << totals.level_updates << ",\n"
            << "  \"rejected_requests\": " << totals.rejected << ",\n"
            << "  \"full_snapshot_comparisons\": " << totals.snapshot_checks << ",\n"
            << "  \"max_resting_orders_seen\": " << totals.max_resting_orders << ",\n"
            << "  \"wall_seconds\": " << seconds << "\n"
            << "}\n";
    }
    return 0;
}
