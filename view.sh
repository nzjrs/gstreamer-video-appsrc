#!/bin/sh
gst-launch -v udpsrc port=1234 ! theoradec ! autovideosink
