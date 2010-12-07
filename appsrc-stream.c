/* gcc gdk-gstappsrc-stream.c -Wall `pkg-config --cflags --libs gstreamer-app-0.10 gdk-pixbuf-2.0` -o gdkstream */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappbuffer.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

GST_DEBUG_CATEGORY (appsrc_pipeline_debug);
#define GST_CAT_DEFAULT appsrc_pipeline_debug

typedef struct _App App;

struct _App
{
  GstElement *pipeline;
  GstAppSrc *appsrc;

  GMainLoop *loop;
  guint sourceid;

  GRand *rand;
  guint32 offset;

};

App s_app;
const guint framesize = 640*480*3*sizeof(guchar);

static gboolean
read_data (App * app)
{
    GstFlowReturn ret;
    gdouble ms;
    GstBuffer   *buffer;
    GdkPixbuf   *pb;
    guint32     colour;
    guint       len = framesize;

    pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 640, 480);

    colour = g_rand_int_range(app->rand, 0x00, 0xFF);
    gdk_pixbuf_fill(pb, (colour << 24) | (colour << 16) | (colour << 8) | 0x000000FF);

    buffer = gst_app_buffer_new (gdk_pixbuf_get_pixels(pb), len, g_object_unref, pb);

    /* has no effect */
    GST_BUFFER_OFFSET(buffer) = app->offset;
    GST_BUFFER_SIZE(buffer) = len;
    app->offset += len;

    GST_DEBUG ("feed buffer colour: %d", colour);

    ret = gst_app_src_push_buffer (app->appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        /* some error, stop sending data */
        GST_DEBUG ("some error");
        return FALSE;
    }

    return TRUE;
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
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg_info = NULL;

        gst_message_parse_error (message, &err, &dbg_info);
        g_printerr ("ERROR from element %s: %s\n",
            GST_OBJECT_NAME (message->src), err->message);
        g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
        g_error_free (err);
        g_free (dbg_info);
        g_main_loop_quit (app->loop);
        break;
    }
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
    GstBus *bus;
    GstCaps *caps;

    gst_init (&argc, &argv);

    GST_DEBUG_CATEGORY_INIT (appsrc_pipeline_debug, "appsrc-pipeline", 0,
      "appsrc pipeline example");

    /* create a mainloop to get messages and to handle the idle handler that will
    * feed data to appsrc. */
    app->loop = g_main_loop_new (NULL, TRUE);

    app->rand = g_rand_new();
    app->offset = 0;

    app->pipeline = gst_parse_launch("appsrc name=mysource ! ffmpegcolorspace ! videoscale method=1 ! theoraenc bitrate=150 ! udpsink host=127.0.0.1 port=1234", NULL);

    g_assert (app->pipeline);

    bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
    g_assert(bus);

    /* add watch for messages */
    gst_bus_add_watch (bus, (GstBusFunc) bus_message, app);

    /* get the appsrc */
    app->appsrc = GST_APP_SRC( gst_bin_get_by_name (GST_BIN(app->pipeline), "mysource") );
    gst_app_src_set_size(app->appsrc, G_MAXINT64);
    gst_app_src_set_max_bytes (app->appsrc, framesize);
    g_signal_connect (app->appsrc, "need-data", G_CALLBACK (start_feed), app);
    g_signal_connect (app->appsrc, "enough-data", G_CALLBACK (stop_feed), app);

    /* set the caps on the source */
    caps = gst_caps_new_simple ("video/x-raw-rgb",
                "width", G_TYPE_INT, 640,
                "height", G_TYPE_INT, 480,
                "bpp", G_TYPE_INT, 24,
                "depth", G_TYPE_INT, 24,
                "red_mask",   G_TYPE_INT, 0x00ff0000,
                "green_mask", G_TYPE_INT, 0x0000ff00,
                "blue_mask",  G_TYPE_INT, 0x000000ff,
                "framerate", GST_TYPE_FRACTION, 25, 1,
                "endianness", G_TYPE_INT, G_BIG_ENDIAN,
                NULL);
    gst_app_src_set_caps(GST_APP_SRC(app->appsrc), caps);

    /* go to playing and wait in a mainloop. */
    gst_element_set_state (app->pipeline, GST_STATE_PLAYING);

    /* this mainloop is stopped when we receive an error or EOS */
    g_main_loop_run (app->loop);

    GST_DEBUG ("stopping");

    gst_element_set_state (app->pipeline, GST_STATE_NULL);

    gst_object_unref (bus);
    g_main_loop_unref (app->loop);

    return 0;
}
