/* GStreamer
 *
 * appsrc-stream.c: example for using appsrc in streaming mode.
 *
 * Copyright (C) 2008 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <firefly-mv-utils/utils.h>

//FIXME: for testing
#include <gdk-pixbuf/gdk-pixbuf.h>

GST_DEBUG_CATEGORY (appsrc_pipeline_debug);
#define GST_CAT_DEFAULT appsrc_pipeline_debug

/*
 * an example application of using appsrc in streaming push mode. We simply push
 * buffers into appsrc. The size of the buffers we push can be any size we
 * choose.
 *
 * This example is very close to how one would deal with a streaming webserver
 * that does not support range requests or does not report the total file size.
 *
 * Some optimisations are done so that we don't push too much data. We connect
 * to the need-data and enough-data signals to start/stop sending buffers.
 *
 * Appsrc in streaming mode (the default) does not support seeking so we don't
 * have to handle any seek callbacks.
 *
 * Some formats are able to estimate the duration of the media file based on the
 * file length (mp3, mpeg,..), others report an unknown length (ogg,..).
 */
typedef struct _App App;

struct _App
{
  GstElement *pipeline;
  GstElement *appsrc;

  GMainLoop *loop;
  guint sourceid;

  dc1394camera_t *camera;
};

App s_app;

#define COLOUR 1

/* This method is called by the idle GSource in the mainloop. We feed CHUNK_SIZE
 * bytes into appsrc.
 * The ide handler is added to the mainloop when appsrc requests us to start
 * sending data (need-data signal) and is removed when appsrc has enough data
 * (enough-data signal).
 */
static gboolean
read_data (App * app)
{
    dc1394video_frame_t * frame;
    dc1394error_t err;
    guint len;
    GstFlowReturn ret;

    err = dc1394_capture_dequeue(app->camera, DC1394_CAPTURE_POLICY_WAIT, &frame);
    DC1394_WRN(err,"Could not capture a frame");

    if (frame) {
        GstBuffer *buffer;
        dc1394video_frame_t dest;
        gboolean ok = TRUE;

#if COLOUR
        len = frame->size[0]*frame->size[1]*3*sizeof(unsigned char);
        dest.image = (unsigned char *)malloc(len);

        err=dc1394_debayer_frames(frame, &dest, DC1394_BAYER_METHOD_NEAREST); 
        DC1394_WRN(err,"Could not debayer frame");

        buffer = gst_buffer_new ();
        GST_BUFFER_DATA (buffer) = dest.image;
        GST_BUFFER_SIZE (buffer) = len;
#else
        len = frame->size[0]*frame->size[1]*1*sizeof(unsigned char);
        buffer = gst_buffer_new ();
        GST_BUFFER_DATA (buffer) = frame->image;
        GST_BUFFER_SIZE (buffer) = len;
#endif

        GST_DEBUG ("feed buffer");
        g_signal_emit_by_name (app->appsrc, "push-buffer", buffer, &ret);
        gst_buffer_unref (buffer);

        if (ret != GST_FLOW_OK) {
            /* some error, stop sending data */
            GST_DEBUG ("some error");
            ok = FALSE;
        }

        err = dc1394_capture_enqueue(app->camera, frame);
        DC1394_WRN(err,"releasing buffer");

        return ok;
    }

    //  g_signal_emit_by_name (app->appsrc, "end-of-stream", &ret);
    return FALSE;

#if 0
  buffer = gst_buffer_new ();

  if (app->offset >= app->length) {
    /* we are EOS, send end-of-stream and remove the source */
    g_signal_emit_by_name (app->appsrc, "end-of-stream", &ret);
    return FALSE;
  }

  /* read the next chunk */
  len = CHUNK_SIZE;
  if (app->offset + len > app->length)
    len = app->length - app->offset;

  GST_BUFFER_DATA (buffer) = app->data + app->offset;
  GST_BUFFER_SIZE (buffer) = len;

  GST_DEBUG ("feed buffer %p, offset %" G_GUINT64_FORMAT "-%u", buffer,
      app->offset, len);
  g_signal_emit_by_name (app->appsrc, "push-buffer", buffer, &ret);
  gst_buffer_unref (buffer);
  if (ret != GST_FLOW_OK) {
    /* some error, stop sending data */
    return FALSE;
  }

  app->offset += len;

  return TRUE;
#endif
}

/* This signal callback is called when appsrc needs data, we add an idle handler
 * to the mainloop to start pushing data into the appsrc */
static void
start_feed (GstElement * pipeline, guint size, App * app)
{
  if (app->sourceid == 0) {
    GST_DEBUG ("start feeding");
    app->sourceid = g_idle_add ((GSourceFunc) read_data, app);
  }
}

/* This callback is called when appsrc has enough data and we can stop sending.
 * We remove the idle handler from the mainloop */
static void
stop_feed (GstElement * pipeline, App * app)
{
  if (app->sourceid != 0) {
    GST_DEBUG ("stop feeding");
    g_source_remove (app->sourceid);
    app->sourceid = 0;
  }
}

static gboolean
bus_message (GstBus * bus, GstMessage * message, App * app)
{
  GST_DEBUG ("got message %s",
      gst_message_type_get_name (GST_MESSAGE_TYPE (message)));

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
      g_error ("received error");
      g_main_loop_quit (app->loop);
      break;
    case GST_MESSAGE_EOS:
      g_main_loop_quit (app->loop);
      break;
    default:
      break;
  }
  return TRUE;
}

int
main (int argc, char *argv[])
{
  App *app = &s_app;
  GError *error = NULL;
  GstBus *bus;
  GstCaps *caps;
    dc1394_t * d;
    dc1394error_t   err;

  gst_init (&argc, &argv);

  GST_DEBUG_CATEGORY_INIT (appsrc_pipeline_debug, "appsrc-pipeline", 0,
      "appsrc pipeline example");

    /* setup the camera */
    d = dc1394_new ();
    if (!d) {
        g_critical("Could not setup dc1394");
        return -1;
    }

    app->camera = dc1394_camera_new (d, 0x00b09d010090a88dLL);
    if (!app->camera) {
        g_critical("Could not setup camera");
        return -1;
    }

#if COLOUR
    err = setup_color_capture(app->camera, DC1394_VIDEO_MODE_FORMAT7_0, DC1394_COLOR_CODING_RAW8);
#else
    err = setup_gray_capture(app->camera, DC1394_VIDEO_MODE_640x480_MONO8);
#endif
    DC1394_ERR_CLN_RTN(err, cleanup_and_exit(app->camera), "Could not setup camera");

    err = dc1394_video_set_transmission(app->camera, DC1394_ON);
    DC1394_ERR_CLN_RTN(err, cleanup_and_exit(app->camera), "Could not start camera iso transmission");

  /* create a mainloop to get messages and to handle the idle handler that will
   * feed data to appsrc. */
  app->loop = g_main_loop_new (NULL, TRUE);

  app->pipeline = gst_parse_launch("appsrc name=ffmvsource ! ffmpegcolorspace ! videoscale method=1 ! theoraenc bitrate = 150 ! udpsink host=127.0.0.1 port=1234", NULL);
  g_assert (app->pipeline);

  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  g_assert(bus);

  /* add watch for messages */
  gst_bus_add_watch (bus, (GstBusFunc) bus_message, app);

  /* get the appsrc */
  app->appsrc = gst_bin_get_by_name (GST_BIN(app->pipeline), "ffmvsource");
    g_assert(app->appsrc);
    g_assert(GST_IS_APP_SRC(app->appsrc));
  g_signal_connect (app->appsrc, "need-data", G_CALLBACK (start_feed), app);
  g_signal_connect (app->appsrc, "enough-data", G_CALLBACK (stop_feed), app);

  /* set the caps on the source */
#if COLOUR
  caps = gst_caps_new_simple ("video/x-raw-rgb",
     "width", G_TYPE_INT, 640,
     "height", G_TYPE_INT, 480,
     NULL);
#else
  caps = gst_caps_new_simple ("video/x-raw-gray",
     "bpp", G_TYPE_INT, 8,
     "depth", G_TYPE_INT, 8,
     "width", G_TYPE_INT, 640,
     "height", G_TYPE_INT, 480,
     NULL);
#endif
   gst_app_src_set_caps(GST_APP_SRC(app->appsrc), caps);


  /* go to playing and wait in a mainloop. */
  gst_element_set_state (app->pipeline, GST_STATE_PLAYING);

  /* this mainloop is stopped when we receive an error or EOS */
  g_main_loop_run (app->loop);

  GST_DEBUG ("stopping");

  gst_element_set_state (app->pipeline, GST_STATE_NULL);

    /* close the camera, etc */
    err = dc1394_video_set_transmission(app->camera, DC1394_OFF);
    DC1394_ERR_CLN_RTN(err,cleanup_and_exit(app->camera),"Could not stop the camera");

    cleanup_and_exit(app->camera);
    dc1394_free (d);

  gst_object_unref (bus);
  g_main_loop_unref (app->loop);

  return 0;
}
