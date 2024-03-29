/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/**
 * SECTION:rtsp-sdp
 * @short_description: Make SDP messages
 * @see_also: #GstRTSPMedia
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include <string.h>

#include <gst/sdp/gstmikey.h>

#include "rtsp-sdp.h"

static gboolean
get_info_from_tags (GstPad * pad, GstEvent ** event, gpointer user_data)
{
  GstSDPMedia *media = (GstSDPMedia *) user_data;

  if (GST_EVENT_TYPE (*event) == GST_EVENT_TAG) {
    GstTagList *tags;
    guint bitrate = 0;

    gst_event_parse_tag (*event, &tags);

    if (gst_tag_list_get_scope (tags) != GST_TAG_SCOPE_STREAM)
      return TRUE;

    if (!gst_tag_list_get_uint (tags, GST_TAG_MAXIMUM_BITRATE,
            &bitrate) || bitrate == 0)
      if (!gst_tag_list_get_uint (tags, GST_TAG_BITRATE, &bitrate) ||
          bitrate == 0)
        return TRUE;

    /* set bandwidth (kbits/s) */
    gst_sdp_media_add_bandwidth (media, GST_SDP_BWTYPE_AS, bitrate / 1000);

    return FALSE;

  }

  return TRUE;
}

static void
update_sdp_from_tags (GstRTSPStream * stream, GstSDPMedia * stream_media)
{
  GstPad *src_pad;

  src_pad = gst_rtsp_stream_get_srcpad (stream);

  gst_pad_sticky_events_foreach (src_pad, get_info_from_tags, stream_media);

  gst_object_unref (src_pad);
}

static void
make_media (GstSDPMessage * sdp, GstSDPInfo * info,
    GstRTSPStream * stream, GstCaps * caps, GstRTSPProfile profile)
{
  GstSDPMedia *smedia;
  gchar *tmp;
  GstRTSPLowerTrans ltrans;
  GSocketFamily family;
  const gchar *addrtype, *proto;
  gchar *address;
  guint ttl;
  GstClockTime rtx_time;
  gchar *base64;
  guint32 ssrc;
  GstMIKEYMessage *mikey_msg;

  gst_sdp_media_new (&smedia);

  if (gst_sdp_media_set_media_from_caps (caps, smedia) != GST_SDP_OK) {
    goto error;
  }

  gst_sdp_media_set_port_info (smedia, 0, 1);

  switch (profile) {
    case GST_RTSP_PROFILE_AVP:
      proto = "RTP/AVP";
      break;
    case GST_RTSP_PROFILE_AVPF:
      proto = "RTP/AVPF";
      break;
    case GST_RTSP_PROFILE_SAVP:
      proto = "RTP/SAVP";
      break;
    case GST_RTSP_PROFILE_SAVPF:
      proto = "RTP/SAVPF";
      break;
    default:
      proto = "udp";
      break;
  }
  gst_sdp_media_set_proto (smedia, proto);

  if (info->is_ipv6) {
    addrtype = "IP6";
    family = G_SOCKET_FAMILY_IPV6;
  } else {
    addrtype = "IP4";
    family = G_SOCKET_FAMILY_IPV4;
  }

  ltrans = gst_rtsp_stream_get_protocols (stream);
  if (ltrans == GST_RTSP_LOWER_TRANS_UDP_MCAST) {
    GstRTSPAddress *addr;

    addr = gst_rtsp_stream_get_multicast_address (stream, family);
    if (addr == NULL)
      goto no_multicast;

    address = g_strdup (addr->address);
    ttl = addr->ttl;
    gst_rtsp_address_free (addr);
  } else {
    ttl = 16;
    if (info->is_ipv6)
      address = g_strdup ("::");
    else
      address = g_strdup ("0.0.0.0");
  }

  /* for the c= line */
  gst_sdp_media_add_connection (smedia, "IN", addrtype, address, ttl, 1);
  g_free (address);

  /* the config uri */
  tmp = gst_rtsp_stream_get_control (stream);
  gst_sdp_media_add_attribute (smedia, "control", tmp);
  g_free (tmp);

  /* check for srtp */
  mikey_msg = gst_mikey_message_new_from_caps (caps);
  if (mikey_msg) {
    gst_rtsp_stream_get_ssrc (stream, &ssrc);
    /* add policy '0' for our SSRC */
    gst_mikey_message_add_cs_srtp (mikey_msg, 0, ssrc, 0);

    base64 = gst_mikey_message_base64_encode (mikey_msg);
    if (base64) {
      tmp = g_strdup_printf ("mikey %s", base64);
      g_free (base64);
      gst_sdp_media_add_attribute (smedia, "key-mgmt", tmp);
      g_free (tmp);
    }

    gst_mikey_message_unref (mikey_msg);
  }

  update_sdp_from_tags (stream, smedia);

  if ((profile == GST_RTSP_PROFILE_AVPF || profile == GST_RTSP_PROFILE_SAVPF)
      && (rtx_time = gst_rtsp_stream_get_retransmission_time (stream))) {
    /* ssrc multiplexed retransmit functionality */
    guint rtx_pt = gst_rtsp_stream_get_retransmission_pt (stream);

    if (rtx_pt == 0) {
      g_warning ("failed to find an available dynamic payload type. "
          "Not adding retransmission");
    } else {
      gchar *tmp;
      GstStructure *s;
      gint caps_pt, caps_rate;

      s = gst_caps_get_structure (caps, 0);
      if (s == NULL)
        goto error;

      /* get payload type and clock rate */
      gst_structure_get_int (s, "payload", &caps_pt);
      gst_structure_get_int (s, "clock-rate", &caps_rate);

      tmp = g_strdup_printf ("%d", rtx_pt);
      gst_sdp_media_add_format (smedia, tmp);
      g_free (tmp);

      tmp = g_strdup_printf ("%d rtx/%d", rtx_pt, caps_rate);
      gst_sdp_media_add_attribute (smedia, "rtpmap", tmp);
      g_free (tmp);

      tmp =
          g_strdup_printf ("%d apt=%d;rtx-time=%" G_GINT64_FORMAT, rtx_pt,
          caps_pt, GST_TIME_AS_MSECONDS (rtx_time));
      gst_sdp_media_add_attribute (smedia, "fmtp", tmp);
      g_free (tmp);
    }
  }

  gst_sdp_message_add_media (sdp, smedia);
  gst_sdp_media_free (smedia);

  return;

  /* ERRORS */
no_multicast:
  {
    gst_sdp_media_free (smedia);
    g_warning ("ignoring stream %d without multicast address",
        gst_rtsp_stream_get_index (stream));
    return;
  }
error:
  {
    gst_sdp_media_free (smedia);
    g_warning ("ignoring stream %d", gst_rtsp_stream_get_index (stream));
    return;
  }
}

/**
 * gst_rtsp_sdp_from_media:
 * @sdp: a #GstSDPMessage
 * @info: (transfer none): a #GstSDPInfo
 * @media: (transfer none): a #GstRTSPMedia
 *
 * Add @media specific info to @sdp. @info is used to configure the connection
 * information in the SDP.
 *
 * Returns: TRUE on success.
 */
gboolean
gst_rtsp_sdp_from_media (GstSDPMessage * sdp, GstSDPInfo * info,
    GstRTSPMedia * media)
{
  guint i, n_streams;
  gchar *rangestr;

  n_streams = gst_rtsp_media_n_streams (media);

  rangestr = gst_rtsp_media_get_range_string (media, FALSE, GST_RTSP_RANGE_NPT);
  if (rangestr == NULL)
    goto not_prepared;

  gst_sdp_message_add_attribute (sdp, "range", rangestr);
  g_free (rangestr);

  for (i = 0; i < n_streams; i++) {
    GstRTSPStream *stream;

    stream = gst_rtsp_media_get_stream (media, i);
    gst_rtsp_sdp_from_stream (sdp, info, stream);
  }

  {
    GstNetTimeProvider *provider;

    if ((provider =
            gst_rtsp_media_get_time_provider (media, info->server_ip, 0))) {
      GstClock *clock;
      gchar *address, *str;
      gint port;

      g_object_get (provider, "clock", &clock, "address", &address, "port",
          &port, NULL);

      str = g_strdup_printf ("GstNetTimeProvider %s %s:%d %" G_GUINT64_FORMAT,
          g_type_name (G_TYPE_FROM_INSTANCE (clock)), address, port,
          gst_clock_get_time (clock));

      gst_sdp_message_add_attribute (sdp, "x-gst-clock", str);
      g_free (str);
      gst_object_unref (clock);
      g_free (address);
      gst_object_unref (provider);
    }
  }

  return TRUE;

  /* ERRORS */
not_prepared:
  {
    GST_ERROR ("media %p is not prepared", media);
    return FALSE;
  }
}

/**
 * gst_rtsp_sdp_from_stream:
 * @sdp: a #GstSDPMessage
 * @info: (transfer none): a #GstSDPInfo
 * @stream: (transfer none): a #GstRTSPStream
 *
 * Add info from @stream to @sdp.
 *
 */
void
gst_rtsp_sdp_from_stream (GstSDPMessage * sdp, GstSDPInfo * info,
    GstRTSPStream * stream)
{
  GstCaps *caps;
  GstRTSPProfile profiles;
  guint mask;

  caps = gst_rtsp_stream_get_caps (stream);

  if (caps == NULL) {
    g_warning ("ignoring stream without caps");
    return;
  }

  /* make a new media for each profile */
  profiles = gst_rtsp_stream_get_profiles (stream);
  mask = 1;
  while (profiles >= mask) {
    GstRTSPProfile prof = profiles & mask;

    if (prof)
      make_media (sdp, info, stream, caps, prof);

    mask <<= 1;
  }
  gst_caps_unref (caps);
}
