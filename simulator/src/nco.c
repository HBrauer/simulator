#include "nco.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NCO_TWO_POW_64 18446744073709551616.0 /* 2^64 */

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

uint64_t nco_phase_step_q64(double frequency_hz, double sample_rate_hz)
{
    if (!(sample_rate_hz > 0.0)) {
        return 0;
    }
    double turns = frequency_hz / sample_rate_hz; /* cycles per sample, any sign/magnitude */
    turns -= floor(turns);                        /* reduce to [0, 1) turns */
    double scaled = turns * NCO_TWO_POW_64;
    if (scaled >= NCO_TWO_POW_64) {
        scaled = 0.0; /* guard against rounding turns==0.999.. up to exactly 2^64 */
    }
    return (uint64_t)scaled;
}

double nco_phase_rad_at(uint64_t phase_step_q64, uint64_t sample_index)
{
    const uint64_t phase_q64 = phase_step_q64 * sample_index; /* wrapping == mod one turn */
    const double frac = (double)phase_q64 / NCO_TWO_POW_64;   /* [0, 1) turns */
    return frac * (2.0 * M_PI);
}
