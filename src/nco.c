#include "nco.h"

#include <math.h>

void nco_init(nco_t *nco, double frequency_hz, double sample_rate_hz)
{
    nco->phase_rad = 0.0;
    nco->phase_step_rad = 2.0 * M_PI * frequency_hz / sample_rate_hz;
}

int16_t sim_clip_i16(double value)
{
    if (value > 32767.0) {
        return 32767;
    }
    if (value < -32768.0) {
        return -32768;
    }
    return (int16_t)lrint(value);
}

void nco_mix_ci16(const nco_t *initial, const iq_ci16_t *in, iq_ci16_t *out, size_t count, double gain)
{
    double phase = initial->phase_rad;
    for (size_t i = 0; i < count; i++) {
        const double c = cos(phase);
        const double s = sin(phase);
        const double ii = (double)in[i].i;
        const double qq = (double)in[i].q;
        out[i].i = sim_clip_i16(gain * (ii * c - qq * s));
        out[i].q = sim_clip_i16(gain * (ii * s + qq * c));
        phase += initial->phase_step_rad;
        if (phase > M_PI) {
            phase -= 2.0 * M_PI;
        } else if (phase < -M_PI) {
            phase += 2.0 * M_PI;
        }
    }
}
