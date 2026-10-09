#include "estimate.hpp"

#include "human.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <ranges>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

namespace egraph {

namespace {

std::string cp_of(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    return std::format("{}/{}", parts.category, parts.name);
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values.at(middle)
                                  : (values.at(middle - 1) + values.at(middle)) / 2;
}

// Whether it got less of the system than it asked for: beside others on a saturated system, or
// beside others on one that said nothing of itself.
bool starved(const BuildRecord& record) {
    if (record.beside == 0) {
        return false;
    }
    if (record.system && record.system->cpu_busy) {
        return *record.system->cpu_busy >= saturated_share;
    }
    return true;
}

// Its test phase's seconds; none without one.
double test_phase_seconds(const BuildRecord& record) {
    double seconds = 0;
    for (const auto& phase : record.phases) {
        if (phase.phase == "test") {
            seconds += phase.seconds;
        }
    }
    return seconds;
}

// The flags one has that the other lacks, either way.
std::size_t use_distance(const std::vector<std::string>& built,
                         const std::vector<std::string>& wanted) {
    std::vector<std::string_view> differing;
    std::ranges::set_symmetric_difference(built, wanted, std::back_inserter(differing));
    return differing.size();
}

// How much the wanted distfiles scale a build's time, where both sizes are known.
double distfile_scale(const BuildRecord& record, std::optional<std::uint64_t> wanted) {
    if (!wanted || *wanted == 0 || !record.distfiles_size || *record.distfiles_size == 0) {
        return 1;
    }
    return std::clamp(static_cast<double>(*wanted) / static_cast<double>(*record.distfiles_size),
                      1 / distfile_scale_limit, distfile_scale_limit);
}

// The least-squares slope of ys over xs, none when xs do not vary.
std::optional<double> slope(const std::vector<double>& xs, const std::vector<double>& ys) {
    const auto n = static_cast<double>(xs.size());
    const auto mean_x = std::ranges::fold_left(xs, 0.0, std::plus<>{}) / n;
    const auto mean_y = std::ranges::fold_left(ys, 0.0, std::plus<>{}) / n;
    double covariance = 0;
    double variance = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        covariance += (xs.at(i) - mean_x) * (ys.at(i) - mean_y);
        variance += (xs.at(i) - mean_x) * (xs.at(i) - mean_x);
    }
    if (variance <= 0) {
        return std::nullopt;
    }
    return covariance / variance;
}

// Weighted geometric mean, each value held to at least floor so that one of none counts.
double geometric_mean(const std::vector<std::pair<double, double>>& weighted, double floor) {
    double logs = 0;
    double weights = 0;
    for (const auto& [value, weight] : weighted) {
        logs += weight * std::log(std::max(value, floor));
        weights += weight;
    }
    return std::exp(logs / weights);
}

} // namespace

BuildKnowledge::BuildKnowledge(const std::vector<BuildRecord>& records) {
    for (const auto& record : records) {
        if (record.outcome == BuildRecord::Outcome::merged) {
            auto& found = by_cp_[cp_of(record.cpv)];
            found.push_back(record);
        }
    }
    // Each package's latest build, sized, as a point of log seconds over log bytes.
    std::vector<double> sizes;
    std::vector<double> times;
    for (const auto& [cp, builds] : by_cp_) {
        const auto& latest = builds.back();
        if (latest.distfiles_size.value_or(0) > 0 && seconds_building(latest) > 0) {
            sizes.push_back(std::log(static_cast<double>(latest.distfiles_size.value_or(0))));
            times.push_back(std::log(seconds_building(latest)));
        }
    }
    if (sizes.size() >= guess_fit_packages) {
        if (const auto fitted = slope(sizes, times)) {
            size_exponent_ = std::clamp(*fitted, 0.0, 1.0);
        }
    }
}

void BuildKnowledge::learn(const RepositoryIndex& index) {
    std::unordered_map<std::string_view, std::size_t> inheriting;
    // By cp built here, the version of its latest build, else its last version.
    std::map<std::string_view, std::reference_wrapper<const IndexVersion>> exact;
    std::map<std::string_view, std::reference_wrapper<const IndexVersion>> last;
    for (const auto& version : index.versions) {
        for (const auto id : index.ids_in(version.inherited)) {
            ++inheriting[index.string(id)];
        }
        const auto cp = index.string(version.cp);
        const auto found = by_cp_.find(cp);
        if (found == by_cp_.end()) {
            continue;
        }
        last.insert_or_assign(cp, version);
        const auto& latest = found->second.back();
        if (index.string(version.cpv) == latest.cpv &&
            (latest.repo.empty() ||
             index.string(index.repositories.at(version.repository).name) == latest.repo)) {
            exact.insert_or_assign(cp, version);
        }
    }
    // The rarer an eclass among the versions, the more sharing it says; one in every version
    // says nothing.
    const auto versions = static_cast<double>(std::max<std::size_t>(index.versions.size(), 1));
    rarity_.clear();
    for (const auto& [eclass, count] : inheriting) {
        rarity_.emplace(eclass, std::log(versions / static_cast<double>(count)));
    }
    unseen_rarity_ = std::log(versions);
    eclasses_.clear();
    for (const auto& [cp, version] : last) {
        const auto chosen = exact.find(cp);
        const IndexVersion& from = chosen != exact.end() ? chosen->second.get() : version.get();
        std::vector<std::string> eclasses;
        for (const auto id : index.ids_in(from.inherited)) {
            eclasses.emplace_back(index.string(id));
        }
        std::ranges::sort(eclasses);
        eclasses_.emplace(cp, std::move(eclasses));
    }
}

std::optional<PackageEstimate> BuildKnowledge::guess(const BuildWanted& wanted,
                                                     unsigned cores) const {
    if (!wanted.eclasses || eclasses_.empty()) {
        return std::nullopt;
    }
    const auto weight = [&](std::string_view eclass) {
        const auto found = rarity_.find(eclass);
        return found == rarity_.end() ? unseen_rarity_ : found->second;
    };
    const auto sum = [&](const std::vector<std::string_view>& eclasses) {
        return std::ranges::fold_left(eclasses | std::views::transform(weight), 0.0, std::plus<>{});
    };
    // Shared eclasses' weight over all of both's; alike when both inherit nothing, or differ
    // only in eclasses that say nothing.
    const auto similarity = [&](const std::vector<std::string>& eclasses) {
        if (wanted.eclasses->empty() || eclasses.empty()) {
            return wanted.eclasses->empty() && eclasses.empty() ? 1.0 : 0.0;
        }
        std::vector<std::string_view> shared;
        std::vector<std::string_view> either;
        std::ranges::set_intersection(*wanted.eclasses, eclasses, std::back_inserter(shared));
        std::ranges::set_union(*wanted.eclasses, eclasses, std::back_inserter(either));
        const auto all = sum(either);
        return all > 0 ? sum(shared) / all : (shared.empty() ? 0.0 : 1.0);
    };
    // How far apart two sizes are, for ties: none known counts as far.
    const auto apart = [&](const BuildRecord& latest) {
        if (wanted.distfiles.value_or(0) == 0 || latest.distfiles_size.value_or(0) == 0) {
            return std::numeric_limits<double>::infinity();
        }
        return std::abs(std::log(static_cast<double>(*wanted.distfiles) /
                                 static_cast<double>(*latest.distfiles_size)));
    };
    struct Alike {
        double similarity;
        double apart;
        std::reference_wrapper<const BuildRecord> latest;
    };
    std::vector<Alike> alike;
    for (const auto& [cp, eclasses] : eclasses_) {
        if (const auto found = similarity(eclasses); found > 0) {
            const auto& latest = by_cp_.find(cp)->second.back();
            alike.push_back({.similarity = found, .apart = apart(latest), .latest = latest});
        }
    }
    if (alike.empty()) {
        return std::nullopt;
    }
    std::ranges::stable_sort(alike, [](const Alike& a, const Alike& b) {
        return a.similarity != b.similarity ? a.similarity > b.similarity : a.apart < b.apart;
    });
    alike.resize(std::min(alike.size(), guess_neighbours), alike.front());
    std::vector<std::pair<double, double>> seconds;
    std::vector<std::pair<double, double>> peaks;
    double parallelism = 0;
    double merge = 0;
    double tests = 0;
    double weights = 0;
    for (const auto& [similar, distance, latest] : alike) {
        const auto own = estimate(latest.get().cpv, cores).value_or(PackageEstimate{});
        double scale = 1;
        if (std::isfinite(distance)) {
            scale = std::clamp(
                std::pow(static_cast<double>(wanted.distfiles.value_or(1)) /
                             static_cast<double>(latest.get().distfiles_size.value_or(1)),
                         size_exponent_),
                1 / guess_scale_limit, guess_scale_limit);
        }
        seconds.emplace_back(own.seconds * scale, similar);
        if (own.peak_memory) {
            peaks.emplace_back(static_cast<double>(*own.peak_memory), similar);
        }
        parallelism += similar * own.parallelism;
        merge += similar * own.merge_seconds;
        tests += similar * own.test_seconds * scale;
        weights += similar;
    }
    PackageEstimate guessed{.seconds = geometric_mean(seconds, 1),
                            .parallelism = parallelism / weights,
                            .merge_seconds = merge / weights,
                            .builds = alike.size(),
                            .guessed = true,
                            .test_seconds = tests / weights};
    if (!peaks.empty()) {
        guessed.peak_memory = static_cast<std::uint64_t>(std::llround(geometric_mean(peaks, 1)));
    }
    return guessed;
}

std::optional<PackageEstimate> BuildKnowledge::estimate(const BuildWanted& wanted,
                                                        unsigned cores) const {
    const auto& cpv = wanted.cpv;
    const auto found = by_cp_.find(cp_of(cpv));
    if (found == by_cp_.end()) {
        return std::nullopt;
    }
    const auto& all = found->second;
    const bool same_version =
        std::ranges::any_of(all, [&](const BuildRecord& record) { return record.cpv == cpv; });
    // The latest first, then those of the nearest USE, keeping that order among equals.
    std::vector<std::reference_wrapper<const BuildRecord>> pool;
    for (const auto& record : all | std::views::reverse) {
        if (!same_version || record.cpv == cpv) {
            pool.emplace_back(record);
        }
    }
    if (wanted.use) {
        std::ranges::stable_sort(pool, std::less<>{}, [&](const BuildRecord& record) {
            return use_distance(record.use, *wanted.use);
        });
    }
    const auto chosen = pool | std::views::take(estimate_builds) | std::ranges::to<std::vector>();
    // The most cores the package kept busy in any of them.
    double most_parallel = 0;
    for (const BuildRecord& record : chosen) {
        if (record.cpu_seconds && seconds_building(record) > 0) {
            most_parallel = std::max(most_parallel, *record.cpu_seconds / seconds_building(record));
        }
    }
    PackageEstimate estimate{.builds = chosen.size(), .same_version = same_version};
    std::vector<double> seconds;
    std::vector<double> cpu;
    std::vector<double> merges;
    std::vector<double> tests;
    for (const BuildRecord& record : chosen) {
        const auto cpu_seconds = record.cpu_seconds.value_or(0);
        const bool timed = starved(record) && record.cpu_seconds && most_parallel > 0;
        const auto scale = distfile_scale(record, wanted.distfiles);
        seconds.push_back(scale * (timed ? cpu_seconds / most_parallel : seconds_building(record)));
        tests.push_back(scale * test_phase_seconds(record));
        if (record.cpu_seconds) {
            cpu.push_back(scale * *record.cpu_seconds);
        }
        if (record.merge_seconds) {
            merges.push_back(*record.merge_seconds);
        }
        if (record.peak_memory) {
            estimate.peak_memory = std::max(estimate.peak_memory.value_or(0), *record.peak_memory);
        }
    }
    estimate.seconds = median(seconds);
    estimate.test_seconds = median(tests);
    // The largest directory, and where the latest of the builds that said ran.
    std::optional<double> latest;
    for (const BuildRecord& record : chosen) {
        const auto tmpdir = record.environment.find("PORTAGE_TMPDIR");
        if (!record.build_dir_size || tmpdir == record.environment.end()) {
            continue;
        }
        auto& directory = estimate.build_directory;
        if (!directory) {
            directory.emplace();
        }
        directory->bytes = std::max(directory->bytes, *record.build_dir_size);
        if (!latest || record.start > *latest) {
            latest = record.start;
            directory->tmpdir = tmpdir->second;
            directory->env_files = record.env_files;
        }
    }
    if (!cpu.empty()) {
        estimate.parallelism = estimate.seconds > 0 ? median(cpu) / estimate.seconds : 0;
    }
    estimate.parallelism = std::min(estimate.parallelism, static_cast<double>(cores));
    if (!merges.empty()) {
        estimate.merge_seconds = median(merges);
    }
    return estimate;
}

std::vector<std::size_t> build_order(const Plan& plan, std::span<const Step> steps,
                                     std::span<const StepCost> costs, BuildOrder order) {
    std::vector<std::size_t> found(steps.size());
    std::ranges::iota(found, std::size_t{0});
    if (order == BuildOrder::emerge) {
        return found;
    }
    const auto cost = [&](std::size_t step) {
        if (!std::holds_alternative<MergeStep>(steps.subspan(step).front()) ||
            step >= costs.size()) {
            return 0.0;
        }
        const auto& each = costs.subspan(step).front();
        return each.build_seconds + (order == BuildOrder::critical ? each.merge_seconds : 0.0);
    };
    std::vector<double> priority(steps.size());
    if (order == BuildOrder::longest) {
        for (std::size_t step = 0; step < steps.size(); ++step) {
            priority.at(step) = cost(step);
        }
    } else {
        // By merge, its step; then by step, the later steps waiting for it.
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> step_of(plan.merges.size(), none);
        for (std::size_t step = 0; step < steps.size(); ++step) {
            if (const auto* merge = std::get_if<MergeStep>(&steps.subspan(step).front())) {
                step_of.at(merge->merge) = step;
            }
        }
        std::vector<std::vector<std::size_t>> waiting(steps.size());
        for (std::size_t step = 0; step < steps.size(); ++step) {
            if (const auto* merge = std::get_if<MergeStep>(&steps.subspan(step).front())) {
                for (const auto& wait : plan.merges.at(merge->merge).waits) {
                    const auto other = step_of.at(wait.merge);
                    if (other != none && other < step) {
                        waiting.at(other).push_back(step);
                    }
                }
            }
        }
        for (std::size_t step = steps.size(); step-- > 0;) {
            double after = 0;
            for (const auto later : waiting.at(step)) {
                after = std::max(after, priority.at(later));
            }
            priority.at(step) = cost(step) + after;
        }
    }
    std::ranges::stable_sort(found, std::greater<>{},
                             [&priority](std::size_t step) { return priority.at(step); });
    return found;
}

PlanEstimate estimate_plan(Schedule schedule, std::span<const StepCost> costs, unsigned cores,
                           std::optional<std::uint32_t> tokens,
                           std::span<const RunningStep> running) {
    struct Going {
        std::size_t step;
        bool merging;
        // Seconds left: a build's at its own pace, a merge's as they pass.
        double left;
    };
    const auto steps = schedule.steps().size();
    const std::vector<StepCost> cost_of(costs.begin(), costs.end());
    PlanEstimate estimate{.finished = std::vector<std::optional<double>>(steps)};
    for (std::size_t step = 0; step < steps; ++step) {
        if (std::holds_alternative<MergeStep>(schedule.step(step)) &&
            schedule.stage(step) != Schedule::Stage::done) {
            const auto& cost = cost_of.at(step);
            ++(cost.known ? estimate.known : cost.guessed ? estimate.guessed : estimate.unknown);
        }
    }
    std::vector<Going> going;
    for (const auto& [step, merging, elapsed] : running) {
        const auto& cost = cost_of.at(step);
        going.push_back(
            {.step = step,
             .merging = merging,
             .left = std::max((merging ? cost.merge_seconds : cost.build_seconds) - elapsed, 0.0)});
    }
    double now = 0;
    for (;;) {
        while (const auto step = schedule.next_merge()) {
            schedule.merge_started(*step);
            going.push_back(
                {.step = *step, .merging = true, .left = cost_of.at(*step).merge_seconds});
        }
        while (const auto step = schedule.next_build()) {
            if (!schedule.alone() && tokens && schedule.building() >= *tokens) {
                break;
            }
            schedule.build_started(*step);
            going.push_back(
                {.step = *step, .merging = false, .left = cost_of.at(*step).build_seconds});
        }
        std::optional<std::uint64_t> memory;
        double wanted = 0;
        for (const auto& each : going) {
            if (!each.merging) {
                const auto& cost = cost_of.at(each.step);
                wanted += cost.parallelism;
                if (cost.peak_memory) {
                    memory = memory.value_or(0) + *cost.peak_memory;
                }
            }
        }
        if (memory) {
            estimate.peak_memory = std::max(estimate.peak_memory.value_or(0), *memory);
        }
        if (going.empty()) {
            break;
        }
        // Builds slowed alike when they want more cores than there are; one that wants none
        // keeps its pace, as merges do.
        const double share = wanted > cores ? cores / wanted : 1.0;
        const auto pace = [&](const Going& each) {
            return each.merging || cost_of.at(each.step).parallelism == 0 ? 1.0 : share;
        };
        double next = std::numeric_limits<double>::infinity();
        for (const auto& each : going) {
            next = std::min(next, each.left / pace(each));
        }
        now += next;
        std::vector<Going> ended;
        std::vector<Going> left;
        for (auto& each : going) {
            each.left -= next * pace(each);
            (each.left <= 1e-9 ? ended : left).push_back(each);
        }
        going = std::move(left);
        for (const auto& each : ended) {
            if (each.merging) {
                schedule.merge_finished(each.step, true);
                estimate.finished.at(each.step) = now;
            } else {
                schedule.build_finished(each.step, true);
            }
        }
    }
    estimate.seconds = now;
    return estimate;
}

std::vector<StepFacts> step_facts(const RepositoryIndex& index, std::span<const BuildStep> steps) {
    std::map<std::pair<std::string_view, std::string_view>, std::size_t> wanted;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.subspan(i).front();
        if (!step.uninstall) {
            wanted.emplace(std::pair{std::string_view{step.cpv}, std::string_view{step.repo}}, i);
        }
    }
    std::map<std::pair<std::string_view, std::string_view>,
             std::reference_wrapper<const IndexVersion>>
        found;
    for (const auto& version : index.versions) {
        const std::pair key{index.string(version.cpv),
                            index.string(index.repositories.at(version.repository).name)};
        if (wanted.contains(key)) {
            found.emplace(key, version);
        }
    }
    std::vector<StepFacts> facts;
    facts.reserve(steps.size());
    for (const auto& step : steps) {
        const auto version =
            found.find(std::pair{std::string_view{step.cpv}, std::string_view{step.repo}});
        if (step.uninstall || version == found.end()) {
            facts.emplace_back();
            continue;
        }
        std::vector<std::string> eclasses;
        for (const auto id : index.ids_in(version->second.get().inherited)) {
            eclasses.emplace_back(index.string(id));
        }
        std::ranges::sort(eclasses);
        facts.push_back({.distfiles = distfiles_size(index, version->second, step.use),
                         .eclasses = std::move(eclasses)});
    }
    return facts;
}

std::vector<StepCost> step_costs(const BuildKnowledge& knowledge, std::span<const BuildStep> steps,
                                 unsigned cores, std::span<const StepFacts> facts) {
    std::vector<StepCost> costs;
    costs.reserve(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.subspan(i).front();
        const auto fact = i < facts.size() ? facts.subspan(i).front() : StepFacts{};
        const BuildWanted wanted{.cpv = step.cpv,
                                 .use = step.use,
                                 .distfiles = fact.distfiles,
                                 .eclasses = fact.eclasses};
        auto estimate = step.uninstall ? std::nullopt : knowledge.estimate(wanted, cores);
        if (!estimate && !step.uninstall) {
            estimate = knowledge.guess(wanted, cores);
        }
        if (!estimate) {
            costs.emplace_back();
            continue;
        }
        costs.push_back({.build_seconds = estimate->seconds,
                         .parallelism = estimate->parallelism,
                         .peak_memory = estimate->peak_memory,
                         .merge_seconds = estimate->merge_seconds,
                         .known = !estimate->guessed,
                         .guessed = estimate->guessed,
                         .test_seconds = estimate->test_seconds,
                         .build_directory = estimate->build_directory});
    }
    return costs;
}

std::optional<std::uint32_t> recorded_jobs(const std::vector<BuildRecord>& records) {
    return records.empty() ? std::nullopt : records.back().jobs;
}

std::string estimate_line(const PlanEstimate& estimate, std::optional<std::uint32_t> jobs) {
    return std::format("\testimate\t{}\t{}\t{}\t{}\t{}\t{}", std::llround(estimate.seconds),
                       estimate.peak_memory ? std::to_string(*estimate.peak_memory) : "",
                       estimate.known, estimate.unknown, jobs ? std::to_string(*jobs) : "",
                       estimate.guessed);
}

std::optional<EstimateLine> parse_estimate_line(std::string_view line) {
    std::vector<std::string_view> fields;
    for (const auto field : std::views::split(line, '\t')) {
        fields.emplace_back(field.begin(), field.end());
    }
    if ((fields.size() != 7 && fields.size() != 8) || !fields.at(0).empty() ||
        fields.at(1) != "estimate") {
        return std::nullopt;
    }
    const auto number = [](std::string_view text, auto& value) {
        const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
        return error == std::errc{} && end == text.end();
    };
    EstimateLine read;
    if (!number(fields.at(2), read.seconds) || !number(fields.at(4), read.known) ||
        !number(fields.at(5), read.unknown)) {
        return std::nullopt;
    }
    if (!fields.at(3).empty() && !number(fields.at(3), read.peak_memory.emplace())) {
        return std::nullopt;
    }
    if (!fields.at(6).empty() && !number(fields.at(6), read.jobs.emplace())) {
        return std::nullopt;
    }
    if (fields.size() == 8 && !number(fields.at(7), read.guessed)) {
        return std::nullopt;
    }
    return read;
}

Countdown::Countdown(std::vector<StepCost> costs, unsigned cores,
                     std::optional<std::uint32_t> tokens)
    : costs_{std::move(costs)}, cores_{cores}, tokens_{tokens} {}

void Countdown::traced(std::size_t step, Traced what, double now) {
    switch (what) {
    case Traced::build_started:
    case Traced::merge_started:
        running_.insert_or_assign(step,
                                  Since{.merging = what == Traced::merge_started, .start = now});
        break;
    case Traced::built:
    case Traced::build_failed:
    case Traced::merged:
    case Traced::merge_failed:
        running_.erase(step);
        break;
    case Traced::skipped:
        break;
    }
    stale_ = true;
}

std::optional<double> Countdown::left(const Schedule& schedule, double now) {
    if (stale_) {
        std::vector<RunningStep> running;
        running.reserve(running_.size());
        for (const auto& [step, since] : running_) {
            running.push_back(
                {.step = step, .merging = since.merging, .elapsed = now - since.start});
        }
        const auto estimate = estimate_plan(schedule, costs_, cores_, tokens_, running);
        estimated_ =
            estimate.known + estimate.guessed == 0 ? std::nullopt : std::optional{estimate.seconds};
        estimated_at_ = now;
        stale_ = false;
    }
    if (!estimated_) {
        return std::nullopt;
    }
    return std::max(*estimated_ - (now - estimated_at_), 0.0);
}

void expect_builds(std::vector<emerge::Snapshot>& snapshots, const BuildKnowledge& knowledge,
                   unsigned cores) {
    for (auto& snapshot : snapshots) {
        for (auto& task : snapshot.tasks) {
            if (task.kind != emerge::TaskKind::build || task.binary ||
                !(task.operation.empty() || task.operation == "merge")) {
                continue;
            }
            if (const auto estimate = knowledge.estimate(task.cpv, cores)) {
                task.expected = estimate->seconds;
            }
        }
    }
}

} // namespace egraph
