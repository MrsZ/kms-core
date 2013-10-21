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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "kmsutils.h"
#include "kmselement.h"
#include "kmsagnosticcaps.h"
#include "kmsplayerendpoint.h"

#define PLUGIN_NAME "playerendpoint"
#define AUDIO_APPSRC "audio_appsrc"
#define VIDEO_APPSRC "video_appsrc"
#define URIDECODEBIN "uridecodebin"

#define APPSRC_DATA "appsrc_data"
#define APPSINK_DATA "appsink_data"

GST_DEBUG_CATEGORY_STATIC (kms_player_end_point_debug_category);
#define GST_CAT_DEFAULT kms_player_end_point_debug_category

#define KMS_PLAYER_END_POINT_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (                 \
    (obj),                                      \
    KMS_TYPE_PLAYER_END_POINT,                  \
    KmsPlayerEndPointPrivate                    \
  )                                             \
)

struct _KmsPlayerEndPointPrivate
{
  GstElement *pipeline;
  GstElement *uridecodebin;
};

enum
{
  SIGNAL_EOS,
  LAST_SIGNAL
};

static guint kms_player_end_point_signals[LAST_SIGNAL] = { 0 };

/* pad templates */

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsPlayerEndPoint, kms_player_end_point,
    KMS_TYPE_URI_END_POINT,
    GST_DEBUG_CATEGORY_INIT (kms_player_end_point_debug_category, PLUGIN_NAME,
        0, "debug category for playerendpoint element"));

static void
kms_player_end_point_dispose (GObject * object)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (object);

  if (self->priv->pipeline != NULL) {
    GstBus *bus;

    bus = gst_pipeline_get_bus (GST_PIPELINE (self->priv->pipeline));
    gst_bus_set_sync_handler (bus, NULL, NULL, NULL);
    g_object_unref (bus);

    gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
    gst_object_unref (GST_OBJECT (self->priv->pipeline));
    self->priv->pipeline = NULL;
  }

  /* clean up as possible. May be called multiple times */

  G_OBJECT_CLASS (kms_player_end_point_parent_class)->dispose (object);
}

static void
kms_player_end_point_finalize (GObject * object)
{
  G_OBJECT_CLASS (kms_player_end_point_parent_class)->finalize (object);
}

static GstFlowReturn
new_sample_cb (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;

  g_signal_emit_by_name (appsink, "pull-sample", &sample);

  if (sample == NULL)
    return GST_FLOW_OK;

  buffer = gst_sample_get_buffer (sample);

  if (buffer == NULL) {
    ret = GST_FLOW_OK;
    goto end;
  }

  gst_buffer_ref (buffer);
  buffer = gst_buffer_make_writable (buffer);

  buffer->pts = GST_CLOCK_TIME_NONE;
  buffer->dts = GST_CLOCK_TIME_NONE;
  buffer->offset = GST_CLOCK_TIME_NONE;
  buffer->offset_end = GST_CLOCK_TIME_NONE;

  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &ret);

  gst_buffer_unref (buffer);

  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send buffer to appsrc %s. Cause: %s",
        GST_ELEMENT_NAME (appsrc), gst_flow_get_name (ret));
  }

end:
  if (sample != NULL)
    gst_sample_unref (sample);

  return ret;
}

static void
eos_cb (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstStructure *s;
  GstEvent *event;
  GstPad *srcpad;

  GST_DEBUG ("Sending custom playerendpoint eos event to %s",
      GST_ELEMENT_NAME (appsrc));

  srcpad = gst_element_get_static_pad (appsrc, "src");
  if (srcpad == NULL) {
    GST_ERROR ("Can not get source pad from %s", GST_ELEMENT_NAME (appsrc));
    return;
  }

  s = gst_structure_new_empty (KMS_PLAYERENDPOINT_CUSTOM_EVENT_NAME);
  event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, s);

  if (!gst_pad_push_event (srcpad, event))
    GST_ERROR (KMS_PLAYERENDPOINT_CUSTOM_EVENT_NAME " event could not be sent");

  g_object_unref (srcpad);
}

static void
pad_added (GstElement * element, GstPad * pad, KmsPlayerEndPoint * self)
{
  GST_DEBUG ("Pad added");
  GstElement *appsrc, *agnosticbin, *appsink;
  GstPad *sinkpad;
  GstCaps *audio_caps, *video_caps;
  GstCaps *src_caps;

  /* Create and link appsrc--agnosticbin with proper caps */
  audio_caps = gst_caps_from_string (KMS_AGNOSTIC_AUDIO_CAPS);
  video_caps = gst_caps_from_string (KMS_AGNOSTIC_VIDEO_CAPS);
  src_caps = gst_pad_query_caps (pad, NULL);
  GST_DEBUG ("caps are %" GST_PTR_FORMAT, src_caps);

  if (gst_caps_can_intersect (audio_caps, src_caps))
    agnosticbin = kms_element_get_audio_agnosticbin (KMS_ELEMENT (self));
  else if (gst_caps_can_intersect (video_caps, src_caps))
    agnosticbin = kms_element_get_video_agnosticbin (KMS_ELEMENT (self));
  else {
    GST_ELEMENT_ERROR (self, CORE, CAPS, ("No agnostic caps provided"),
        GST_ERROR_SYSTEM);
    goto end;
  }

  /* Create appsrc element and link to agnosticbin */
  appsrc = gst_element_factory_make ("appsrc", NULL);
  g_object_set (G_OBJECT (appsrc), "is-live", TRUE, "do-timestamp", TRUE,
      "min-latency", G_GUINT64_CONSTANT (0),
      "max-latency", G_GUINT64_CONSTANT (0), "format", GST_FORMAT_TIME,
      "caps", src_caps, NULL);

  gst_bin_add (GST_BIN (self), appsrc);
  gst_element_sync_state_with_parent (appsrc);
  if (!gst_element_link (appsrc, agnosticbin)) {
    GST_ERROR ("Could not link %s to element %s", GST_ELEMENT_NAME (appsrc),
        GST_ELEMENT_NAME (agnosticbin));
  }

  /* Create appsink and link to pad */
  appsink = gst_element_factory_make ("appsink", NULL);
  g_object_set (appsink, "sync", TRUE, "enable-last-sample",
      FALSE, "emit-signals", TRUE, NULL);
  gst_bin_add (GST_BIN (self->priv->pipeline), appsink);
  gst_element_sync_state_with_parent (appsink);

  sinkpad = gst_element_get_static_pad (appsink, "sink");
  gst_pad_link (pad, sinkpad);
  GST_DEBUG_OBJECT (self, "Linked %s---%s", GST_ELEMENT_NAME (element),
      GST_ELEMENT_NAME (appsink));
  g_object_unref (sinkpad);

  g_object_set_data (G_OBJECT (pad), APPSRC_DATA, appsrc);
  g_object_set_data (G_OBJECT (pad), APPSINK_DATA, appsink);

  /* Connect new-sample signal to callback */
  g_signal_connect (appsink, "new-sample", G_CALLBACK (new_sample_cb), appsrc);
  g_signal_connect (appsink, "eos", G_CALLBACK (eos_cb), appsrc);

end:
  if (src_caps != NULL)
    gst_caps_unref (src_caps);

  if (audio_caps != NULL)
    gst_caps_unref (audio_caps);

  if (video_caps != NULL)
    gst_caps_unref (video_caps);
}

static void
pad_removed (GstElement * element, GstPad * pad, KmsPlayerEndPoint * self)
{
  GST_DEBUG ("Pad removed");
  GstElement *appsink, *appsrc;

  if (GST_PAD_IS_SINK (pad))
    return;

  appsink = g_object_steal_data (G_OBJECT (pad), APPSINK_DATA);
  appsrc = g_object_steal_data (G_OBJECT (pad), APPSRC_DATA);

  if (appsrc != NULL) {
    GST_INFO ("Removing %" GST_PTR_FORMAT " from its parent", appsrc);
    if (GST_OBJECT_PARENT (appsrc) != NULL) {
      g_object_ref (appsrc);
      gst_bin_remove (GST_BIN (GST_OBJECT_PARENT (appsrc)), appsrc);
      gst_element_set_state (appsrc, GST_STATE_NULL);
      g_object_unref (appsrc);
    }
  }

  if (appsink == NULL) {
    GST_ERROR ("No appsink was found associated with %" GST_PTR_FORMAT, pad);
    return;
  }
  if (!gst_element_set_locked_state (appsink, TRUE))
    GST_ERROR ("Could not block element %s", GST_ELEMENT_NAME (appsink));

  GST_DEBUG ("Removing appsink %s from %s", GST_ELEMENT_NAME (appsink),
      GST_ELEMENT_NAME (self->priv->pipeline));

  gst_element_set_state (appsink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->priv->pipeline), appsink);
}

static void
kms_player_end_point_stopped (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set internal pipeline to NULL */
  gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);

  KMS_URI_END_POINT_GET_CLASS (self)->change_state (KMS_URI_END_POINT (self),
      KMS_URI_END_POINT_STATE_STOP);
}

static void
kms_player_end_point_started (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set uri property in uridecodebin */
  g_object_set (G_OBJECT (self->priv->uridecodebin), "uri",
      KMS_URI_END_POINT (self)->uri, NULL);

  /* Set internal pipeline to playing */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PLAYING);

  KMS_URI_END_POINT_GET_CLASS (self)->change_state (KMS_URI_END_POINT (self),
      KMS_URI_END_POINT_STATE_START);
}

static void
kms_player_end_point_paused (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set internal pipeline to paused */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PAUSED);

  KMS_URI_END_POINT_GET_CLASS (self)->change_state (KMS_URI_END_POINT (self),
      KMS_URI_END_POINT_STATE_PAUSE);
}

static void
kms_player_end_point_class_init (KmsPlayerEndPointClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  KmsUriEndPointClass *urienpoint_class = KMS_URI_END_POINT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "PlayerEndPoint", "Sink/Generic", "Kurento plugin player end point",
      "Joaquin Mengual García <kini.mengual@gmail.com>");

  gobject_class->dispose = kms_player_end_point_dispose;
  gobject_class->finalize = kms_player_end_point_finalize;

  urienpoint_class->stopped = kms_player_end_point_stopped;
  urienpoint_class->started = kms_player_end_point_started;
  urienpoint_class->paused = kms_player_end_point_paused;

  kms_player_end_point_signals[SIGNAL_EOS] =
      g_signal_new ("eos",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsPlayerEndPointClass, eos_signal), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsPlayerEndPointPrivate));
}

static GstBusSyncReply
bus_sync_signal_handler (GstBus * bus, GstMessage * msg, gpointer data)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (data);

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
    g_signal_emit (G_OBJECT (self),
        kms_player_end_point_signals[SIGNAL_EOS], 0);
  }

  return GST_BUS_PASS;
}

static void
kms_player_end_point_init (KmsPlayerEndPoint * self)
{
  GstBus *bus;
  GstCaps *deco_caps;

  self->priv = KMS_PLAYER_END_POINT_GET_PRIVATE (self);

  self->priv->pipeline = gst_pipeline_new ("pipeline");
  self->priv->uridecodebin =
      gst_element_factory_make ("uridecodebin", URIDECODEBIN);

  deco_caps = gst_caps_from_string (KMS_AGNOSTIC_CAPS_CAPS);
  g_object_set (G_OBJECT (self->priv->uridecodebin), "caps", deco_caps, NULL);
  gst_caps_unref (deco_caps);

  gst_bin_add (GST_BIN (self->priv->pipeline), self->priv->uridecodebin);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->priv->pipeline));
  gst_bus_set_sync_handler (bus, bus_sync_signal_handler, self, NULL);
  g_object_unref (bus);

  /* Connect to signals */
  g_signal_connect (self->priv->uridecodebin, "pad-added",
      G_CALLBACK (pad_added), self);
  g_signal_connect (self->priv->uridecodebin, "pad-removed",
      G_CALLBACK (pad_removed), self);
}

gboolean
kms_player_end_point_plugin_init (GstPlugin * plugin)
{

  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_PLAYER_END_POINT);
}
