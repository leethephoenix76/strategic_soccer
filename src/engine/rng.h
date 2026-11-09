// Small deterministic random number generator stored inside the game state.
// Copying a GameState copies the generator, so a copied state replays exactly
// the same random events - convenient for tests, replays and search-based bots.
#pragma once

#include <cstdint>

namespace soccer {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x5eed5eedULL) : state_(seed) {}

    void Seed(uint64_t seed) { state_ = seed; }

    // splitmix64
    uint64_t Next() {
        uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform number in [0, 1).
    double Uniform() { return (Next() >> 11) * (1.0 / 9007199254740992.0); }

    // Uniform index in [0, n); n must be positive.
    int Index(int n) { return static_cast<int>(Uniform() * n); }

private:
    uint64_t state_;
};

}  // namespace soccer
