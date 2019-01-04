pfclient: pixelflut.c
	gcc  -O3 -march=skylake -pthread pixelflut.c lut.c `pkg-config --cflags --libs libavcodec libavformat libswscale libavutil` -o pfclient  -ggdb3 
clean:
	rm -f pfclient

.PHONY: clean
