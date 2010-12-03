#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <firefly-mv-utils/utils.h>

GST_DEBUG_CATEGORY (appsrc_pipeline_debug);
#define GST_CAT_DEFAULT appsrc_pipeline_debug

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

        len = frame->size[0]*frame->size[1]*1*sizeof(unsigned char);
        buffer = gst_buffer_new ();
        GST_BUFFER_DATA (buffer) = frame->image;
        GST_BUFFER_SIZE (buffer) = len;

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

    err = setup_gray_capture(app->camera, DC1394_VIDEO_MODE_640x480_MONO8);
    DC1394_ERR_CLN_RTN(err, cleanup_and_exit(app->camera), "Could not setup camera");

    err = dc1394_video_set_transmission(app->camera, DC1394_ON);
    DC1394_ERR_CLN_RTN(err, cleanup_and_exit(app->camera), "Could not start camera iso transmission");

  /* create a mainloop to get messages and to handle the idle handler that will
   * feed data to appsrc. */
  app->loop = g_main_loop_new (NULL, TRUE);

  app->pipeline = gst_parse_launch("appsrc name=mysource ! ffmpegcolorspace ! videoscale method=1 ! theoraenc bitrate=150 ! udpsink host=127.0.0.1 port=1234", NULL); 

  g_assert (app->pipeline);

  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  g_assert(bus);

  /* add watch for messages */
  gst_bus_add_watch (bus, (GstBusFunc) bus_message, app);

  /* get the appsrc */
    app->appsrc = gst_bin_get_by_name (GST_BIN(app->pipeline), "mysource");
    g_assert(app->appsrc);
    g_assert(GST_IS_APP_SRC(app->appsrc));
    g_signal_connect (app->appsrc, "need-data", G_CALLBACK (start_feed), app);
    g_signal_connect (app->appsrc, "enough-data", G_CALLBACK (stop_feed), app);

  /* set the caps on the source */
  caps = gst_caps_new_simple ("video/x-raw-gray",
                "width", G_TYPE_INT, 640,
                "height", G_TYPE_INT, 480,
                "bpp", G_TYPE_INT, 8,
                "depth", G_TYPE_INT, 8,
                "framerate", GST_TYPE_FRACTION, 25, 1,
                NULL);
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
