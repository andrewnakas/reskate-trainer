#include "Extension/Trainer/trainer_physics_policy.h"
#include "Extension/Trainer/trainer_command_snapshot.h"
#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace policy = dingosdk::trainer::physics_policy;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

void carry_identity() {
    constexpr int hold = 360;
    policy::PushCarry carry;
    const policy::MotionOwner first{11, 22, 33};
    require(carry.bind(first, hold), "The first validated body starts a fresh push history");
    for (const auto next : std::array{policy::MotionOwner{44, 22, 33}, policy::MotionOwner{44, 55, 33}, policy::MotionOwner{44, 55, 66}}) {
        carry.pushing = 36;
        carry.since_push = 7;
        carry.carried = true;
        carry.reached = 90;
        require(!carry.bind(carry.owner, hold) && carry.reached == 90 && carry.pushing == 36,
                "Refreshing settings for the same body keeps its push history");
        require(carry.bind(next, hold), "Changing client, entity or physics core starts a new owner");
        require(carry.owner == next && carry.pushing == 0 && carry.since_push == hold && !carry.carried && carry.reached == 0,
                "A new owner inherits no push hold, target or carried flag");
    }
    carry.reached = 40;
    carry.clear(hold);
    require(carry.owner == policy::MotionOwner{} && carry.reached == 0 && carry.since_push == hold,
            "An inactive, expired or unavailable command drops the entire carry history");
}
void reduced_ceiling() {
    policy::PushCarry carry;
    carry.reached = 80;
    require(policy::carry_gain(carry, 30, 20, .2f) == 0 && carry.reached == 20,
            "Reducing the setting cannot accelerate an already faster skater toward the old target");
    carry.reached = 80;
    const float speed = 19.95f;
    const float gain = policy::carry_gain(carry, speed, 20, .2f);
    require(gain > 0 && gain <= .2f && speed + gain <= 20 && carry.reached == 20,
            "Carry below a reduced ceiling stops precisely at the new ceiling");
    require(policy::carry_gain(carry, 20, 20, .2f) == 0, "No extra acceleration at the active ceiling");
}
void pump_identity() {
    policy::PumpAccumulator accumulator;
    const policy::MotionOwner first{11, 22, 33};
    require(accumulator.bind(first), "The first pump sample binds its physics owner");
    for (const auto next : std::array{policy::MotionOwner{44, 22, 33}, policy::MotionOwner{44, 55, 33}, policy::MotionOwner{44, 55, 66}}) {
        accumulator.count = 28;
        accumulator.primed = accumulator.running = true;
        accumulator.energy = 150;
        accumulator.sum = 12;
        accumulator.credit = 4;
        require(!accumulator.bind(accumulator.owner) && accumulator.energy == 150 && accumulator.count == 28,
                "Coherent refreshes for the same pump owner retain its measurement");
        require(accumulator.bind(next), "A changed pump body is a new owner");
        require(accumulator.owner == next && !accumulator.primed && !accumulator.running && accumulator.count == 0 &&
                accumulator.energy == 0 && accumulator.sum == 0 && accumulator.credit == 0,
                "Pumping cannot compare a new skater's energy against the previous skater's");
    }
    accumulator.primed = true;
    accumulator.energy = 150;
    accumulator.clear();
    require(accumulator.owner == policy::MotionOwner{} && !accumulator.primed && accumulator.energy == 0,
            "Disconnecting or stopping pumping invalidates the energy baseline");
}
void revert_identity() {
    policy::RevertHistory history;
    require(history.bind({11, 22, 33}), "A revert history starts with a validated owner");
    require(!history.accept(10) && history.accept(11) && !history.accept(11),
            "Only new landings after priming are judged, once each");
    history.cooldown_until = 9000;
    require(!history.bind({11, 22, 33}) && history.cooldown_until == 9000,
            "Refreshing the same owner retains its landing cooldown");
    for (const auto next : std::array{policy::MotionOwner{44, 22, 33}, policy::MotionOwner{44, 55, 33}, policy::MotionOwner{44, 55, 66}}) {
        require(history.bind(next) && !history.primed && history.seen == 0 && history.cooldown_until == 0,
                "A new revert owner receives neither the previous landing nor its cooldown");
        require(!history.accept(11) && history.accept(12), "An existing landing cannot fire immediately after an owner change");
        history.cooldown_until = 9000;
    }
    history.clear();
    require(!history.primed && history.cooldown_until == 0 && !history.accept(12),
            "Re-enabling an expired revert command first discards the historical landing");
}
void unrelated_steps() {
    const policy::MotionOwner owner{11, 22, 33}, replacement{44, 55, 66};
    policy::PushCarry push;
    policy::PumpAccumulator pump;
    policy::RevertHistory revert;
    require(policy::bind_step(push, owner, 33, 360) == policy::StepOwnership::changed &&
            policy::bind_step(pump, owner, 33) == policy::StepOwnership::changed &&
            policy::bind_step(revert, owner, 33) == policy::StepOwnership::changed,
            "Only this skater's core establishes its histories");
    push.reached = 40;
    push.pushing = 12;
    pump.primed = pump.running = true;
    pump.energy = 150;
    require(!revert.accept(10), "Revert history primes on its own core");
    revert.cooldown_until = 9000;
    for (const auto candidate : {owner, replacement}) {
        require(policy::bind_step(push, candidate, 999, 360) == policy::StepOwnership::unrelated &&
                policy::bind_step(pump, candidate, 999) == policy::StepOwnership::unrelated &&
                policy::bind_step(revert, candidate, 999) == policy::StepOwnership::unrelated,
                "Another skater's callback is ignored even with a newly published local owner");
        require(push.owner == owner && push.reached == 40 && push.pushing == 12 && pump.owner == owner && pump.running &&
                pump.energy == 150 && revert.owner == owner && revert.primed && revert.seen == 10 && revert.cooldown_until == 9000,
                "Unrelated physics callbacks preserve local carry, pump energy and landing cooldown");
    }
    require(policy::bind_step(push, replacement, 66, 360) == policy::StepOwnership::changed && push.reached == 0 &&
            policy::bind_step(pump, replacement, 66) == policy::StepOwnership::changed && !pump.primed &&
            policy::bind_step(revert, replacement, 66) == policy::StepOwnership::changed && !revert.primed,
            "The replacement skater's own callback starts fresh histories");
}
void flip_lifetime() {
    const policy::FlipObservation expected{{10, 20, 30, 40, 50, 0x1000010, 0x1000020}, 2, .6f};
    require(policy::can_write_flip(expected, expected, 1), "An unchanged owner and observation can restore its baseline");
    for (int changed = 0; changed < 8; ++changed) {
        auto current = expected;
        switch (changed) {
            case 0: ++current.owner.base; break;
            case 1: ++current.owner.entity; break;
            case 2: ++current.owner.block; break;
            case 3: ++current.owner.speed_at; break;
            case 4: ++current.owner.trick_at; break;
            case 5: ++current.owner.speed_handle; break;
            case 6: ++current.owner.trick_handle; break;
            case 7: ++current.trick; break;
        }
        require(!policy::can_write_flip(expected, current, 1),
                "Matching speed alone never permits a write or restore across a changed owner, handle or trick");
    }
    auto current = expected;
    current.speed = .7f;
    require(!policy::can_write_flip(expected, current, 1), "A newly supplied game speed supersedes the old observation");
    current = expected;
    current.owner = {};
    require(!policy::can_write_flip(expected, current, 1), "An unavailable state cannot be written");
    require(policy::can_write_flip(expected, expected, 0) && policy::can_write_flip(expected, expected, 1000),
            "The supported flip speed endpoints remain usable");
}
void catch_priority() {
    constexpr float saved_individual = 100;
    // boosts_in_force supplies .25x while catch is enabled with a stock global speed.
    const float catch_speed = policy::effective_flip_factor(.25f, saved_individual, true, true, false);
    require(catch_speed == .25f && catch_speed * 1.2f == .3f,
            "Timed catch keeps its slow animation base even with a stored 100x individual speed");
    require(policy::effective_flip_factor(.5f, saved_individual, true, false, false) == 50,
            "Turning catch off restores the ordinary global times saved individual speed");
    require(policy::effective_flip_factor(.75f, saved_individual, true, false, true) == .75f,
            "Blocked personal extras keep the effective global session speed");
    require(policy::effective_flip_factor(.5f, saved_individual, false, false, false) == .5f,
            "Disabling advanced speeds retains the selected global speed");
    require(policy::effective_flip_factor(.01f, saved_individual, true, true, false) == .01f,
            "An intentionally slower global catch base is preserved without an individual boost");
}
void nonfinite() {
    const float nan = std::numeric_limits<float>::quiet_NaN(), infinity = std::numeric_limits<float>::infinity();
    for (const float invalid : {nan, infinity, -infinity}) {
        require(policy::push_factor(invalid) == 1 && policy::push_stock(invalid) == 9.25f &&
                policy::cruise_speed(invalid) == 0 && policy::pump_factor(invalid) == 1,
                "Nonfinite physics commands always select safe defaults");
        require(policy::revert_strength(invalid) == 0 && !policy::finite_values({90, 40, invalid, 1.5f, 40, .5f}),
                "A nonfinite revert setting disables a command before any velocity write");
        policy::PushCarry carry;
        carry.reached = invalid;
        require(policy::carry_gain(carry, 10, 20, .2f) == 0 && carry.reached == 0,
                "A nonfinite saved push target cannot reach a velocity write");
        carry.reached = 15;
        require(policy::carry_gain(carry, 10, invalid, .2f) == 0, "A nonfinite ceiling clears carry");
        const policy::FlipObservation expected{{10, 20, 30, 40, 50, 1, 2}, 2, 1};
        require(!policy::can_write_flip(expected, expected, invalid), "A nonfinite requested flip speed is refused");
        auto current = expected;
        current.speed = invalid;
        require(!policy::can_write_flip(expected, current, 1), "A nonfinite live flip state is refused");
    }
    require(policy::pump_factor(0) == 0 && policy::push_factor(2) == 2 && policy::cruise_speed(8) == 8,
            "Finite supported commands retain their intended values");
    require(policy::revert_strength(2) == 2 && policy::revert_strength(2000) == 1000 && policy::finite_values({90, 40, 1.5f}),
            "Finite revert strength retains its supported range");
    const policy::FlipObservation expected{{10, 20, 30, 40, 50, 1, 2}, 2, 1};
    require(!policy::can_write_flip(expected, expected, -1) && !policy::can_write_flip(expected, expected, 1001),
            "Finite out-of-range flip writes are also refused");
}
void coherent_commands() {
    // Correlated fields represent a complete owner/settings/expiry generation. This uses
    // the very mailbox production uses, with concurrent publishers and consumers; no game
    // memory, controller input, physics hooks or sleeps are involved.
    struct Command {
        std::uint64_t sequence{}, client{}, entity{}, expires{};
        float factor{}, stock{};
        bool pumping{}, measure{};
        std::array<float, 9> tuning{};
        bool operator==(const Command &) const = default;
    };
    const auto command_for = [](std::uint64_t sequence) {
        Command command{sequence, sequence * 11 + 3, sequence * 17 + 5, sequence * 31 + 7,
                       static_cast<float>(sequence % 97 + 1), static_cast<float>(sequence % 73 + 2),
                       (sequence & 1) != 0, (sequence & 2) != 0};
        for (std::size_t i = 0; i < command.tuning.size(); ++i) command.tuning[i] = static_cast<float>(sequence % 101 + i);
        return command;
    };
    policy::CommandSnapshot<Command> mailbox;
    mailbox.store(command_for(0));
    std::barrier start{6};
    std::atomic<bool> coherent{true};
    std::atomic<std::uint32_t> reads{};
    std::vector<std::jthread> threads;
    for (std::uint64_t writer = 0; writer < 2; ++writer) threads.emplace_back([&, writer] {
        start.arrive_and_wait();
        for (std::uint64_t i = 1; i <= 100000; ++i) mailbox.store(command_for(i * 2 + writer));
    });
    for (int reader = 0; reader < 3; ++reader) threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 100000; ++i) {
            const auto snapshot = mailbox.load();
            if (!(snapshot == command_for(snapshot.sequence))) coherent.store(false, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    start.arrive_and_wait();
    for (auto &thread : threads) thread.join();
    require(coherent.load() && reads.load() == 300000, "Every concurrent read contains exactly one complete command generation");
}
int main() {
    try {
        carry_identity();
        reduced_ceiling();
        pump_identity();
        revert_identity();
        unrelated_steps();
        flip_lifetime();
        catch_priority();
        nonfinite();
        coherent_commands();
        std::cout << "Physics ownership, reduced carry, flip lifetime, finite inputs and coherent commands passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
