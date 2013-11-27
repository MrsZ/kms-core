/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#include "kmstestutils.h"

#define GST_CAT_DEFAULT kms_utils
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "kms_utils"

#define KEY_DATA_PROPERTY "kms-test-utils-prop"

struct tmp_data
{
  gchar *src_pad_name;
  GstElement *sink;
  gchar *sink_pad_name;
  gulong handler;
};

static void
destroy_tmp_data (gpointer data, GClosure * closure)
{
  struct tmp_data *tmp = data;

  if (tmp->sink != NULL)
    gst_object_unref (tmp->sink);

  if (tmp->src_pad_name != NULL)
    g_free (tmp->src_pad_name);

  if (tmp->sink_pad_name != NULL)
    g_free (tmp->sink_pad_name);

  g_slice_free (struct tmp_data, tmp);
}

static struct tmp_data *
create_tmp_data (const gchar * src_pad_name, GstElement * sink,
    const gchar * sink_pad_name)
{
  struct tmp_data *tmp;

  tmp = g_slice_new0 (struct tmp_data);

  tmp->src_pad_name = g_strdup (src_pad_name);
  tmp->sink = gst_object_ref (sink);
  tmp->sink_pad_name = g_strdup (sink_pad_name);
  tmp->handler = 0L;

  return tmp;
}

static void
connect_to_sink (GstElement * sink, const gchar * sinkname, GstPad * srcpad)
{
  GstPad *sinkpad;

  GST_DEBUG ("Getting pad %s from %" GST_PTR_FORMAT, sinkname, sink);
  sinkpad = gst_element_get_static_pad (sink, sinkname);

  if (sinkpad == NULL)
    sinkpad = gst_element_get_request_pad (sink, sinkname);

  if (sinkpad == NULL) {
    GST_ERROR ("Can not get sink pad.");
    return;
  }

  if (gst_pad_is_linked (sinkpad)) {
    GST_ERROR ("Pad %" GST_PTR_FORMAT " is already linked.", sinkpad);
    goto end;
  }

  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    GST_ERROR ("Can not link pad %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
        srcpad, sinkpad);
  else
    GST_DEBUG ("Connected %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
        srcpad, sinkpad);

end:
  g_object_unref (sinkpad);
}

static void
agnosticbin_added_cb (GstElement * element, gpointer data)
{
  struct tmp_data *tmp = data;
  GstPad *pad;

  pad = gst_element_get_request_pad (element, tmp->src_pad_name);
  if (pad == NULL)
    return;

  GST_DEBUG ("Connecting pad %s", tmp->src_pad_name);

  connect_to_sink (tmp->sink, tmp->sink_pad_name, pad);
  gst_object_unref (pad);

  if (tmp->handler != 0L)
    g_signal_handler_disconnect (element, tmp->handler);
}

void
kms_element_link_pads (GstElement * src, const gchar * src_pad_name,
    GstElement * sink, const gchar * sink_pad_name)
{
  GstPad *pad;

  pad = gst_element_get_request_pad (src, src_pad_name);
  if (pad == NULL) {
    struct tmp_data *tmp;

    GST_DEBUG ("Put connection off until agnostic bin is created for pad %s",
        src_pad_name);
    tmp = create_tmp_data (src_pad_name, sink, sink_pad_name);
    tmp->handler = g_signal_connect_data (src, "agnosticbin-added",
        G_CALLBACK (agnosticbin_added_cb), tmp, destroy_tmp_data,
        (GConnectFlags) 0);
  } else {
    connect_to_sink (sink, sink_pad_name, pad);
    g_object_unref (pad);
  }
}

static void init_debug (void) __attribute__ ((constructor));

static void
init_debug (void)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
      GST_DEFAULT_NAME);
}
