/* Multi-stage DDC filter design (see ddc.h for the stage/stopband model).
 *
 * Plans are pure functions of (source_rate, output_rate, bandwidth): the factorization is a
 * deterministic greedy walk and the tap values depend only on the stage rates, so two
 * instances always build byte-identical plans -- a prerequisite for the renderer's
 * byte-identical-output invariant. */
#include "ddc.h"
#include "sim_config.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if SIM_HAVE_VOLK
#include <volk/volk.h>
#endif

/* Transition bands are never allowed narrower than this fraction of the stage output rate,
 * so profiles with sample_rate close to bandwidth still get a bounded filter (the passband
 * protection then reaches slightly less far than bandwidth/2 -- documented approximation). */
#define DDC_MIN_TRANSITION_FRACTION 0.04

#define DDC_CACHE_MIN_TAIL_RATIO 16U
#define DDC_CACHE_MIN_FRONT_DECIM 4U

static void ddc_set_error(char *error, size_t error_size, const char *code)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", code);
    }
}

static double ddc_sinc(double x)
{
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(M_PI * x) / (M_PI * x);
}

/* Zeroth-order modified Bessel function of the first kind, by its power series. Converges in
 * a handful of terms for the beta values 80 dB designs use (~8). */
static double ddc_i0(double x)
{
    const double half_sq = x * x / 4.0;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; k++) {
        term *= half_sq / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-14) {
            break;
        }
    }
    return sum;
}

static double ddc_kaiser_beta(double stopband_db)
{
    if (stopband_db > 50.0) {
        return 0.1102 * (stopband_db - 8.7);
    }
    if (stopband_db >= 21.0) {
        return 0.5842 * pow(stopband_db - 21.0, 0.4) + 0.07886 * (stopband_db - 21.0);
    }
    return 0.0;
}

/* Kaiser tap-count estimate N ~= (A - 8) / (2.285 * delta_omega) + 1, rounded up to odd.
 * The factorization uses the same estimate the designer uses, so a stage it accepts always
 * designs successfully. */
static uint32_t ddc_tap_estimate(double input_rate_hz, double transition_hz, double stopband_db)
{
    const double delta_omega = 2.0 * M_PI * transition_hz / input_rate_hz;
    const double estimate = (stopband_db - 8.0) / (2.285 * delta_omega) + 1.0;
    double taps = ceil(estimate);
    if (taps < 5.0) {
        taps = 5.0;
    }
    if (taps > (double)UINT32_MAX) {
        return UINT32_MAX;
    }
    uint32_t count = (uint32_t)taps;
    if ((count & 1U) == 0U) {
        count += 1U;
    }
    return count;
}

static bool ddc_design_stage(ddc_stage_t *stage,
                             double input_rate_hz,
                             uint32_t decim,
                             double passband_edge_hz,
                             double stopband_edge_hz,
                             double stopband_db,
                             uint32_t max_taps)
{
    const double transition_hz = stopband_edge_hz - passband_edge_hz;
    if (transition_hz <= 0.0) {
        return false;
    }
    const uint32_t taps = ddc_tap_estimate(input_rate_hz, transition_hz, stopband_db);
    if (taps > max_taps) {
        return false;
    }
    const double beta = ddc_kaiser_beta(stopband_db);
    const double i0_beta = ddc_i0(beta);
    /* Normalized cutoff 2*fc/Fin at the middle of the transition band. */
    const double cutoff = (passband_edge_hz + stopband_edge_hz) / input_rate_hz;
    const int center = ((int)taps - 1) / 2;
    double sum = 0.0;
    for (uint32_t j = 0; j < taps; j++) {
        const double offset = (double)((int)j - center);
        const double t = center > 0 ? offset / (double)center : 0.0;
        const double window = ddc_i0(beta * sqrt(1.0 - t * t)) / i0_beta;
        const double value = cutoff * ddc_sinc(cutoff * offset) * window;
        stage->taps[j] = (float)value;
        sum += value;
    }
    if (fabs(sum) < 1e-12) {
        return false;
    }
    for (uint32_t j = 0; j < taps; j++) {
        stage->taps[j] = (float)((double)stage->taps[j] / sum);
    }
    stage->decim = decim;
    stage->tap_count = taps;
    return true;
}

/* Stage transition band for a candidate decimation: passband is always the final channel
 * half-bandwidth; the stopband starts where post-decimation folding would land on it. For a
 * non-final stage a clamped (too-narrow) transition means the stage output cannot hold the
 * band at all, so the candidate is rejected; the final stage keeps the clamp as a documented
 * approximation for tight bandwidth/rate profiles. Returns false when not viable. */
static bool ddc_stage_edges(double output_rate_hz,
                            double passband_edge_hz,
                            bool is_final,
                            double *stopband_edge_hz)
{
    double stopband = output_rate_hz - passband_edge_hz;
    const double min_transition = DDC_MIN_TRANSITION_FRACTION * output_rate_hz;
    if (stopband - passband_edge_hz < min_transition) {
        if (!is_final) {
            return false;
        }
        stopband = passband_edge_hz + min_transition;
    }
    *stopband_edge_hz = stopband;
    return true;
}

bool ddc_plan_design(ddc_plan_t *plan,
                     uint32_t source_rate_hz,
                     uint32_t output_rate_hz,
                     uint32_t bandwidth_hz,
                     double stopband_db,
                     char *error,
                     size_t error_size)
{
    memset(plan, 0, sizeof(*plan));
    if (source_rate_hz == 0U || output_rate_hz == 0U || source_rate_hz % output_rate_hz != 0U) {
        ddc_set_error(error, error_size, "non_integer_ratio");
        return false;
    }
    const uint32_t ratio = source_rate_hz / output_rate_hz;
    if (ratio < 2U) {
        ddc_set_error(error, error_size, "ratio_too_small");
        return false;
    }
    if (bandwidth_hz == 0U || bandwidth_hz >= output_rate_hz) {
        ddc_set_error(error, error_size, "invalid_bandwidth");
        return false;
    }
    plan->source_rate_hz = source_rate_hz;
    plan->output_rate_hz = output_rate_hz;
    plan->bandwidth_hz = bandwidth_hz;
    plan->ratio = ratio;

    const double passband_edge_hz = (double)bandwidth_hz / 2.0;
    uint32_t remaining = ratio;
    double input_rate_hz = (double)source_rate_hz;
    while (remaining > 1U) {
        if (plan->stage_count == DDC_MAX_STAGES) {
            ddc_set_error(error, error_size, "too_many_stages");
            return false;
        }
        /* Greedy: the largest divisor whose tap estimate fits the stage cap. A single
         * oversized final stage (remaining > DDC_MAX_STAGE_DECIM, e.g. a large prime) is
         * tried last, against the roomier final-stage cap. */
        uint32_t chosen = 0U;
        double chosen_stopband_hz = 0.0;
        const uint32_t start = remaining < DDC_MAX_STAGE_DECIM ? remaining : DDC_MAX_STAGE_DECIM;
        for (uint32_t d = start; d >= 2U; d--) {
            if (remaining % d != 0U) {
                continue;
            }
            const bool is_final = d == remaining;
            const double output_hz = input_rate_hz / (double)d;
            double stopband_hz;
            if (!ddc_stage_edges(output_hz, passband_edge_hz, is_final, &stopband_hz)) {
                continue;
            }
            const uint32_t estimate = ddc_tap_estimate(input_rate_hz, stopband_hz - passband_edge_hz, stopband_db);
            if (estimate <= (is_final ? DDC_MAX_STAGE_TAPS : DDC_NONFINAL_STAGE_TAPS)) {
                chosen = d;
                chosen_stopband_hz = stopband_hz;
                break;
            }
        }
        if (chosen == 0U && remaining > DDC_MAX_STAGE_DECIM) {
            const double output_hz = input_rate_hz / (double)remaining;
            double stopband_hz;
            if (ddc_stage_edges(output_hz, passband_edge_hz, true, &stopband_hz) &&
                ddc_tap_estimate(input_rate_hz, stopband_hz - passband_edge_hz, stopband_db) <= DDC_MAX_STAGE_TAPS) {
                chosen = remaining;
                chosen_stopband_hz = stopband_hz;
            }
        }
        if (chosen == 0U) {
            ddc_set_error(error, error_size, "no_stage_factorization");
            return false;
        }
        if (!ddc_design_stage(&plan->stages[plan->stage_count],
                              input_rate_hz,
                              chosen,
                              passband_edge_hz,
                              chosen_stopband_hz,
                              stopband_db,
                              chosen == remaining ? DDC_MAX_STAGE_TAPS : DDC_NONFINAL_STAGE_TAPS)) {
            ddc_set_error(error, error_size, "stage_design_failed");
            return false;
        }
        plan->stage_count++;
        input_rate_hz /= (double)chosen;
        remaining /= chosen;
    }

    /* Composite reach: output sample m needs stage-k inputs m*Dk +/- Mk, each of which needs
     * the previous stage's +/- M(k-1) around it, so in source samples the half-widths add up
     * weighted by the cumulative decimation in front of each stage. */
    uint64_t stride = 1;
    uint64_t reach = 0;
    for (size_t k = 0; k < plan->stage_count; k++) {
        reach += (uint64_t)((plan->stages[k].tap_count - 1U) / 2U) * stride;
        stride *= plan->stages[k].decim;
    }
    plan->history_source_samples = reach;
    return true;
}

static int64_t ddc_ceil_div_i64(int64_t numerator, int64_t denominator)
{
    /* denominator > 0; exact ceiling for negative numerators too (early blocks feed the
     * cascade from before source index 0 via the caller's zero-padding). */
    if (numerator >= 0) {
        return (numerator + denominator - 1) / denominator;
    }
    return -((-numerator) / denominator);
}

void ddc_exec_init(ddc_exec_t *exec, const ddc_plan_t *plan, int64_t first_input_index)
{
    exec->plan = plan;
    int64_t in_start = first_input_index;
    for (size_t k = 0; k < plan->stage_count; k++) {
        ddc_exec_stage_t *stage_state = &exec->stages[k];
        const int64_t half = (int64_t)((plan->stages[k].tap_count - 1U) / 2U);
        const int64_t decim = (int64_t)plan->stages[k].decim;
        stage_state->buf_start = in_start;
        stage_state->next_in = in_start;
        stage_state->buf_len = 0;
        /* First output whose centered window [n*D - M, n*D + M] starts at or after the first
         * available input. Feeding from that window's start onward, output n is exact. */
        stage_state->next_out = ddc_ceil_div_i64(in_start + half, decim);
        in_start = stage_state->next_out;
    }
}

int64_t ddc_exec_next_output_index(const ddc_exec_t *exec)
{
    return exec->stages[exec->plan->stage_count - 1].next_out;
}

/* Append inputs to one stage and produce every output whose window is complete, dropping
 * inputs no future window can reach. Returns outputs written to `out` (capped at capacity;
 * production resumes on the next call where it paused). */
static size_t ddc_stage_process(ddc_exec_stage_t *stage_state,
                                const ddc_stage_t *stage,
                                const float complex *in,
                                size_t count,
                                float complex *out,
                                size_t out_capacity)
{
    assert(stage_state->buf_len + count <= DDC_EXEC_STAGE_CAPACITY);
    memcpy(&stage_state->buf[stage_state->buf_len], in, count * sizeof(*in));
    stage_state->buf_len += count;
    stage_state->next_in += (int64_t)count;

    const int64_t decim = (int64_t)stage->decim;
    const int64_t half = (int64_t)((stage->tap_count - 1U) / 2U);
    size_t produced = 0;
    while (produced < out_capacity &&
           stage_state->next_out * decim + half < stage_state->next_in) {
        const int64_t window_start = stage_state->next_out * decim - half;
        const float complex *window = &stage_state->buf[window_start - stage_state->buf_start];
#if SIM_HAVE_VOLK
        lv_32fc_t acc;
        volk_32fc_32f_dot_prod_32fc(&acc, (const lv_32fc_t *)window, stage->taps, stage->tap_count);
        out[produced] = acc;
#else
        float acc_re = 0.0f;
        float acc_im = 0.0f;
        for (uint32_t j = 0; j < stage->tap_count; j++) {
            acc_re += crealf(window[j]) * stage->taps[j];
            acc_im += cimagf(window[j]) * stage->taps[j];
        }
        out[produced] = acc_re + acc_im * I;
#endif
        produced++;
        stage_state->next_out++;
    }

    /* Drop everything below the next window's start. */
    const int64_t keep_from = stage_state->next_out * decim - half;
    if (keep_from > stage_state->buf_start) {
        const int64_t available_from = stage_state->next_in - (int64_t)stage_state->buf_len;
        const int64_t drop_from = keep_from < stage_state->next_in ? keep_from : stage_state->next_in;
        const size_t drop = (size_t)(drop_from - available_from);
        if (drop > 0) {
            stage_state->buf_len -= drop;
            memmove(stage_state->buf, &stage_state->buf[drop], stage_state->buf_len * sizeof(stage_state->buf[0]));
            stage_state->buf_start = drop_from;
        }
    }
    return produced;
}

size_t ddc_exec_push(ddc_exec_t *exec,
                     const float complex *in,
                     size_t count,
                     float complex *out,
                     size_t out_capacity)
{
    const ddc_plan_t *plan = exec->plan;
    size_t written = 0;
    size_t offset = 0;
    /* Ping-pong slice buffers: a stage's outputs are the next stage's inputs. A slice of
     * DDC_EXEC_CHUNK inputs produces at most CHUNK/2 + 1 outputs (decim >= 2), so the
     * fixed-size scratch always suffices and no stage buffer ever overflows. */
    float complex scratch_a[DDC_EXEC_CHUNK / 2U + 2U];
    float complex scratch_b[DDC_EXEC_CHUNK / 2U + 2U];
    while (offset < count) {
        const size_t slice = count - offset < DDC_EXEC_CHUNK ? count - offset : DDC_EXEC_CHUNK;
        const float complex *stage_in = in + offset;
        size_t stage_in_count = slice;
        for (size_t k = 0; k < plan->stage_count; k++) {
            const bool is_final = k + 1 == plan->stage_count;
            float complex *stage_out = is_final ? out + written : (stage_in == scratch_a ? scratch_b : scratch_a);
            const size_t capacity = is_final ? out_capacity - written : DDC_EXEC_CHUNK / 2U + 2U;
            stage_in_count = ddc_stage_process(&exec->stages[k], &plan->stages[k],
                                               stage_in, stage_in_count, stage_out, capacity);
            if (is_final) {
                written += stage_in_count;
            }
            stage_in = stage_out;
            if (stage_in_count == 0 && !is_final) {
                break; /* nothing propagated further this slice */
            }
        }
        offset += slice;
    }
    return written;
}

#define DDC_PLAN_CACHE 32U

static ddc_plan_t g_ddc_plans[DDC_PLAN_CACHE];
static _Atomic size_t g_ddc_plan_count = 0;
static pthread_mutex_t g_ddc_plan_lock = PTHREAD_MUTEX_INITIALIZER;

static bool ddc_plan_matches(const ddc_plan_t *plan,
                             uint32_t source_rate_hz,
                             uint32_t output_rate_hz,
                             uint32_t bandwidth_hz)
{
    return plan->source_rate_hz == source_rate_hz &&
           plan->output_rate_hz == output_rate_hz &&
           plan->bandwidth_hz == bandwidth_hz;
}

const ddc_plan_t *ddc_plan_cache_get(uint32_t source_rate_hz,
                                     uint32_t output_rate_hz,
                                     uint32_t bandwidth_hz)
{
    size_t count = atomic_load_explicit(&g_ddc_plan_count, memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        if (ddc_plan_matches(&g_ddc_plans[i], source_rate_hz, output_rate_hz, bandwidth_hz)) {
            return &g_ddc_plans[i];
        }
    }
    pthread_mutex_lock(&g_ddc_plan_lock);
    count = atomic_load_explicit(&g_ddc_plan_count, memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (ddc_plan_matches(&g_ddc_plans[i], source_rate_hz, output_rate_hz, bandwidth_hz)) {
            pthread_mutex_unlock(&g_ddc_plan_lock);
            return &g_ddc_plans[i];
        }
    }
    const ddc_plan_t *result = NULL;
    if (count < DDC_PLAN_CACHE) {
        char error[32];
        if (ddc_plan_design(&g_ddc_plans[count], source_rate_hz, output_rate_hz, bandwidth_hz,
                            DDC_STOPBAND_DB, error, sizeof(error))) {
            atomic_store_explicit(&g_ddc_plan_count, count + 1, memory_order_release);
            result = &g_ddc_plans[count];
        }
    }
    pthread_mutex_unlock(&g_ddc_plan_lock);
    return result;
}

uint32_t ddc_intermediate_rate_hz(uint32_t source_rate_hz, uint32_t output_rate_hz)
{
    if (source_rate_hz == 0U || output_rate_hz == 0U || source_rate_hz % output_rate_hz != 0U) {
        return 0U;
    }
    const uint32_t ratio = source_rate_hz / output_rate_hz;
    for (uint32_t m = DDC_CACHE_MIN_TAIL_RATIO; m <= ratio / DDC_CACHE_MIN_FRONT_DECIM; m++) {
        if (ratio % m == 0U) {
            return output_rate_hz * m; /* <= source_rate / DDC_CACHE_MIN_FRONT_DECIM */
        }
    }
    return 0U;
}

uint32_t ddc_front_bandwidth_hz(uint32_t intermediate_rate_hz)
{
    return (uint32_t)(((uint64_t)intermediate_rate_hz * 4ULL) / 5ULL);
}

uint32_t ddc_grid_step_hz(uint32_t intermediate_rate_hz)
{
    return intermediate_rate_hz / 5U;
}

int64_t ddc_grid_center_hz(int64_t center_hz, uint32_t grid_step_hz)
{
    if (grid_step_hz == 0U) {
        return center_hz;
    }
    const int64_t step = (int64_t)grid_step_hz;
    const int64_t shifted = center_hz + step / 2;
    int64_t quotient = shifted / step;
    if (shifted % step != 0 && shifted < 0) {
        quotient -= 1; /* floor division for negative centers */
    }
    return quotient * step;
}
