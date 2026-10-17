#pragma once
#include <cstddef>

namespace dbdeck {

// Something the mixer can pull interleaved stereo float frames from on the
// audio thread. Implementations must be real-time safe: no locks, no
// allocation, no I/O. `pull` must always fill `frames` frames (silence when
// no data is available).
class PullSource {
public:
    virtual ~PullSource() = default;
    virtual void pull(float* out, size_t frames) = 0;
};

}  // namespace dbdeck
