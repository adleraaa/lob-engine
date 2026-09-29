// Throughput and latency benchmark: MapOrderBook vs FlatOrderBook on three
// synthetic request mixes.
//
// Throughput: the whole pre-generated request stream is replayed on a fresh
// book `reps` times per book (alternating map/flat); we report the median
// requests/second and the min-max range.
// Latency: one more replay reads the CPU timestamp counter around every
// single request; we report percentiles of those per-request times. The
// numbers include the cost of reading the counter (reported separately as
// timer_overhead_ns) and the event listener callbacks.
//
// Usage: lob_bench [--ops N] [--reps R] [--cpu K] [--out DIR]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

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

#ifndef LOB_BUILD_FLAGS
#define LOB_BUILD_FLAGS "unknown"
#endif

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;

// ---- timing helpers -----------------------------------------------------------

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

std::string os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "other";
#endif
}

// Pins the calling thread to one logical CPU so the OS does not migrate it
// mid-run (on hybrid CPUs a move between a P-core and an E-core changes
// the speed noticeably). Returns false if pinning is unsupported or failed.
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
void apply(Book& book, const flow::Op& op) {
    switch (op.kind) {
        case flow::Op::Kind::Add: book.add(op.id, op.side, op.type, op.price, op.qty); break;
        case flow::Op::Kind::Cancel: book.cancel(op.id); break;
        case flow::Op::Kind::Modify: book.modify(op.id, op.price, op.qty); break;
    }
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
    std::uint64_t level_updates = 0;
    std::size_t final_resting_orders = 0;
};

template <typename Book>
typename Book::Config book_config(const flow::Config& cfg) {
    if constexpr (std::is_same_v<Book, FlatOrderBook>) {
        return {cfg.min_price, cfg.max_price};  // the tick range the generator stays in
    } else {
        return {};
    }
}

// Replays the whole stream once on a fresh book; returns requests/second and
// records the event counts in `r`.
template <typename Book>
double replay_rate(const std::vector<flow::Op>& ops, const flow::Config& cfg, Result& r) {
    CountingListener listener;
    Book book(listener, book_config<Book>(cfg));
    const auto start = Clock::now();
    for (const flow::Op& op : ops) {
        apply(book, op);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.trades = listener.trades;
    r.level_updates = listener.updates;
    r.final_resting_orders = book.order_count();
    return static_cast<double>(ops.size()) / seconds;
}

// Separate pass that times every request, so the timer reads do not distort
// the throughput numbers.
template <typename Book>
void measure_latency(const std::vector<flow::Op>& ops, const flow::Config& cfg, double ns_per_tick, Result& r) {
    std::vector<std::uint64_t> samples(ops.size());
    {
        CountingListener listener;
        Book book(listener, book_config<Book>(cfg));
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const std::uint64_t t0 = ticks();
            apply(book, ops[i]);
            samples[i] = ticks() - t0;
        }
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]) * ns_per_tick;
    };
    r.p50_ns = pct(0.50);
    r.p99_ns = pct(0.99);
    r.p999_ns = pct(0.999);
    r.max_ns = static_cast<double>(samples.back()) * ns_per_tick;
}

void summarize_rates(std::vector<double> rates, Result& r) {
    std::sort(rates.begin(), rates.end());
    r.median_ops_per_sec = rates[rates.size() / 2];
    r.min_ops_per_sec = rates.front();
    r.max_ops_per_sec = rates.back();
}

// Measures both books on one request stream. Throughput runs alternate
// map, flat, map, flat, ... so a slow period on a busy machine (turbo
// changes, background work) hits both books rather than just one of them.
// One untimed replay of each comes first to warm caches and clocks.
void measure_workload(const Workload& w, const std::vector<flow::Op>& ops, const flow::Config& cfg, int reps,
                      double ns_per_tick, std::vector<Result>& out) {
    Result map_r;
    Result flat_r;
    map_r.workload = flat_r.workload = w.name;
    map_r.book = "map";
    flat_r.book = "flat";
    map_r.ops = flat_r.ops = ops.size();

    replay_rate<MapOrderBook>(ops, cfg, map_r);
    replay_rate<FlatOrderBook>(ops, cfg, flat_r);
    std::vector<double> map_rates;
    std::vector<double> flat_rates;
    for (int rep = 0; rep < reps; ++rep) {
        map_rates.push_back(replay_rate<MapOrderBook>(ops, cfg, map_r));
        flat_rates.push_back(replay_rate<FlatOrderBook>(ops, cfg, flat_r));
    }
    summarize_rates(map_rates, map_r);
    summarize_rates(flat_rates, flat_r);
    measure_latency<MapOrderBook>(ops, cfg, ns_per_tick, map_r);
    measure_latency<FlatOrderBook>(ops, cfg, ns_per_tick, flat_r);
    out.push_back(map_r);
    out.push_back(flat_r);
}

// Fractions of each request kind actually generated. They differ from the
// requested mix because the generator forces a cancel whenever it already
// tracks max_live_ids possibly-resting orders, and because a cancel or modify
// is only possible once some order has been added.
std::string mix_json(const std::vector<flow::Op>& ops) {
    double limit = 0, market = 0, ioc = 0, fok = 0, cancel = 0, modify = 0;
    for (const flow::Op& op : ops) {
        if (op.kind == flow::Op::Kind::Cancel) {
            ++cancel;
        } else if (op.kind == flow::Op::Kind::Modify) {
            ++modify;
        } else if (op.type == OrderType::Limit) {
            ++limit;
        } else if (op.type == OrderType::Market) {
            ++market;
        } else if (op.type == OrderType::IOC) {
            ++ioc;
        } else {
            ++fok;
        }
    }
    const double n = static_cast<double>(ops.size());
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"limit\": %.4f, \"market\": %.4f, \"ioc\": %.4f, \"fok\": %.4f, \"cancel\": %.4f, "
                  "\"modify\": %.4f}",
                  limit / n, market / n, ioc / n, fok / n, cancel / n, modify / n);
    return buf;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t op_count = 2'000'000;
    int reps = 5;
    int cpu = -1;
    std::string out_dir;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--ops") == 0)
            op_count = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--reps") == 0)
            reps = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--cpu") == 0)
            cpu = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--out") == 0)
            out_dir = argv[i + 1];
        else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (op_count == 0 || reps <= 0) {
        std::fprintf(stderr, "--ops and --reps must be positive\n");
        return 2;
    }

    const bool pinned = cpu >= 0 && pin_to_cpu(cpu);
    const double ns_per_tick = calibrate_ns_per_tick();
    const double overhead_ns = static_cast<double>(timer_overhead_ticks()) * ns_per_tick;

    std::vector<Result> results;
    std::vector<std::string> actual_mixes;
    for (const Workload& w : workloads()) {
        flow::Config cfg;
        cfg.seed = 2026;
        cfg.mix = w.mix;
        const std::vector<flow::Op> ops = flow::Generator(cfg).generate(op_count);
        actual_mixes.push_back(mix_json(ops));
        measure_workload(w, ops, cfg, reps, ns_per_tick, results);
        for (std::size_t k = results.size() - 2; k < results.size(); ++k) {
            const Result& r = results[k];
            std::printf(
                "%-12s %-5s %6.2f M req/s  p50 %6.1f ns  p99 %7.1f ns  p99.9 %8.1f ns  trades %llu  resting %zu\n",
                r.workload.c_str(), r.book.c_str(), r.median_ops_per_sec / 1e6, r.p50_ns, r.p99_ns, r.p999_ns,
                static_cast<unsigned long long>(r.trades), r.final_resting_orders);
        }
    }

    const std::string cpu_str = cpu_name();
#if defined(__clang__)
    const std::string compiler = __VERSION__;  // already starts with "Clang"
#else
    const std::string compiler = std::string("GCC ") + __VERSION__;
#endif
    std::printf("cpu: %s | compiler: %s | flags: %s | pinned: %s | timer overhead: %.1f ns\n", cpu_str.c_str(),
                compiler.c_str(), LOB_BUILD_FLAGS, pinned ? "yes" : "no", overhead_ns);

    if (out_dir.empty()) {
        return 0;
    }

    std::ofstream csv(out_dir + "/bench.csv");
    csv << "workload,book,requests,median_req_per_s,min_req_per_s,max_req_per_s,p50_ns,p99_ns,p999_ns,max_ns,"
           "trades,level_updates,final_resting_orders\n";
    for (const Result& r : results) {
        csv << r.workload << ',' << r.book << ',' << r.ops << ',' << static_cast<std::uint64_t>(r.median_ops_per_sec)
            << ',' << static_cast<std::uint64_t>(r.min_ops_per_sec) << ','
            << static_cast<std::uint64_t>(r.max_ops_per_sec) << ',' << r.p50_ns << ',' << r.p99_ns << ',' << r.p999_ns
            << ',' << r.max_ns << ',' << r.trades << ',' << r.level_updates << ',' << r.final_resting_orders << '\n';
    }

    std::ofstream json(out_dir + "/bench.json");
    json << "{\n  \"environment\": {\n"
         << "    \"cpu\": \"" << json_escape(cpu_str) << "\",\n"
         << "    \"os\": \"" << os_name() << "\",\n"
         << "    \"compiler\": \"" << json_escape(compiler) << "\",\n"
         << "    \"flags\": \"" << json_escape(LOB_BUILD_FLAGS) << "\",\n"
         << "    \"pinned_cpu\": " << (pinned ? cpu : -1) << ",\n"
         << "    \"timer_overhead_ns\": " << overhead_ns << ",\n"
         << "    \"requests_per_run\": " << op_count << ",\n"
         << "    \"throughput_reps\": " << reps << ",\n"
         << "    \"generator_seed\": 2026\n  },\n  \"workloads\": {\n";
    const std::vector<Workload> ws = workloads();
    for (std::size_t i = 0; i < ws.size(); ++i) {
        json << "    \"" << ws[i].name << "\": {\"description\": \"" << json_escape(ws[i].description)
             << "\", \"actual_fractions\": " << actual_mixes[i] << "}" << (i + 1 < ws.size() ? ",\n" : "\n");
    }
    json << "  },\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        json << "    {\"workload\": \"" << r.workload << "\", \"book\": \"" << r.book << "\", \"requests\": " << r.ops
             << ", \"median_req_per_s\": " << static_cast<std::uint64_t>(r.median_ops_per_sec)
             << ", \"min_req_per_s\": " << static_cast<std::uint64_t>(r.min_ops_per_sec)
             << ", \"max_req_per_s\": " << static_cast<std::uint64_t>(r.max_ops_per_sec) << ", \"p50_ns\": " << r.p50_ns
             << ", \"p99_ns\": " << r.p99_ns << ", \"p999_ns\": " << r.p999_ns << ", \"max_ns\": " << r.max_ns
             << ", \"trades\": " << r.trades << ", \"level_updates\": " << r.level_updates
             << ", \"final_resting_orders\": " << r.final_resting_orders << "}"
             << (i + 1 < results.size() ? ",\n" : "\n");
    }
    json << "  ]\n}\n";

    std::ofstream md(out_dir + "/bench.md");
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
    std::snprintf(line, sizeof line,
                  "\n%zu requests per run, throughput = median of %d runs, latency = one run timed per request "
                  "(includes ~%.0f ns timer overhead).\n",
                  op_count, reps, overhead_ns);
    md << line << "CPU: " << cpu_str << " (" << os_name() << ", thread pinned: " << (pinned ? "yes" : "no")
       << "). Compiler: " << compiler << ". Flags: " << LOB_BUILD_FLAGS << ".\n";
    return 0;
}
