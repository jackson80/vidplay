CC = gcc
CFLAGS = `pkg-config --cflags gstreamer-plugins-base-1.0 gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0`
LDFLAGS = `pkg-config --libs gstreamer-plugins-base-1.0 gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0`

objects = vidplayer.o

vidplayer : $(objects)
	$(CC) $(objects) $(CFLAGS) $(LDFLAGS) -g -Wall -o vidplayer


.PHONY : clean
clean :
	rm vidplayer $(objects)
