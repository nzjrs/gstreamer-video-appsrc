#!/bin/sh
gcc appsrc-stream.c `pkg-config --cflags --libs gstreamer-app-0.10 gdk-pixbuf-2.0` -o gdk-appsrc-stream
gcc appsrc-stream-ffmv.c `pkg-config --cflags --libs gstreamer-app-0.10 firefly-mv-utils` -o ffmv-appsrc-stream
