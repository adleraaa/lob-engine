// Randomized differential test.
//
// Feeds the same generated request stream to three books - the simple
// ReferenceBook oracle, MapOrderBook and FlatOrderBook - and checks after
// every request that all three returned the same result and emitted exactly
// the same trades and level updates, in the same order, and that they agree
// on the best bid and ask. Full book snapshots
// (every order, level and queue position) are compared periodically and at
// the end. Any difference stops the run with a description of the request.
//
// All three books get the same tick range, so they must also agree on which
// prices are PriceOutOfRange.
//
// Usage: lob_difftest [--ops N] [--seeds K] [--first-seed S] [--out file.json]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "build_info.hpp"
#include "cli.hpp"
#include "lob/order_book.hpp"
#include "order_flow.hpp"
#include "reference_book.hpp"

namespace {

using namespace lob;

struct Scenario {
    const char* name;
    flow::Config config;  // seed is set per run
};

// Different mixes stress different paths: deep passive books with lots of
// cancels, aggressive flow that sweeps several levels, a stream with
// deliberately invalid requests mixed in, and a tiny tick range that keeps
// the book pressed against both ends of the flat book's array.
std::vector<Scenario> scenarios() {
    flow::Config base;
    base.max_live_ids = 1000;  // keeps the O(n) reference fast enough for millions of requests

    std::vector<Scenario> list;
    list.push_back({"balanced", base});

    Scenario aggressive{"aggressive", base};
    aggressive.config.mix.cross = 0.45;
    aggressive.config.mix.market = 0.08;
    aggressive.config.mix.ioc = 0.08;
    aggressive.config.mix.fok = 0.08;
    list.push_back(aggressive);

    Scenario passive{"passive_modify_heavy", base};
    passive.config.mix.cancel = 0.40;
    passive.config.mix.modify = 0.15;
    passive.config.mix.cross = 0.05;
    list.push_back(passive);

    Scenario invalid{"with_invalid_requests", base};
    invalid.config.mix.invalid = 0.05;
    list.push_back(invalid);

    // Prices 1..300 with orders reaching up to 140 ticks from a mid that
    // wanders across the whole range: many orders are clamped onto price 1
    // or 300 (index 0 and the last index of the flat array), levels are
    // sparse, so the best-level scan crosses long empty gaps, and
    // out-of-range requests hit both edges.
    Scenario edge{"edge_range", base};
    edge.config.min_price = 1;
    edge.config.max_price = 300;
    edge.config.mid = 150;
    edge.config.passive_depth = 140;
    edge.config.cross_depth = 100;
    edge.config.max_live_ids = 60;
    edge.config.mix.invalid = 0.02;
    list.push_back(edge);
    return list;
}

constexpr int kStatusCount = static_cast<int>(Status::UnknownId) + 1;
constexpr int kKindCount = 3;  // flow::Op::Kind: Add, Cancel, Modify

struct Totals {
    std::uint64_t ops = 0;
    std::uint64_t trades = 0;
    std::uint64_t level_updates = 0;
    std::uint64_t rejected = 0;
    std::uint64_t by_kind_status[kKindCount][kStatusCount] = {};
    std::uint64_t snapshot_checks = 0;
    std::size_t max_resting_orders = 0;

    std::uint64_t status_total(int st) const {
        std::uint64_t n = 0;
        for (int k = 0; k < kKindCount; ++k) n += by_kind_status[k][st];
        return n;
    }
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
    flow::Config cfg = scenario.config;
    cfg.seed = seed;
    flow::Generator gen(cfg);

    RecordingListener ref_events, map_events, flat_events;
    ref::ReferenceBook reference(ref_events, {cfg.min_price, cfg.max_price});
    MapOrderBook map_book(map_events, {cfg.min_price, cfg.max_price});
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
        } else if (map_book.best_bid() != reference.best_bid() || flat_book.best_bid() != reference.best_bid() ||
                   map_book.best_ask() != reference.best_ask() || flat_book.best_ask() != reference.best_ask()) {
            // Cheap top-of-book check after every request, so a bug in
            // best-level tracking shows up at the request that caused it
            // instead of at a later crash or snapshot.
            problem = "best bid/ask";
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
        ++totals.by_kind_status[static_cast<int>(op.kind)][static_cast<int>(want.status)];
        ref_events.clear();
        map_events.clear();
        flat_events.clear();
    }
    return true;
}

void write_status_counts(std::ofstream& out, const std::uint64_t* counts) {
    out << "{";
    for (int st = 0; st < kStatusCount; ++st) {
        out << (st ? ", " : "") << '"' << to_string(static_cast<Status>(st)) << "\": " << counts[st];
    }
    out << "}";
}

void write_totals(std::ofstream& out, const Totals& t, const char* indent) {
    std::uint64_t by_status[kStatusCount];
    for (int st = 0; st < kStatusCount; ++st) by_status[st] = t.status_total(st);
    out << indent << "\"requests\": " << t.ops << ",\n"
        << indent << "\"trades_compared\": " << t.trades << ",\n"
        << indent << "\"level_updates_compared\": " << t.level_updates << ",\n"
        << indent << "\"rejected_requests\": " << t.rejected << ",\n"
        << indent << "\"requests_by_status\": ";
    write_status_counts(out, by_status);
    out << ",\n" << indent << "\"by_request_kind\": {";
    const char* kinds[kKindCount] = {"add", "cancel", "modify"};
    for (int k = 0; k < kKindCount; ++k) {
        out << (k ? ", " : "") << '"' << kinds[k] << "\": ";
        write_status_counts(out, t.by_kind_status[k]);
    }
    out << "},\n"
        << indent << "\"full_snapshot_comparisons\": " << t.snapshot_checks << ",\n"
        << indent << "\"max_resting_orders_seen\": " << t.max_resting_orders;
}

void usage() {
    std::fprintf(stderr,
                 "usage: lob_difftest [--ops N] [--seeds K] [--first-seed S] [--out file.json]\n"
                 "  N, K > 0; runs every scenario for seeds S .. S+K-1, N requests each\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t ops = 20'000;
    std::uint64_t seeds = 2;
    std::uint64_t first_seed = 1;
    const char* out_path = nullptr;
    for (int i = 1; i < argc; i += 2) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            usage();
            return 2;
        }
        const char* value = argv[i + 1];
        bool ok = true;
        if (std::strcmp(argv[i], "--ops") == 0)
            ok = cli::parse_u64(value, ops) && ops > 0;
        else if (std::strcmp(argv[i], "--seeds") == 0)
            ok = cli::parse_u64(value, seeds) && seeds > 0;
        else if (std::strcmp(argv[i], "--first-seed") == 0)
            ok = cli::parse_u64(value, first_seed);
        else if (std::strcmp(argv[i], "--out") == 0)
            out_path = value;
        else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            usage();
            return 2;
        }
        if (!ok) {
            std::fprintf(stderr, "bad value for %s: '%s'\n", argv[i], value);
            usage();
            return 2;
        }
    }

    // Open the output first, so a bad path fails before minutes of work.
    std::ofstream out;
    if (out_path != nullptr) {
        out.open(out_path);
        if (!out) {
            std::perror(out_path);
            return 2;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<Scenario> list = scenarios();
    Totals all;
    std::vector<Totals> per_scenario(list.size());
    for (std::size_t s = 0; s < list.size(); ++s) {
        for (std::uint64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
            if (!run(list[s], seed, ops, per_scenario[s])) {
                return 1;
            }
        }
    }
    for (const Totals& t : per_scenario) {
        all.ops += t.ops;
        all.trades += t.trades;
        all.level_updates += t.level_updates;
        all.rejected += t.rejected;
        all.snapshot_checks += t.snapshot_checks;
        if (t.max_resting_orders > all.max_resting_orders) all.max_resting_orders = t.max_resting_orders;
        for (int k = 0; k < kKindCount; ++k)
            for (int st = 0; st < kStatusCount; ++st) all.by_kind_status[k][st] += t.by_kind_status[k][st];
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf(
        "OK: %zu scenarios x %llu seeds, %llu requests, %llu trades, %llu level updates, "
        "%llu rejected requests, %llu snapshot comparisons, max %zu resting orders, %.1f s\n",
        list.size(), static_cast<unsigned long long>(seeds), static_cast<unsigned long long>(all.ops),
        static_cast<unsigned long long>(all.trades), static_cast<unsigned long long>(all.level_updates),
        static_cast<unsigned long long>(all.rejected), static_cast<unsigned long long>(all.snapshot_checks),
        all.max_resting_orders, seconds);

    if (out_path != nullptr) {
        out << "{\n"
            << "  \"result\": \"all books identical\",\n"
            << "  \"build\": " << build::json_object() << ",\n"
            << "  \"seeds_per_scenario\": " << seeds << ",\n"
            << "  \"first_seed\": " << first_seed << ",\n"
            << "  \"requests_per_run\": " << ops << ",\n"
            << "  \"total\": {\n";
        write_totals(out, all, "    ");
        out << "\n  },\n  \"scenarios\": {\n";
        for (std::size_t s = 0; s < list.size(); ++s) {
            out << "    \"" << list[s].name << "\": {\n";
            write_totals(out, per_scenario[s], "      ");
            out << "\n    }" << (s + 1 < list.size() ? ",\n" : "\n");
        }
        out << "  },\n  \"wall_seconds\": " << seconds << "\n}\n";
        out.close();
        if (!out) {
            std::perror(out_path);
            return 2;
        }
    }
    return 0;
}
