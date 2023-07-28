## Introduction

**ff-rknn** is a small application optimized to decode H264 / H265 stream with rkmpp hardware decoding, access Rockchip NPU hardware accelerator and render the result using SDL3 texture with mali hardware acceleration.<br>
This document describes how to build and run ff-rknn on Rockchip devices with NPU. It can decode h264/hevc, rtsp, rtmp and http streams.


### System requirements

* Rockchip board with NPU enabled (rknn)
* SDL3 with hardware acceleration for Rockchip platform
* FFmpeg with rkmpp decoder enabled
* X11 with hardware acceleration or Wayland / Weston with hardware acceleration (mali)
* librga - Rockchip 2D acceleration
* libyuv (for latest FFmpeg)



### Limitations

The code makes use of FFmpeg (LGPL), SDL3 (zlib), and Rockhip rknn (Apache 2) model type converted from Yolov5s model. The code is provided in source code form to comply with license requirements.<br>
Binary code is not provided and you must compile it yourself. It is highly dependent on these libraries and Desktop Hardware acceleration. <br>A version with OpenCV may be provided to extend AI capabilities but OpenCV is slow.<br>
ff-rknn can decode and render H264 / H265 stream only.


## Install necessary libraries

Install SDL3, FFmpeg (with mp4,mkv,rtsp,rtmp,http support), librga, libyuv, librockchip_rkmpp


## Build and run ff-rknn

 - **build**

	    g++ -O2 --permissive -o ff-rknn ff-rknn.c postprocess.cc -I/usr/include/drm -I/usr/include -D_FILE_OFFSET_BITS=64 -D REENTRANT `pkg-config --cflags --libs sdl3` -lz -lm -lpthread -ldrm -lrockchip_mpp -lrga -lvorbis -lvorbisenc -ltiff -lopus -logg -lmp3lame -llzma -lrtmp -lssl -lcrypto -lbz2 -lxml2 -lX11 -lxcb -lXv -lXext -lv4l2 -lasound -lpulse -lGL -lGLESv2 -lsndio -lfreetype -lxcb -lxcb-shm -lxcb -lxcb-xfixes -lxcb-render -lxcb-shape -lxcb -lxcb-shape -lxcb -lavutil -lavcodec -lavformat -lavdevice -lavfilter -lswscale -lswresample -lpostproc -lrknnrt


 - **run**


	    export LD_LIBRARY_PATH=./lib1.5/


    - `MP4 / MKV video streams` - Play h264 video stream

		    ./ff-rknn -i ../../videos_rknn/vid-3.mp4 -x 960 -y 540 -l 0 -t 0 -m ./model/RK3588/yolov5s-640-640.rknn

    - `RTSP` - Play *rtsp* live stream from camera

		    ./ff-rknn -f rtsp -i rtsp://192.168.254.217:554/stream1  -x 960 -y 540 -left 0 -top 0 -m ./model/RK3588/yolov5s-640-640.rknn

    - `RTMP / FLV` - Play *rtmp* live video stream from camera (flv format)

		    ./ff-rknn -f rtmp -i rtmp://192.168.254.217:1935/live/stream -x 960 -y 540 -l 1920 -t 540 -m ./model/RK3588/yolov5s-640-640.rknn

    - `HTTP / FLV` - Play *http* live video stream from camera (flv format)

		    ./ff-rknn -f http -i http://192.168.254.217:8080/live/stream.flv -x 960 -y 540 -l 1920 -t 0 -m ./model/RK3588/yolov5s-640-640.rknn

    - `V4l2 H264` - Play *h264* live video stream from H264 USB camera

		    ./ff-rknn -f v4l2 -p h264 -s 1920x1080 -i /dev/video23 -m ./model/RK3588/yolov5s-640-640.rknn -x 960 -y 540

    - `ALPHA BLEND` - Play *h264* / *H265* video stream and draw alpha blend rectangle on detected objects

		     DISPLAY=:0.0 ./ff-rknn -i /apps/videos_rknn/vid-2.mp4 -x 960 -y 540 -l 0 -t 0 -m ./model/RK3588/yolov5s-640-640.rknn -b 80 -o motorcycle -a 60

- **parameters**

  - -x displayed width
  - -y displayed height
  - -l displayed left position (X11)
  - -t displayed top position (X11)
  - -m rknn model
  - -f protocol (v4l2, rtsp, rtmp, http)
  - -p pixel format (h264) - camera
  - -s video frame size (WxH) - camera
  - -r video frame rate - camera
  - -o unique object to detect
  - -b use alpha blend on detected objects (1 ~ 255)
  - -a accuracy perc (1 ~ 100)\n");

## References

* SDL3 - https://github.com/libsdl-org/SDL	
* Rockchip Acceleration - https://github.com/JeffyCN/
* Rockchip RKNPU2 - https://github.com/rockchip-linux/rknpu2
* FFMPEG - https://github.com/hbiyik/FFmpeg or your preferred FFmpeg
