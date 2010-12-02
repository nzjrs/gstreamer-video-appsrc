#!/bin/sh
gcc appsrc-stream.c `pkg-config --cflags --libs gstreamer-app-0.10 gdk-pixbuf-2.0` -o appsrc-stream
