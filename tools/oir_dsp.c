/* oir_dsp.c — mali LADSPA plugin za iRadio: HPF + peaking EQ + preamp.
 *
 * Kontrole:
 *   0: hpf_hz      (0 = iskljucen, inace 20..200 Hz, 12 dB/okt)
 *   1: peak_hz     (60..400 Hz, default 140)
 *   2: peak_db     (-12..+6 dB, default 0)
 *   3: preamp_db   (-12..0 dB, default 0)
 *   4: peak_q      (0.3..10, default 1.0)
 *
 * Ulaz/izlaz: stereo. Koristi se preko PulseAudio module-ladspa-sink.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ladspa.h>

#define PORT_HPF    0
#define PORT_PKF    1
#define PORT_PKG    2
#define PORT_PRE    3
#define PORT_Q      4
#define PORT_IN_L   5
#define PORT_IN_R   6
#define PORT_OUT_L  7
#define PORT_OUT_R  8

typedef struct {
    LADSPA_Data *hpf_hz, *peak_hz, *peak_db, *preamp_db, *peak_q;
    LADSPA_Data *in_l, *in_r, *out_l, *out_r;
    double fs;

    /* HPF biquad */
    double hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
    double hp_x1l, hp_x2l, hp_y1l, hp_y2l;
    double hp_x1r, hp_x2r, hp_y1r, hp_y2r;
    double hp_last;

    /* peaking biquad */
    double pk_b0, pk_b1, pk_b2, pk_a1, pk_a2;
    double pk_x1l, pk_x2l, pk_y1l, pk_y2l;
    double pk_x1r, pk_x2r, pk_y1r, pk_y2r;
    double pk_last_f, pk_last_g, pk_last_q;

    double pre_gain;
} OirDsp;

static void calc_hpf(OirDsp *p, double fc)
{
    if (fc < 5.0) { p->hp_b0 = 1.0; p->hp_b1 = p->hp_b2 = p->hp_a1 = p->hp_a2 = 0.0; return; }
    double w0 = 2.0 * M_PI * fc / p->fs;
    double cosw = cos(w0), sinw = sin(w0);
    double alpha = sinw / (2.0 * 0.7071);
    double b0 = (1.0 + cosw) / 2.0;
    double b1 = -(1.0 + cosw);
    double b2 = (1.0 + cosw) / 2.0;
    double a0 = 1.0 + alpha;
    p->hp_b0 = b0 / a0; p->hp_b1 = b1 / a0; p->hp_b2 = b2 / a0;
    p->hp_a1 = (-2.0 * cosw) / a0;
    p->hp_a2 = (1.0 - alpha) / a0;
}

static void calc_peak(OirDsp *p, double fc, double gain_db, double q)
{
    if (fabs(gain_db) < 0.01) { p->pk_b0 = 1.0; p->pk_b1 = p->pk_b2 = p->pk_a1 = p->pk_a2 = 0.0; return; }
    if (q < 0.1) q = 0.1;
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * fc / p->fs;
    double cosw = cos(w0), sinw = sin(w0);
    double alpha = sinw / (2.0 * q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosw;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    p->pk_b0 = b0 / a0; p->pk_b1 = b1 / a0; p->pk_b2 = b2 / a0;
    p->pk_a1 = (-2.0 * cosw) / a0;
    p->pk_a2 = (1.0 - alpha / A) / a0;
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    OirDsp *p = calloc(1, sizeof(OirDsp));
    if (!p) return NULL;
    p->fs = (double)rate;
    p->pre_gain = 1.0;
    p->hp_last = -1.0;
    p->pk_last_f = -1.0;
    p->pk_last_g = 0.0;
    p->pk_last_q = 1.0;
    return (LADSPA_Handle)p;
}

static void connect_port(LADSPA_Handle h, unsigned long port, LADSPA_Data *data)
{
    OirDsp *p = (OirDsp *)h;
    switch (port) {
        case PORT_HPF:   p->hpf_hz = data; break;
        case PORT_PKF:   p->peak_hz = data; break;
        case PORT_PKG:   p->peak_db = data; break;
        case PORT_PRE:   p->preamp_db = data; break;
        case PORT_Q:     p->peak_q = data; break;
        case PORT_IN_L:  p->in_l = data; break;
        case PORT_IN_R:  p->in_r = data; break;
        case PORT_OUT_L: p->out_l = data; break;
        case PORT_OUT_R: p->out_r = data; break;
    }
}

static void activate(LADSPA_Handle h)
{
    OirDsp *p = (OirDsp *)h;
    p->hp_x1l = p->hp_x2l = p->hp_y1l = p->hp_y2l = 0.0;
    p->hp_x1r = p->hp_x2r = p->hp_y1r = p->hp_y2r = 0.0;
    p->pk_x1l = p->pk_x2l = p->pk_y1l = p->pk_y2l = 0.0;
    p->pk_x1r = p->pk_x2r = p->pk_y1r = p->pk_y2r = 0.0;
}

static void run(LADSPA_Handle h, unsigned long n)
{
    OirDsp *p = (OirDsp *)h;
    double hpf = p->hpf_hz ? *p->hpf_hz : 0.0;
    double pkf = p->peak_hz ? *p->peak_hz : 140.0;
    double pkg = p->peak_db ? *p->peak_db : 0.0;
    double pre = p->preamp_db ? *p->preamp_db : 0.0;
    double pkq = p->peak_q ? *p->peak_q : 1.0;

    if (hpf != p->hp_last) { calc_hpf(p, hpf); p->hp_last = hpf; }
    if (pkf != p->pk_last_f || pkg != p->pk_last_g || pkq != p->pk_last_q) {
        calc_peak(p, pkf, pkg, pkq);
        p->pk_last_f = pkf;
        p->pk_last_g = pkg;
        p->pk_last_q = pkq;
    }
    p->pre_gain = pow(10.0, pre / 20.0);

    for (unsigned long i = 0; i < n; i++) {
        double l = p->in_l[i], r = p->in_r[i];

        double yl = p->hp_b0 * l + p->hp_b1 * p->hp_x1l + p->hp_b2 * p->hp_x2l
                    - p->hp_a1 * p->hp_y1l - p->hp_a2 * p->hp_y2l;
        p->hp_x2l = p->hp_x1l; p->hp_x1l = l;
        p->hp_y2l = p->hp_y1l; p->hp_y1l = yl;

        double yr = p->hp_b0 * r + p->hp_b1 * p->hp_x1r + p->hp_b2 * p->hp_x2r
                    - p->hp_a1 * p->hp_y1r - p->hp_a2 * p->hp_y2r;
        p->hp_x2r = p->hp_x1r; p->hp_x1r = r;
        p->hp_y2r = p->hp_y1r; p->hp_y1r = yr;

        /* peaking na HPF izlazu */
        double pl = p->pk_b0 * yl + p->pk_b1 * p->pk_x1l + p->pk_b2 * p->pk_x2l
                    - p->pk_a1 * p->pk_y1l - p->pk_a2 * p->pk_y2l;
        p->pk_x2l = p->pk_x1l; p->pk_x1l = yl;
        p->pk_y2l = p->pk_y1l; p->pk_y1l = pl;

        double pr = p->pk_b0 * yr + p->pk_b1 * p->pk_x1r + p->pk_b2 * p->pk_x2r
                    - p->pk_a1 * p->pk_y1r - p->pk_a2 * p->pk_y2r;
        p->pk_x2r = p->pk_x1r; p->pk_x1r = yr;
        p->pk_y2r = p->pk_y1r; p->pk_y1r = pr;

        p->out_l[i] = (LADSPA_Data)(pl * p->pre_gain);
        p->out_r[i] = (LADSPA_Data)(pr * p->pre_gain);
    }
}

static void cleanup(LADSPA_Handle h) { free(h); }

static const LADSPA_PortDescriptor ports_desc[] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO
};

static const char *ports_name[] = {
    "HPF frequency (Hz)", "Peak frequency (Hz)", "Peak gain (dB)",
    "Preamp (dB)", "Peak Q", "In L", "In R", "Out L", "Out R"
};

static const LADSPA_PortRangeHint ports_hint[] = {
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0,
      0.0f, 200.0f },
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE,
      60.0f, 400.0f },
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0,
      -12.0f, 6.0f },
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0,
      -12.0f, 0.0f },
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE,
      0.3f, 10.0f },
    { 0, 0.0f, 0.0f }, { 0, 0.0f, 0.0f }, { 0, 0.0f, 0.0f }, { 0, 0.0f, 0.0f }
};

static const LADSPA_Descriptor descriptor = {
    0x4f495200, "oir_dsp", LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "OIR DSP (HPF + peak EQ + preamp)", "iRadio", "None",
    9, ports_desc, ports_name, ports_hint,
    NULL, instantiate, connect_port, activate, run, NULL, NULL, NULL, cleanup
};

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    return (index == 0) ? &descriptor : NULL;
}
