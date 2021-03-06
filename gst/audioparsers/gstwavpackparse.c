/* GStreamer Wavpack parser
 * Copyright (C) 2012 Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>
 * Copyright (C) 2012 Nokia Corporation. All rights reserved.
 *   Contact: Stefan Kost <stefan.kost@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/**
 * SECTION:element-wavpackparse
 * @short_description: Wavpack parser
 * @see_also: #GstAmrParse, #GstAACParse
 *
 * This is an Wavpack parser.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch filesrc location=abc.wavpack ! wavpackparse ! wavpackdec ! audioresample ! audioconvert ! autoaudiosink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstwavpackparse.h"

#include <gst/base/gstbytereader.h>
#include <gst/audio/multichannel.h>

GST_DEBUG_CATEGORY_STATIC (wavpack_parse_debug);
#define GST_CAT_DEFAULT wavpack_parse_debug

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-wavpack, "
        "width = (int) [ 1, 32 ], "
        "channels = (int) [ 1, 8 ], "
        "rate = (int) [ 6000, 192000 ], " "framed = (boolean) TRUE; "
        "audio/x-wavpack-correction, " "framed = (boolean) TRUE")
    );

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-wavpack"));

static void gst_wavpack_parse_finalize (GObject * object);

static gboolean gst_wavpack_parse_start (GstBaseParse * parse);
static gboolean gst_wavpack_parse_stop (GstBaseParse * parse);
static gboolean gst_wavpack_parse_check_valid_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, guint * size, gint * skipsize);
static GstFlowReturn gst_wavpack_parse_parse_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame);
static GstCaps *gst_wavpack_parse_get_sink_caps (GstBaseParse * parse);

/* FIXME remove when all properly renamed */
typedef GstWavpackParse GstWavpackParse2;
typedef GstWavpackParseClass GstWavpackParse2Class;

GST_BOILERPLATE (GstWavpackParse2, gst_wavpack_parse, GstBaseParse,
    GST_TYPE_BASE_PARSE);

static void
gst_wavpack_parse_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  gst_element_class_set_details_simple (element_class,
      "Wavpack audio stream parser", "Codec/Parser/Audio",
      "Wavpack parser", "Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>");
}

static void
gst_wavpack_parse_class_init (GstWavpackParseClass * klass)
{
  GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (wavpack_parse_debug, "wavpackparse", 0,
      "Wavpack audio stream parser");

  object_class->finalize = gst_wavpack_parse_finalize;

  parse_class->start = GST_DEBUG_FUNCPTR (gst_wavpack_parse_start);
  parse_class->stop = GST_DEBUG_FUNCPTR (gst_wavpack_parse_stop);
  parse_class->check_valid_frame =
      GST_DEBUG_FUNCPTR (gst_wavpack_parse_check_valid_frame);
  parse_class->parse_frame = GST_DEBUG_FUNCPTR (gst_wavpack_parse_parse_frame);
  parse_class->get_sink_caps =
      GST_DEBUG_FUNCPTR (gst_wavpack_parse_get_sink_caps);
}

static void
gst_wavpack_parse_reset (GstWavpackParse * wvparse)
{
  wvparse->channels = -1;
  wvparse->channel_mask = 0;
  wvparse->sample_rate = -1;
  wvparse->width = -1;
  wvparse->total_samples = 0;
}

static void
gst_wavpack_parse_init (GstWavpackParse * wvparse, GstWavpackParseClass * klass)
{
  gst_wavpack_parse_reset (wvparse);
}

static void
gst_wavpack_parse_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_wavpack_parse_start (GstBaseParse * parse)
{
  GstWavpackParse *wvparse = GST_WAVPACK_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "starting");

  gst_wavpack_parse_reset (wvparse);

  /* need header at least */
  gst_base_parse_set_min_frame_size (GST_BASE_PARSE (wvparse),
      sizeof (WavpackHeader));

  /* inform baseclass we can come up with ts, based on counters in packets */
  gst_base_parse_set_has_timing_info (GST_BASE_PARSE_CAST (wvparse), TRUE);
  gst_base_parse_set_syncable (GST_BASE_PARSE_CAST (wvparse), TRUE);

  return TRUE;
}

static gboolean
gst_wavpack_parse_stop (GstBaseParse * parse)
{
  GST_DEBUG_OBJECT (parse, "stopping");

  return TRUE;
}

static gint
gst_wavpack_get_default_channel_mask (gint nchannels)
{
  gint channel_mask = 0;

  /* Set the default channel mask for the given number of channels.
   * It's the same as for WAVE_FORMAT_EXTENDED:
   * http://www.microsoft.com/whdc/device/audio/multichaud.mspx
   */
  switch (nchannels) {
    case 11:
      channel_mask |= 0x00400;
      channel_mask |= 0x00200;
    case 9:
      channel_mask |= 0x00100;
    case 8:
      channel_mask |= 0x00080;
      channel_mask |= 0x00040;
    case 6:
      channel_mask |= 0x00020;
      channel_mask |= 0x00010;
    case 4:
      channel_mask |= 0x00008;
    case 3:
      channel_mask |= 0x00004;
    case 2:
      channel_mask |= 0x00002;
      channel_mask |= 0x00001;
      break;
    case 1:
      /* For mono use front center */
      channel_mask |= 0x00004;
      break;
  }

  return channel_mask;
}

static const struct
{
  const guint32 ms_mask;
  const GstAudioChannelPosition gst_pos;
} layout_mapping[] = {
  {
  0x00001, GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT}, {
  0x00002, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT}, {
  0x00004, GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER}, {
  0x00008, GST_AUDIO_CHANNEL_POSITION_LFE}, {
  0x00010, GST_AUDIO_CHANNEL_POSITION_REAR_LEFT}, {
  0x00020, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT}, {
  0x00040, GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER}, {
  0x00080, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER}, {
  0x00100, GST_AUDIO_CHANNEL_POSITION_REAR_CENTER}, {
  0x00200, GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT}, {
  0x00400, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT}, {
  0x00800, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_CENTER       */
  {
  0x01000, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_FRONT_LEFT   */
  {
  0x02000, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_FRONT_CENTER */
  {
  0x04000, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_FRONT_RIGHT  */
  {
  0x08000, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_BACK_LEFT    */
  {
  0x10000, GST_AUDIO_CHANNEL_POSITION_INVALID}, /* TOP_BACK_CENTER  */
  {
  0x20000, GST_AUDIO_CHANNEL_POSITION_INVALID}  /* TOP_BACK_RIGHT   */
};

#define MAX_CHANNEL_POSITIONS G_N_ELEMENTS (layout_mapping)

static gboolean
gst_wavpack_set_channel_layout (GstCaps * caps, gint layout)
{
  GstAudioChannelPosition pos[MAX_CHANNEL_POSITIONS];
  GstStructure *s;
  gint num_channels, i, p;

  s = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (s, "channels", &num_channels))
    g_return_val_if_reached (FALSE);

  if (num_channels == 1 && layout == 0x00004) {
    pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_MONO;
    gst_audio_set_channel_positions (s, pos);
    return TRUE;
  }

  p = 0;
  for (i = 0; i < MAX_CHANNEL_POSITIONS; ++i) {
    if ((layout & layout_mapping[i].ms_mask) != 0) {
      if (p >= num_channels) {
        GST_WARNING ("More bits set in the channel layout map than there "
            "are channels! Broken file");
        return FALSE;
      }
      if (layout_mapping[i].gst_pos == GST_AUDIO_CHANNEL_POSITION_INVALID) {
        GST_WARNING ("Unsupported channel position (mask 0x%08x) in channel "
            "layout map - ignoring those channels", layout_mapping[i].ms_mask);
        /* what to do? just ignore it and let downstream deal with a channel
         * layout that has INVALID positions in it for now ... */
      }
      pos[p] = layout_mapping[i].gst_pos;
      ++p;
    }
  }

  if (p != num_channels) {
    GST_WARNING ("Only %d bits set in the channel layout map, but there are "
        "supposed to be %d channels! Broken file", p, num_channels);
    return FALSE;
  }

  gst_audio_set_channel_positions (s, pos);
  return TRUE;
}

static const guint32 sample_rates[] = {
  6000, 8000, 9600, 11025, 12000, 16000, 22050,
  24000, 32000, 44100, 48000, 64000, 88200, 96000, 192000
};

#define CHECK(call) { \
  if (!call) \
    goto read_failed; \
}

/* caller ensures properly sync'ed with enough data */
static gboolean
gst_wavpack_parse_frame_metadata (GstWavpackParse * parse, GstBuffer * buf,
    gint skip, WavpackHeader * wph, WavpackInfo * wpi)
{
  GstByteReader br;
  gint i;

  g_return_val_if_fail (wph != NULL || wpi != NULL, FALSE);
  g_return_val_if_fail (GST_BUFFER_SIZE (buf) >= skip + sizeof (WavpackHeader),
      FALSE);

  gst_byte_reader_init (&br, GST_BUFFER_DATA (buf) + skip, wph->ckSize + 8);
  /* skip past header */
  gst_byte_reader_skip_unchecked (&br, sizeof (WavpackHeader));

  /* get some basics from header */
  i = (wph->flags >> 23) & 0xF;
  if (!wpi->rate)
    wpi->rate = (i < G_N_ELEMENTS (sample_rates)) ? sample_rates[i] : 44100;
  wpi->width = ((wph->flags & 0x3) + 1) * 8;
  if (!wpi->channels)
    wpi->channels = (wph->flags & 0x4) ? 1 : 2;
  if (!wpi->channel_mask)
    wpi->channel_mask = 5 - wpi->channels;

  /* need to dig metadata blocks for some more */
  while (gst_byte_reader_get_remaining (&br)) {
    gint size = 0;
    guint16 size2 = 0;
    guint8 c, id;
    const guint8 *data;
    GstByteReader mbr;

    CHECK (gst_byte_reader_get_uint8 (&br, &id));
    CHECK (gst_byte_reader_get_uint8 (&br, &c));
    if (id & ID_LARGE)
      CHECK (gst_byte_reader_get_uint16_le (&br, &size2));
    size = size2;
    size <<= 8;
    size += c;
    size <<= 1;
    if (id & ID_ODD_SIZE)
      size--;

    CHECK (gst_byte_reader_get_data (&br, size + (size & 1), &data));
    gst_byte_reader_init (&mbr, data, size);

    switch (id) {
      case ID_WVC_BITSTREAM:
        GST_LOG_OBJECT (parse, "correction bitstream");
        wpi->correction = TRUE;
        break;
      case ID_WV_BITSTREAM:
      case ID_WVX_BITSTREAM:
        break;
      case ID_SAMPLE_RATE:
        if (size == 3) {
          CHECK (gst_byte_reader_get_uint24_le (&mbr, &wpi->rate));
          GST_LOG_OBJECT (parse, "updated with custom rate %d", wpi->rate);
        } else {
          GST_DEBUG_OBJECT (parse, "unexpected size for SAMPLE_RATE metadata");
        }
        break;
      case ID_CHANNEL_INFO:
      {
        guint16 channels;
        guint32 mask = 0;

        if (size == 6) {
          CHECK (gst_byte_reader_get_uint16_le (&mbr, &channels));
          channels = channels & 0xFFF;
          CHECK (gst_byte_reader_get_uint24_le (&mbr, &mask));
        } else if (size) {
          CHECK (gst_byte_reader_get_uint8 (&mbr, &c));
          channels = c;
          while (gst_byte_reader_get_uint8 (&mbr, &c))
            mask |= (((guint32) c) << 8);
        } else {
          GST_DEBUG_OBJECT (parse, "unexpected size for CHANNEL_INFO metadata");
          break;
        }
        wpi->channels = channels;
        wpi->channel_mask = mask;
        break;
      }
      default:
        GST_LOG_OBJECT (parse, "unparsed ID 0x%x", id);
        break;
    }
  }

  return TRUE;

  /* ERRORS */
read_failed:
  {
    GST_DEBUG_OBJECT (parse, "short read while parsing metadata");
    /* let's look the other way anyway */
    return TRUE;
  }
}

/* caller ensures properly sync'ed with enough data */
static gboolean
gst_wavpack_parse_frame_header (GstWavpackParse * parse, GstBuffer * buf,
    gint skip, WavpackHeader * _wph)
{
  GstByteReader br = GST_BYTE_READER_INIT_FROM_BUFFER (buf);
  WavpackHeader wph = { {0,}, 0, };

  g_return_val_if_fail (GST_BUFFER_SIZE (buf) >= skip + sizeof (WavpackHeader),
      FALSE);

  /* marker */
  gst_byte_reader_skip_unchecked (&br, skip + 4);

  /* read */
  gst_byte_reader_get_uint32_le (&br, &wph.ckSize);
  gst_byte_reader_get_uint16_le (&br, &wph.version);
  gst_byte_reader_get_uint8 (&br, &wph.track_no);
  gst_byte_reader_get_uint8 (&br, &wph.index_no);
  gst_byte_reader_get_uint32_le (&br, &wph.total_samples);
  gst_byte_reader_get_uint32_le (&br, &wph.block_index);
  gst_byte_reader_get_uint32_le (&br, &wph.block_samples);
  gst_byte_reader_get_uint32_le (&br, &wph.flags);
  gst_byte_reader_get_uint32_le (&br, &wph.crc);

  /* dump */
  GST_LOG_OBJECT (parse, "size %d", wph.ckSize);
  GST_LOG_OBJECT (parse, "version 0x%x", wph.version);
  GST_LOG_OBJECT (parse, "total samples %d", wph.total_samples);
  GST_LOG_OBJECT (parse, "block index %d", wph.block_index);
  GST_LOG_OBJECT (parse, "block samples %d", wph.block_samples);
  GST_LOG_OBJECT (parse, "flags 0x%x", wph.flags);
  GST_LOG_OBJECT (parse, "crc 0x%x", wph.flags);

  if (!parse->total_samples && wph.block_index == 0 && wph.total_samples != -1) {
    GST_DEBUG_OBJECT (parse, "determined duration of %u samples",
        wph.total_samples);
    parse->total_samples = wph.total_samples;
  }

  if (_wph)
    *_wph = wph;

  return TRUE;
}

static gboolean
gst_wavpack_parse_check_valid_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, guint * framesize, gint * skipsize)
{
  GstWavpackParse *wvparse = GST_WAVPACK_PARSE (parse);
  GstBuffer *buf = frame->buffer;
  GstByteReader reader = GST_BYTE_READER_INIT_FROM_BUFFER (buf);
  gint off;
  gboolean lost_sync, draining, final;
  guint frmsize = 0;
  WavpackHeader wph;
  WavpackInfo wpi = { 0, };

  if (G_UNLIKELY (GST_BUFFER_SIZE (buf) < sizeof (WavpackHeader)))
    return FALSE;

  /* scan for 'wvpk' marker */
  off = gst_byte_reader_masked_scan_uint32 (&reader, 0xffffffff, 0x7776706b,
      0, GST_BUFFER_SIZE (buf));

  GST_LOG_OBJECT (parse, "possible sync at buffer offset %d", off);

  /* didn't find anything that looks like a sync word, skip */
  if (off < 0) {
    *skipsize = GST_BUFFER_SIZE (buf) - 3;
    goto skip;
  }

  /* possible frame header, but not at offset 0? skip bytes before sync */
  if (off > 0) {
    *skipsize = off;
    goto skip;
  }

  /* make sure the values in the frame header look sane */
  gst_wavpack_parse_frame_header (wvparse, buf, 0, &wph);
  frmsize = wph.ckSize + 8;

  /* need the entire frame for parsing */
  if (gst_byte_reader_get_remaining (&reader) < frmsize)
    goto more;

  /* got a frame, now we can dig for some more metadata */
  GST_LOG_OBJECT (parse, "got frame");
  gst_wavpack_parse_frame_metadata (wvparse, buf, 0, &wph, &wpi);

  lost_sync = GST_BASE_PARSE_LOST_SYNC (parse);
  draining = GST_BASE_PARSE_DRAINING (parse);

  while (!(final = (wph.flags & FLAG_FINAL_BLOCK)) || (lost_sync && !draining)) {
    guint32 word = 0;

    GST_LOG_OBJECT (wvparse, "checking next frame syncword; "
        "lost_sync: %d, draining: %d, final: %d", lost_sync, draining, final);

    if (!gst_byte_reader_skip (&reader, wph.ckSize + 8) ||
        !gst_byte_reader_peek_uint32_be (&reader, &word)) {
      GST_DEBUG_OBJECT (wvparse, "... but not sufficient data");
      frmsize += 4;
      goto more;
    } else {
      if (word != 0x7776706b) {
        GST_DEBUG_OBJECT (wvparse, "0x%x not OK", word);
        *skipsize = off + 2;
        goto skip;
      }
      /* need to parse each frame/block for metadata if several ones */
      if (!final) {
        gint av;

        GST_LOG_OBJECT (wvparse, "checking frame at offset %d (0x%x)",
            frmsize, frmsize);
        av = gst_byte_reader_get_remaining (&reader);
        if (av < sizeof (WavpackHeader)) {
          frmsize += sizeof (WavpackHeader);
          goto more;
        }
        gst_wavpack_parse_frame_header (wvparse, buf, frmsize, &wph);
        off = frmsize;
        frmsize += wph.ckSize + 8;
        if (av < wph.ckSize + 8)
          goto more;
        gst_wavpack_parse_frame_metadata (wvparse, buf, off, &wph, &wpi);
        /* could also check for matching block_index and block_samples ?? */
      }
    }

    /* resynced if we make it here */
    lost_sync = FALSE;
  }

  /* found frame (up to final), record gathered metadata */
  wvparse->wpi = wpi;
  wvparse->wph = wph;

  *framesize = frmsize;
  gst_base_parse_set_min_frame_size (parse, sizeof (WavpackHeader));

  return TRUE;

skip:
  GST_LOG_OBJECT (wvparse, "skipping %d", *skipsize);
  return FALSE;

more:
  GST_LOG_OBJECT (wvparse, "need at least %u", frmsize);
  gst_base_parse_set_min_frame_size (parse, frmsize);
  *skipsize = 0;
  return FALSE;
}

static GstFlowReturn
gst_wavpack_parse_parse_frame (GstBaseParse * parse, GstBaseParseFrame * frame)
{
  GstWavpackParse *wvparse = GST_WAVPACK_PARSE (parse);
  GstBuffer *buf = frame->buffer;
  guint rate, chans, width, mask;

  /* re-use previously parsed data */
  rate = wvparse->wpi.rate;
  width = wvparse->wpi.width;
  chans = wvparse->wpi.channels;
  mask = wvparse->wpi.channel_mask;

  GST_LOG_OBJECT (parse, "rate: %u, width: %u, chans: %u", rate, width, chans);

  GST_BUFFER_TIMESTAMP (buf) =
      gst_util_uint64_scale_int (wvparse->wph.block_index, GST_SECOND, rate);
  GST_BUFFER_DURATION (buf) =
      gst_util_uint64_scale_int (wvparse->wph.block_index +
      wvparse->wph.block_samples, GST_SECOND, rate) -
      GST_BUFFER_TIMESTAMP (buf);

  if (G_UNLIKELY (wvparse->sample_rate != rate || wvparse->channels != chans
          || wvparse->width != width || wvparse->channel_mask != mask)) {
    GstCaps *caps;

    if (wvparse->wpi.correction) {
      caps = gst_caps_new_simple ("audio/x-wavpack-correction",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
    } else {
      caps = gst_caps_new_simple ("audio/x-wavpack",
          "channels", G_TYPE_INT, chans,
          "rate", G_TYPE_INT, rate,
          "width", G_TYPE_INT, width, "framed", G_TYPE_BOOLEAN, TRUE, NULL);

      if (!mask)
        mask = gst_wavpack_get_default_channel_mask (wvparse->channels);
      if (mask != 0) {
        if (!gst_wavpack_set_channel_layout (caps, mask)) {
          GST_WARNING_OBJECT (wvparse, "Failed to set channel layout");
        }
      }
    }

    gst_buffer_set_caps (buf, caps);
    gst_pad_set_caps (GST_BASE_PARSE_SRC_PAD (parse), caps);
    gst_caps_unref (caps);

    wvparse->sample_rate = rate;
    wvparse->channels = chans;
    wvparse->width = width;
    wvparse->channel_mask = mask;

    if (wvparse->total_samples) {
      GST_DEBUG_OBJECT (wvparse, "setting duration");
      gst_base_parse_set_duration (GST_BASE_PARSE (wvparse),
          GST_FORMAT_TIME, gst_util_uint64_scale_int (wvparse->total_samples,
              GST_SECOND, wvparse->sample_rate), 0);
    }
  }

  return GST_FLOW_OK;
}

static GstCaps *
gst_wavpack_parse_get_sink_caps (GstBaseParse * parse)
{
  GstCaps *peercaps;
  GstCaps *res;

  peercaps = gst_pad_get_allowed_caps (GST_BASE_PARSE_SRC_PAD (parse));
  if (peercaps) {
    guint i, n;

    /* Remove the framed field */
    peercaps = gst_caps_make_writable (peercaps);
    n = gst_caps_get_size (peercaps);
    for (i = 0; i < n; i++) {
      GstStructure *s = gst_caps_get_structure (peercaps, i);

      gst_structure_remove_field (s, "framed");
    }

    res =
        gst_caps_intersect_full (peercaps,
        gst_pad_get_pad_template_caps (GST_BASE_PARSE_SINK_PAD (parse)),
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (peercaps);
  } else {
    res =
        gst_caps_copy (gst_pad_get_pad_template_caps (GST_BASE_PARSE_SINK_PAD
            (parse)));
  }

  return res;
}
