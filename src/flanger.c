/*
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, write to the Free Software Foundation,
 * Fifth Floor, 51 Franklin Street, Boston, MA 02111-1301, USA.
 */

/* Effect: Stereo Flanger   (c) 2006 robs@users.sourceforge.net */

#define sox_flanger_usage \
  "[delay depth regen width speed shape phase interp]\n"
/*
  "                  .\n" \
  "                 /|regen\n" \
  "                / |\n" \
  "            +--(  |------------+\n" \
  "            |   \\ |            |   .\n" \
  "           _V_   \\|  _______   |   |\\ width   ___\n" \
  "          |   |   ' |       |  |   | \\       |   |\n" \
  "      +-->| + |---->| DELAY |--+-->|  )----->|   |\n" \
  "      |   |___|     |_______|      | /       |   |\n" \
  "      |           delay : depth    |/        |   |\n" \
  "  In  |                 : interp   '         |   | Out\n" \
  "  --->+               __:__                  | + |--->\n" \
  "      |              |     |speed            |   |\n" \
  "      |              |  ~  |shape            |   |\n" \
  "      |              |_____|phase            |   |\n" \
  "      +------------------------------------->|   |\n" \
  "                                             |___|\n" \
  "\n" \
  "       RANGE DEFAULT DESCRIPTION\n" \
  "delay   0 10    0    base delay in milliseconds\n" \
  "depth   0 10    2    added swept delay in milliseconds\n" \
  "regen -95 +95   0    percentage regeneration (delayed signal feedback)\n" \
  "width   0 100   71   percentage of delayed signal mixed with original\n" \
  "speed  0.1 10  0.5   sweeps per second (Hz) \n" \
  "shape    --    sin   swept wave shape: sine|triangle\n" \
  "phase   0 100   25   swept wave percentage phase-shift for multi-channel\n" \
  "                     (e.g. stereo) flange; 0 = 100 = same phase on each channel\n" \
  "interp   --    lin   delay-line interpolation: linear|quadratic"
*/

/* TODO: Slide in the delay at the start? */



#include "sox_i.h"
#include <math.h>
#include <string.h>



typedef enum {INTERP_LINEAR, INTERP_QUADRATIC} interp_t;

#define MAX_CHANNELS 4



typedef struct flanger {
  /* Parameters */
  double     delay_min;
  double     delay_depth;
  double     feedback_gain;
  double     delay_gain;
  double     speed;
  sox_wave_t  wave_shape;
  double     channel_phase;
  interp_t   interpolation;
            
  /* Delay buffers */
  double *   delay_bufs[MAX_CHANNELS];
  sox_size_t  delay_buf_length;
  sox_size_t  delay_buf_pos;
  double     delay_last[MAX_CHANNELS];
            
  /* Low Frequency Oscillator */
  float *    lfo;
  sox_size_t  lfo_length;
  sox_size_t  lfo_pos;
            
  /* Balancing */
  double     in_gain;
} * flanger_t;

assert_static(sizeof(struct flanger) <= SOX_MAX_EFFECT_PRIVSIZE,
              /* else */ flanger_PRIVSIZE_too_big);



static enum_item const interp_enum[] = {
  ENUM_ITEM(INTERP_,LINEAR)
  ENUM_ITEM(INTERP_,QUADRATIC)
  {0, 0}};



static int sox_flanger_getopts(sox_effect_t * effp, int argc, char *argv[])
{
  flanger_t p = (flanger_t) effp->priv;

  /* Set non-zero defaults: */
  p->delay_depth  = 2;
  p->delay_gain   = 71;
  p->speed        = 0.5;
  p->channel_phase= 25;

  do { /* break-able block */
    NUMERIC_PARAMETER(delay_min    , 0  , 10 )
    NUMERIC_PARAMETER(delay_depth  , 0  , 10 )
    NUMERIC_PARAMETER(feedback_gain,-95 , 95 )
    NUMERIC_PARAMETER(delay_gain   , 0  , 100)
    NUMERIC_PARAMETER(speed        , 0.1, 10 )
    TEXTUAL_PARAMETER(wave_shape, sox_wave_enum)
    NUMERIC_PARAMETER(channel_phase, 0  , 100)
    TEXTUAL_PARAMETER(interpolation, interp_enum)
  } while (0);

  if (argc != 0)
    return sox_usage(effp);

  sox_report("parameters:\n"
      "delay = %gms\n"
      "depth = %gms\n"
      "regen = %g%%\n"
      "width = %g%%\n"
      "speed = %gHz\n"
      "shape = %s\n"
      "phase = %g%%\n"
      "interp= %s",
      p->delay_min,
      p->delay_depth,
      p->feedback_gain,
      p->delay_gain,
      p->speed,
      sox_wave_enum[p->wave_shape].text,
      p->channel_phase,
      interp_enum[p->interpolation].text);

  return SOX_SUCCESS;
}



static int sox_flanger_start(sox_effect_t * effp)
{
  flanger_t f = (flanger_t) effp->priv;
  int c, channels = effp->ininfo.channels;

  if (channels > MAX_CHANNELS) {
    sox_fail("Can not operate with more than %i channels", MAX_CHANNELS);
    return SOX_EOF;
  }

  /* Scale percentages to unity: */
  f->feedback_gain /= 100;
  f->delay_gain    /= 100;
  f->channel_phase /= 100;

  /* Balance output: */
  f->in_gain = 1 / (1 + f->delay_gain);
  f->delay_gain  /= 1 + f->delay_gain;

  /* Balance feedback loop: */
  f->delay_gain *= 1 - fabs(f->feedback_gain);

  sox_debug("in_gain=%g feedback_gain=%g delay_gain=%g\n",
      f->in_gain, f->feedback_gain, f->delay_gain);

  /* Create the delay buffers, one for each channel: */
  f->delay_buf_length =
    (f->delay_min + f->delay_depth) / 1000 * effp->ininfo.rate + 0.5;
  ++f->delay_buf_length;  /* Need 0 to n, i.e. n + 1. */
  ++f->delay_buf_length;  /* Quadratic interpolator needs one more. */
  for (c = 0; c < channels; ++c)
    f->delay_bufs[c] = xcalloc(f->delay_buf_length, sizeof(*f->delay_bufs[0]));

  /* Create the LFO lookup table: */
  f->lfo_length = effp->ininfo.rate / f->speed;
  f->lfo = xcalloc(f->lfo_length, sizeof(*f->lfo));
  sox_generate_wave_table(
      f->wave_shape,
      SOX_FLOAT,
      f->lfo,
      f->lfo_length,
      (double)(sox_size_t)(f->delay_min / 1000 * effp->ininfo.rate + .5),
      (double)(f->delay_buf_length - 2),
      3 * M_PI_2);  /* Start the sweep at minimum delay (for mono at least) */

  sox_debug("delay_buf_length=%u lfo_length=%u\n",
      f->delay_buf_length, f->lfo_length);

  return SOX_SUCCESS;
}



static int sox_flanger_flow(sox_effect_t * effp, sox_ssample_t const * ibuf,
    sox_ssample_t * obuf, sox_size_t * isamp, sox_size_t * osamp)
{
  flanger_t f = (flanger_t) effp->priv;
  int c, channels = effp->ininfo.channels;
  sox_size_t len = (*isamp > *osamp ? *osamp : *isamp) / channels;

  *isamp = *osamp = len * channels;

  while (len--) {
    f->delay_buf_pos =
      (f->delay_buf_pos + f->delay_buf_length - 1) % f->delay_buf_length;
    for (c = 0; c < channels; ++c) {
      double delayed_0, delayed_1;
      double delayed;
      double in, out;
      sox_size_t channel_phase = c * f->lfo_length * f->channel_phase + .5;
      double delay = f->lfo[(f->lfo_pos + channel_phase) % f->lfo_length];
      double frac_delay = modf(delay, &delay);
      sox_size_t int_delay = (size_t)delay;

      in = *ibuf++;
      f->delay_bufs[c][f->delay_buf_pos] = in + f->delay_last[c] * f->feedback_gain;

      delayed_0 = f->delay_bufs[c]
        [(f->delay_buf_pos + int_delay++) % f->delay_buf_length];
      delayed_1 = f->delay_bufs[c]
        [(f->delay_buf_pos + int_delay++) % f->delay_buf_length];

      if (f->interpolation == INTERP_LINEAR)
        delayed = delayed_0 + (delayed_1 - delayed_0) * frac_delay;
      else /* if (f->interpolation == INTERP_QUADRATIC) */
      {
        double a, b;
        double delayed_2 = f->delay_bufs[c]
          [(f->delay_buf_pos + int_delay++) % f->delay_buf_length];
        delayed_2 -= delayed_0;
        delayed_1 -= delayed_0;
        a = delayed_2 *.5 - delayed_1;
        b = delayed_1 * 2 - delayed_2 *.5;
        delayed = delayed_0 + (a * frac_delay + b) * frac_delay;
      }

      f->delay_last[c] = delayed;
      out = in * f->in_gain + delayed * f->delay_gain;
      *obuf++ = SOX_ROUND_CLIP_COUNT(out, effp->clips);
    }
    f->lfo_pos = (f->lfo_pos + 1) % f->lfo_length;
  }

  return SOX_SUCCESS;
}



static int sox_flanger_stop(sox_effect_t * effp)
{
  flanger_t f = (flanger_t) effp->priv;
  int c, channels = effp->ininfo.channels;

  for (c = 0; c < channels; ++c)
    free(f->delay_bufs[c]);

  free(f->lfo);

  memset(f, 0, sizeof(*f));

  return SOX_SUCCESS;
}



static sox_effect_handler_t sox_flanger_effect = {
  "flanger",
  sox_flanger_usage,
  SOX_EFF_MCHAN,
  sox_flanger_getopts,
  sox_flanger_start,
  sox_flanger_flow,
  NULL,
  sox_flanger_stop,
  NULL
};



sox_effect_handler_t const * sox_flanger_effect_fn(void)
{
  return &sox_flanger_effect;
}
