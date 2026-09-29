// Throughput and latency benchmark: MapOrderBook vs FlatOrderBook on three
// synthetic request mixes.
//
// Stream: the generator remembers every Limit id until it cancels it, and it
// does not see fills, so on its own it emits many cancels/modifies of orders
// that have already traded away. The book rejects those with UnknownId after
// one hash lookup; left in, they would make the "cancel" share of a mix
// mostly no-ops and inflate the request rate. So the stream is built by
// running the generator through an untimed MapOrderBook and keeping only
// requests that did not come back UnknownId. Dropping them does not change
// what the kept requests do, because an UnknownId request never changes the
// book. Every book configuration is deterministic, so the timed replays see
// exactly the same results as this filter pass.
//
// Throughput: the whole stream is replayed on a fresh, pre-sized book `reps`
// times per book (alternating map/flat); we report the median requests/second
// and the min-max range.
// Latency: `lat_reps` more replays per book read the CPU timestamp counter
// around every single request; each percentile is the median over those
// replays. The numbers include the cost of reading the counter (reported as
// timer_overhead_ns) and the event listener callbacks.
//
// Usage: lob_bench [--ops N] [--reps R] [--lat-reps L] [--cpu K] [--out DIR]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "build_info.hpp"
#include "cli.hpp"
#include "lob/order_book.hpp"
#include "order_flow.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <x86intrin.h>
#define LOB_HAVE_TSC 1
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSeed = 2026;

// ---- timing helpers -----------------------------------------------------------

// rdtsc is not a serializing instruction: the CPU may start it slightly
// before earlier instructions finish. For per-request times of tens of ns
// that blurs individual samples by a few ns, which is acceptable for a
// map-vs-flat comparison; a serialized read (rdtscp + lfence) would cost more.
std::uint64_t ticks() {
#ifdef LOB_HAVE_TSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per tick, measured against steady_clock over ~200 ms.
double calibrate_ns_per_tick() {
    const auto t0 = Clock::now();
    const std::uint64_t c0 = ticks();
    while (Clock::now() - t0 < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t c1 = ticks();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    return ns / static_cast<double>(c1 - c0);
}

// Median cost of two back-to-back ticks() calls, in ticks.
std::uint64_t timer_overhead_ticks() {
    std::vector<std::uint64_t> samples(100'000);
    for (auto& s : samples) {
        const std::uint64_t a = ticks();
        const std::uint64_t b = ticks();
        s = b - a;
    }
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}

std::string cpu_name() {
#ifdef LOB_HAVE_TSC
    unsigned int regs[12] = {};
    if (__get_cpuid(0x80000002, &regs[0], &regs[1], &regs[2], &regs[3]) &&
        __get_cpuid(0x80000003, &regs[4], &regs[5], &regs[6], &regs[7]) &&
        __get_cpuid(0x80000004, &regs[8], &regs[9], &regs[10], &regs[11])) {
        char name[49] = {};
        std::memcpy(name, regs, 48);
        std::string s(name);
        const auto first = s.find_first_not_of(' ');
        const auto last = s.find_last_not_of(' ');
        return first == std::string::npos ? "unknown" : s.substr(first, last - first + 1);
    }
#endif
    return "unknown";
}

// Pins the calling thread to one logical CPU (0..63) so the OS does not
// migrate it mid-run (on hybrid CPUs a move between a P-core and an E-core
// changes the speed noticeably). Returns false if pinning is unsupported or
// failed. The caller has already checked 0 <= cpu < 64, which keeps the
// Windows mask shift defined.
bool pin_to_cpu(int cpu) {
#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

// ---- workload -------------------------------------------------------------------

// Counts events so the compiler cannot drop the work, and so we can report
// how many trades each workload produced. Roughly the minimum a real
// consumer (market-data publisher) would do per event.
class CountingListener final : public EventListener {
public:
    void on_trade(const Trade& t) override {
        ++trades;
        traded_qty += static_cast<std::uint64_t>(t.qty);
    }
    void on_level_update(const LevelUpdate&) override { ++updates; }
    std::uint64_t trades = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t updates = 0;
};

struct Workload {
    std::string name;
    std::string description;
    flow::Mix mix;
};

std::vector<Workload> workloads() {
    std::vector<Workload> list;
    flow::Mix add_cancel;
    add_cancel.cancel = 0.45;
    add_cancel.modify = 0;
    add_cancel.market = 0;
    add_cancel.ioc = 0;
    add_cancel.fok = 0;
    add_cancel.cross = 0;
    list.push_back({"add_cancel", "passive-priced limit adds and cancels only (a few still trade when the mid drifts)",
                    add_cancel});
    list.push_back({"balanced", "mostly passive adds and cancels; 15% of limit adds cross; some modify/market/IOC/FOK",
                    flow::Mix{}});
    flow::Mix match_heavy;
    match_heavy.cancel = 0.20;
    match_heavy.modify = 0.05;
    match_heavy.market = 0.10;
    match_heavy.ioc = 0.10;
    match_heavy.fok = 0.05;
    match_heavy.cross = 0.40;
    list.push_back({"match_heavy", "aggressive flow: 40% of limit adds cross; more market/IOC/FOK", match_heavy});
    return list;
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

constexpr int kStatusCount = static_cast<int>(Status::UnknownId) + 1;

// What the stream-building pass saw.
struct StreamStats {
    std::uint64_t generated = 0;                 // requests the generator produced
    std::uint64_t dropped_unknown = 0;           // of those, UnknownId no-ops left out
    std::uint64_t by_status[kStatusCount] = {};  // statuses of the kept requests
    std::uint64_t limit = 0, market = 0, ioc = 0, fok = 0, cancel = 0, modify = 0;
    std::uint64_t fok_filled = 0;
    std::uint64_t fok_killed = 0;
};

// Both books get the generator's tick range (the map book accepts it too),
// so both validate prices by the same rule.
MapOrderBook::Config map_config(const flow::Config& cfg) { return {cfg.min_price, cfg.max_price}; }
FlatOrderBook::Config flat_config(const flow::Config& cfg) { return {cfg.min_price, cfg.max_price}; }

// Builds a stream of `count` requests with the UnknownId no-ops removed (see
// the comment at the top of the file) and fills in `stats`.
std::vector<flow::Op> build_stream(const flow::Config& cfg, std::size_t count, StreamStats& stats) {
    flow::Generator gen(cfg);
    NullListener sink;
    MapOrderBook filter(sink, map_config(cfg));
    std::vector<flow::Op> ops;
    ops.reserve(count);
    while (ops.size() < count) {
        const flow::Op op = gen.next();
        ++stats.generated;
        const ExecReport r = apply(filter, op);
        if (r.status == Status::UnknownId) {
            ++stats.dropped_unknown;
            continue;
        }
        ops.push_back(op);
        ++stats.by_status[static_cast<int>(r.status)];
        if (op.kind == flow::Op::Kind::Cancel) {
            ++stats.cancel;
        } else if (op.kind == flow::Op::Kind::Modify) {
            ++stats.modify;
        } else if (op.type == OrderType::Limit) {
            ++stats.limit;
        } else if (op.type == OrderType::Market) {
            ++stats.market;
        } else if (op.type == OrderType::IOC) {
            ++stats.ioc;
        } else {
            ++(r.filled > 0 ? stats.fok_filled : stats.fok_killed);
            ++stats.fok;
        }
    }
    return ops;
}

struct Result {
    std::string workload;
    std::string book;
    std::size_t ops = 0;
    double median_ops_per_sec = 0;
    double min_ops_per_sec = 0;
    double max_ops_per_sec = 0;
    double p50_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double max_ns = 0;
    std::uint64_t trades = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t level_updates = 0;
    std::size_t final_resting_orders = 0;
};

// A fresh book with its pool and id map already sized for the most orders
// the generator can have resting, so no allocation of those two lands in a
// timed region. (A map book still allocates a tree node per new price level;
// that is part of what is being compared.)
template <typename Book>
struct PreparedBook {
    CountingListener listener;
    Book book;
    PreparedBook(const typename Book::Config& config, std::size_t max_orders) : book(listener, config) {
        book.reserve(max_orders);
    }
};

// Replays the whole stream once on a fresh book; returns requests/second and
// records the event counts in `r`.
template <typename Book>
double replay_rate(const std::vector<flow::Op>& ops, const typename Book::Config& config, std::size_t max_orders,
                   Result& r) {
    PreparedBook<Book> b(config, max_orders);
    const auto start = Clock::now();
    for (const flow::Op& op : ops) {
        apply(b.book, op);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.trades = b.listener.trades;
    r.traded_qty = b.listener.traded_qty;
    r.level_updates = b.listener.updates;
    r.final_resting_orders = b.book.order_count();
    return static_cast<double>(ops.size()) / seconds;
}

struct Percentiles {
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

// One replay that times every request separately (a separate pass, so the
// timer reads do not distort the throughput numbers).
template <typename Book>
Percentiles latency_once(const std::vector<flow::Op>& ops, const typename Book::Config& config, std::size_t max_orders,
                         double ns_per_tick, std::vector<std::uint64_t>& samples) {
    samples.resize(ops.size());
    {
        PreparedBook<Book> b(config, max_orders);
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const std::uint64_t t0 = ticks();
            apply(b.book, ops[i]);
            samples[i] = ticks() - t0;
        }
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]) * ns_per_tick;
    };
    return {pct(0.50), pct(0.99), pct(0.999), static_cast<double>(samples.back()) * ns_per_tick};
}

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void summarize_rates(std::vector<double> rates, Result& r) {
    std::sort(rates.begin(), rates.end());
    r.median_ops_per_sec = rates[rates.size() / 2];
    r.min_ops_per_sec = rates.front();
    r.max_ops_per_sec = rates.back();
}

// Median of each percentile across the latency runs; max is the overall max.
void summarize_latency(const std::vector<Percentiles>& runs, Result& r) {
    std::vector<double> p50, p99, p999;
    for (const Percentiles& p : runs) {
        p50.push_back(p.p50);
        p99.push_back(p.p99);
        p999.push_back(p.p999);
        r.max_ns = std::max(r.max_ns, p.max);
    }
    r.p50_ns = median_of(p50);
    r.p99_ns = median_of(p99);
    r.p999_ns = median_of(p999);
}

// Measures both books on one request stream. Throughput runs alternate
// map, flat, map, flat, ... so a slow period (turbo changes, background
// work) hits both books rather than just one of them. One untimed replay of
// each comes first to warm caches and clocks.
void measure_workload(const Workload& w, const std::vector<flow::Op>& ops, const flow::Config& cfg, int reps,
                      int lat_reps, double ns_per_tick, std::vector<Result>& out) {
    const std::size_t max_orders = cfg.max_live_ids;
    Result map_r;
    Result flat_r;
    map_r.workload = flat_r.workload = w.name;
    map_r.book = "map";
    flat_r.book = "flat";
    map_r.ops = flat_r.ops = ops.size();

    replay_rate<MapOrderBook>(ops, map_config(cfg), max_orders, map_r);
    replay_rate<FlatOrderBook>(ops, flat_config(cfg), max_orders, flat_r);
    std::vector<double> map_rates;
    std::vector<double> flat_rates;
    for (int rep = 0; rep < reps; ++rep) {
        map_rates.push_back(replay_rate<MapOrderBook>(ops, map_config(cfg), max_orders, map_r));
        flat_rates.push_back(replay_rate<FlatOrderBook>(ops, flat_config(cfg), max_orders, flat_r));
    }
    summarize_rates(map_rates, map_r);
    summarize_rates(flat_rates, flat_r);

    std::vector<std::uint64_t> samples;
    std::vector<Percentiles> map_lat;
    std::vector<Percentiles> flat_lat;
    for (int rep = 0; rep < lat_reps; ++rep) {
        map_lat.push_back(latency_once<MapOrderBook>(ops, map_config(cfg), max_orders, ns_per_tick, samples));
        flat_lat.push_back(latency_once<FlatOrderBook>(ops, flat_config(cfg), max_orders, ns_per_tick, samples));
    }
    summarize_latency(map_lat, map_r);
    summarize_latency(flat_lat, flat_r);
    out.push_back(map_r);
    out.push_back(flat_r);
}

std::string stream_json(const StreamStats& s) {
    const double n = static_cast<double>(s.generated - s.dropped_unknown);
    std::string status = "{";
    for (int st = 0; st < kStatusCount; ++st) {
        status += (st ? ", \"" : "\"") + std::string(to_string(static_cast<Status>(st))) +
                  "\": " + std::to_string(s.by_status[st]);
    }
    status += "}";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "\"generated_requests\": %llu, \"dropped_unknown_id\": %llu, "
                  "\"actual_fractions\": {\"limit\": %.4f, \"market\": %.4f, \"ioc\": %.4f, \"fok\": %.4f, "
                  "\"cancel\": %.4f, \"modify\": %.4f}, \"fok_filled\": %llu, \"fok_killed\": %llu, "
                  "\"requests_by_status\": ",
                  static_cast<unsigned long long>(s.generated), static_cast<unsigned long long>(s.dropped_unknown),
                  static_cast<double>(s.limit) / n, static_cast<double>(s.market) / n, static_cast<double>(s.ioc) / n,
                  static_cast<double>(s.fok) / n, static_cast<double>(s.cancel) / n, static_cast<double>(s.modify) / n,
                  static_cast<unsigned long long>(s.fok_filled), static_cast<unsigned long long>(s.fok_killed));
    return buf + status;
}

bool open_output(std::ofstream& file, const std::string& path) {
    file.open(path);
    if (!file) {
        std::perror(path.c_str());
        return false;
    }
    return true;
}

void usage() {
    std::fprintf(stderr,
                 "usage: lob_bench [--ops N] [--reps R] [--lat-reps L] [--cpu K] [--out DIR]\n"
                 "  N, R, L > 0; K in 0..63 pins the thread to that logical CPU\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t op_count = 2'000'000;
    std::uint64_t reps = 5;
    std::uint64_t lat_reps = 5;
    std::uint64_t cpu = 0;
    bool want_pin = false;
    std::string out_dir;
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
        if (std::strcmp(argv[i], "--ops") == 0) {
            ok = cli::parse_u64(value, op_count) && op_count > 0;
        } else if (std::strcmp(argv[i], "--reps") == 0) {
            ok = cli::parse_u64(value, reps) && reps > 0 && reps <= 1000;
        } else if (std::strcmp(argv[i], "--lat-reps") == 0) {
            ok = cli::parse_u64(value, lat_reps) && lat_reps > 0 && lat_reps <= 1000;
        } else if (std::strcmp(argv[i], "--cpu") == 0) {
            ok = cli::parse_u64(value, cpu) && cpu < 64;
            want_pin = true;
        } else if (std::strcmp(argv[i], "--out") == 0) {
            out_dir = value;
        } else {
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

    // Open the outputs first, so a bad directory fails before the long run.
    std::ofstream csv, json, md;
    if (!out_dir.empty()) {
        if (!open_output(csv, out_dir + "/bench.csv") || !open_output(json, out_dir + "/bench.json") ||
            !open_output(md, out_dir + "/bench.md")) {
            return 2;
        }
    }

    const bool pinned = want_pin && pin_to_cpu(static_cast<int>(cpu));
    if (want_pin && !pinned) {
        std::fprintf(stderr, "warning: could not pin to CPU %llu; running unpinned\n",
                     static_cast<unsigned long long>(cpu));
    }
    const double ns_per_tick = calibrate_ns_per_tick();
    const double overhead_ns = static_cast<double>(timer_overhead_ticks()) * ns_per_tick;

    std::vector<Result> results;
    std::vector<StreamStats> stats;
    for (const Workload& w : workloads()) {
        flow::Config cfg;
        cfg.seed = kSeed;
        cfg.mix = w.mix;
        stats.emplace_back();
        const std::vector<flow::Op> ops = build_stream(cfg, op_count, stats.back());
        measure_workload(w, ops, cfg, static_cast<int>(reps), static_cast<int>(lat_reps), ns_per_tick, results);
        for (std::size_t k = results.size() - 2; k < results.size(); ++k) {
            const Result& r = results[k];
            std::printf(
                "%-12s %-5s %6.2f M req/s  p50 %6.1f ns  p99 %7.1f ns  p99.9 %8.1f ns  trades %llu  resting %zu\n",
                r.workload.c_str(), r.book.c_str(), r.median_ops_per_sec / 1e6, r.p50_ns, r.p99_ns, r.p999_ns,
                static_cast<unsigned long long>(r.trades), r.final_resting_orders);
        }
    }

    const std::string cpu_str = cpu_name();
    std::printf("cpu: %s | %s | pinned: %s | timer overhead: %.1f ns\n", cpu_str.c_str(), build::json_object().c_str(),
                pinned ? "yes" : "no", overhead_ns);

    if (out_dir.empty()) {
        return 0;
    }

    csv << "workload,book,requests,median_req_per_s,min_req_per_s,max_req_per_s,p50_ns,p99_ns,p999_ns,max_ns,"
           "trades,traded_qty,level_updates,final_resting_orders\n";
    for (const Result& r : results) {
        csv << r.workload << ',' << r.book << ',' << r.ops << ',' << static_cast<std::uint64_t>(r.median_ops_per_sec)
            << ',' << static_cast<std::uint64_t>(r.min_ops_per_sec) << ','
            << static_cast<std::uint64_t>(r.max_ops_per_sec) << ',' << r.p50_ns << ',' << r.p99_ns << ',' << r.p999_ns
            << ',' << r.max_ns << ',' << r.trades << ',' << r.traded_qty << ',' << r.level_updates << ','
            << r.final_resting_orders << '\n';
    }

    json << "{\n  \"build\": " << build::json_object() << ",\n  \"environment\": {\n"
         << "    \"cpu\": \"" << build::json_escape(cpu_str) << "\",\n"
         << "    \"pinned_cpu\": " << (pinned ? static_cast<long long>(cpu) : -1LL) << ",\n"
         << "    \"timer_overhead_ns\": " << overhead_ns << ",\n"
         << "    \"requests_per_run\": " << op_count << ",\n"
         << "    \"throughput_reps\": " << reps << ",\n"
         << "    \"latency_reps\": " << lat_reps << ",\n"
         << "    \"generator_seed\": " << kSeed << "\n  },\n  \"workloads\": {\n";
    const std::vector<Workload> ws = workloads();
    for (std::size_t i = 0; i < ws.size(); ++i) {
        json << "    \"" << ws[i].name << "\": {\"description\": \"" << build::json_escape(ws[i].description) << "\", "
             << stream_json(stats[i]) << "}" << (i + 1 < ws.size() ? ",\n" : "\n");
    }
    json << "  },\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        json << "    {\"workload\": \"" << r.workload << "\", \"book\": \"" << r.book << "\", \"requests\": " << r.ops
             << ", \"median_req_per_s\": " << static_cast<std::uint64_t>(r.median_ops_per_sec)
             << ", \"min_req_per_s\": " << static_cast<std::uint64_t>(r.min_ops_per_sec)
             << ", \"max_req_per_s\": " << static_cast<std::uint64_t>(r.max_ops_per_sec) << ", \"p50_ns\": " << r.p50_ns
             << ", \"p99_ns\": " << r.p99_ns << ", \"p999_ns\": " << r.p999_ns << ", \"max_ns\": " << r.max_ns
             << ", \"trades\": " << r.trades << ", \"traded_qty\": " << r.traded_qty
             << ", \"level_updates\": " << r.level_updates << ", \"final_resting_orders\": " << r.final_resting_orders
             << "}" << (i + 1 < results.size() ? ",\n" : "\n");
    }
    json << "  ]\n}\n";

    md << "| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns | trades |\n"
       << "|---|---|---|---|---|---|---|\n";
    char line[256];
    for (const Result& r : results) {
        std::snprintf(line, sizeof line, "| %s | %s | %.2f (%.2f-%.2f) | %.0f | %.0f | %.0f | %llu |\n",
                      r.workload.c_str(), r.book.c_str(), r.median_ops_per_sec / 1e6, r.min_ops_per_sec / 1e6,
                      r.max_ops_per_sec / 1e6, r.p50_ns, r.p99_ns, r.p999_ns,
                      static_cast<unsigned long long>(r.trades));
        md << line;
    }
    md << "\n| workload | generated | dropped (UnknownId) | limit | cancel | modify | market | IOC | FOK filled / "
          "killed "
          "| other rejects |\n|---|---|---|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < ws.size(); ++i) {
        const StreamStats& s = stats[i];
        const double n = static_cast<double>(op_count);
        std::uint64_t other_rejects = 0;
        for (int st = 1; st < kStatusCount; ++st) other_rejects += s.by_status[st];
        std::snprintf(
            line, sizeof line, "| %s | %llu | %llu | %.3f | %.3f | %.3f | %.3f | %.3f | %llu / %llu | %llu |\n",
            ws[i].name.c_str(), static_cast<unsigned long long>(s.generated),
            static_cast<unsigned long long>(s.dropped_unknown), static_cast<double>(s.limit) / n,
            static_cast<double>(s.cancel) / n, static_cast<double>(s.modify) / n, static_cast<double>(s.market) / n,
            static_cast<double>(s.ioc) / n, static_cast<unsigned long long>(s.fok_filled),
            static_cast<unsigned long long>(s.fok_killed), static_cast<unsigned long long>(other_rejects));
        md << line;
    }
    std::snprintf(line, sizeof line,
                  "\n%llu requests per run, throughput = median of %llu runs, latency = median over %llu runs "
                  "timed per request (includes ~%.0f ns timer overhead).\n",
                  static_cast<unsigned long long>(op_count), static_cast<unsigned long long>(reps),
                  static_cast<unsigned long long>(lat_reps), overhead_ns);
    md << line << "CPU: " << cpu_str << " (thread pinned: " << (pinned ? "yes" : "no")
       << "). Build: " << build::json_object() << ".\n";

    for (std::ofstream* f : {&csv, &json, &md}) {
        f->close();
        if (!*f) {
            std::fprintf(stderr, "error writing results to %s\n", out_dir.c_str());
            return 2;
        }
    }
    return 0;
}
