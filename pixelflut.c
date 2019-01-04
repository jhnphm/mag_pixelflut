#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <resolv.h>
#include <netdb.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <sys/time.h>

//#define PIXELFLUT_ADDR "10.13.38.233"
#define PIXELFLUT_ADDR "localhost"
#define PIXELFLUT_PORT "1234"

#define PIXELFLUT_MOVIE "shining.mp4"

#define MAX_DIM 10000 
#define RECV_SZ (4+1+5+1+5+1) // actual size needed is 24, assuming 5 chars for res (overkill)
#define SEND_SZ (2+1+5+1+5+1+8+1)  // actual size needed is 24, assuming 8 chars for res (overkill)

#define THREADS 10

#include "lut.h"

struct vidbuf {
    size_t len;
    size_t linewidth;
    uint8_t buf[];
};

struct dimensions {
    uint32_t w;
    uint32_t h;
    uint32_t x;
    uint32_t y;
};

struct vidbuf* vidbuf;


int bw_shader(void* param, int x, int y){
  //  struct dimensions* dim = param;

  //  const int w = dim->w;
  //  const int h = dim->h;
  //  const int cw= w/2;
  //  const int ch= h/2;

  //  int gg = 0;
  //  int ii = (x-cw);
  //  int jj  = (y-ch);

  //  if(ii*ii + jj*jj < 50){
  //      gg = 0xFF;
  //  }
    if (vidbuf == 0){
        return 0;
    }
    if ( x >= vidbuf->linewidth){
        return 0;
    }
    if ( y >= vidbuf->len/vidbuf->linewidth){
        return 0;
    }
    return vidbuf->buf[x+y*vidbuf->linewidth];
}


int parse_size(struct dimensions* dim, char* str, size_t len){
    if(0 == memcmp(str, "SIZE ", 5)){
        char* endptr;
        dim->w = strtol(str+5, &endptr, 10);
        dim->h = strtol(endptr, &endptr, 10);
        if(dim->w == 0 || dim->h == 0){
            goto failsize;
        }
        if(dim->w > MAX_DIM || dim->h > MAX_DIM){
            goto failsize;
        }
    }else{
        goto failsize;
    }

    return 0;
failsize:
    return -1;
}

void render(int sockfd, struct dimensions* dim){
    const size_t SENDBUF_SZ = SEND_SZ*dim->w*dim->h;
    int retval = -1;
    char* sendbuf = malloc(SENDBUF_SZ);

    while(1){
        size_t offset = 0;
        for(int j = 0; j< dim->h; j++){
            for(int i = 0; i< dim->w; i++){
                // essentially a shader prog
                int gg = bw_shader (dim, i+dim->x,j+dim->y);

                // endprog
                const size_t remaining = SENDBUF_SZ - offset;
                size_t index = 0;
                //retval = snprintf(sendbuf + offset, remaining, "PX %" PRId32 " %" PRId32 " %02" PRIx32 "\n", i+dim->x, j+dim->y , gg);
                memcpy(sendbuf + offset, "PX ", 3);
                index += 3;

                memcpy(sendbuf + offset+index, DEC_LUT[i+dim->x].str, DEC_LUT[i+dim->x].len);
                index+=DEC_LUT[i+dim->x].len;
                sendbuf[offset+index] = ' ';
                index++;
                memcpy(sendbuf + offset+index, DEC_LUT[j+dim->y].str, DEC_LUT[j+dim->y].len);
                index+=DEC_LUT[j+dim->y].len;
                sendbuf[offset+index] = ' ';
                index++;

                memcpy(sendbuf + offset+index, HEX_LUT2[gg], 2);
                index+=4;
                sendbuf[offset+index] = '\n';
                index++;

                offset += index;
                if(offset > SENDBUF_SZ){
           //         printf("%zd\n",offset);
                    break;
                }
            }
        }
        sendbuf[offset] = 0;
        send(sockfd, sendbuf, offset, 0);
    }
    // never gets here
    free(sendbuf);

    return;
}

void* netthread(void* params){
    int id = (uintptr_t) params; // deliberately cast pointer to int
    int rval = -1;
    struct addrinfo addrinfo ={0};
    struct addrinfo* res = NULL;

    uint8_t recvbuf[RECV_SZ] = {0};

    addrinfo.ai_family = PF_UNSPEC;
    addrinfo.ai_socktype = SOCK_STREAM;
    addrinfo.ai_protocol = IPPROTO_TCP;

    rval = getaddrinfo(PIXELFLUT_ADDR, PIXELFLUT_PORT, &addrinfo, &res);
    if(rval != 0){
        goto failaddr;
    }

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    rval = connect(sockfd, res->ai_addr, res->ai_addrlen);
    if(rval != 0){
        printf("Failed to connect\n");
        goto failconn;
    }

    send(sockfd, "SIZE\n", 5, 0) ;
    recv(sockfd, recvbuf, sizeof(recvbuf), 0); 


    struct dimensions gfxsize = {0};
    rval = parse_size(&gfxsize, recvbuf, sizeof(recvbuf));

    gfxsize.x = gfxsize.w/THREADS * id;
    gfxsize.w = gfxsize.w/THREADS;
    if(rval != 0){
        goto failsize;
    }

    //printf("w: %" PRIu32 ", h: %" PRIu32 "\n", gfxsize.w, gfxsize.h);

    render(sockfd, &gfxsize);
failsize:

    close(sockfd);

failconn:
	freeaddrinfo(res);

failaddr:
    return NULL;
    

}

int64_t get_microsecond_time(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(int64_t)1000000+tv.tv_usec;
}

static void decode_vid_packet(AVPacket* packet, AVCodecContext* codec_ctx, AVFrame* frame, 
const AVRational* time_base,
        int64_t* tlast, int64_t *last_ts){
    int rval = -1;
    rval = avcodec_send_packet(codec_ctx, packet);
    if(rval < 0){
        printf("Error decoding packet\n");
    }
    const int64_t tbase_us = 1000000LL*time_base->num/time_base->den;

    while(rval >= 0){
        rval = avcodec_receive_frame(codec_ctx, frame);
        if(rval == AVERROR(EAGAIN) || rval == AVERROR_EOF || rval < 0){
            break;
        }
        int64_t tcurrent = get_microsecond_time();
        if(vidbuf == NULL){
            vidbuf = malloc(frame->linesize[0]*frame->height + sizeof(size_t)*2);
            vidbuf->len = frame->linesize[0]*frame->height;
            vidbuf->linewidth = frame->linesize[0];
            *tlast = tcurrent;
        }
        memcpy(vidbuf->buf, frame->data[0], frame->linesize[0]*frame->height);

        int64_t tdelay = (frame->pts-*last_ts)*tbase_us - (tcurrent - *tlast);
        tdelay = tdelay > 0 ? tdelay : 0;
        //printf("%" PRIx64 "\n", tdelay);
        
        usleep(tdelay);

        *last_ts= frame->pts;
        *tlast = tcurrent;
        av_frame_unref(frame);
    }                                                                                                                                                                                                           

}




void* vidthread(void* params){
    int64_t tlast = 0; 
    int64_t last_ts = 0; 
    int rval=-1;
    int vid_stream_idx=0;
    bool fail = true;
    char* fname = params;
    AVFormatContext* fmt_ctx = NULL;
	AVCodecParameters* codec_par ;
    AVCodecContext* codec_ctx;
	AVCodec* codec;

    av_register_all();
    avcodec_register_all();

    puts(fname);
    rval = avformat_open_input(&fmt_ctx, fname, NULL, NULL);
    if(rval != 0){
        char errbuf[128];
        av_strerror(rval, errbuf, sizeof(errbuf));
        printf("Unable to open video, fname: %s, rval: %s\n", fname, errbuf);
        goto fail_open_vid;
    }
    //printf("Format %s, duration %" PRId64 " us\n", fmt_ctx->iformat->long_name, fmt_ctx->duration);

    avformat_find_stream_info(fmt_ctx, NULL);

    for(int i = 0; i < fmt_ctx->nb_streams; i++){
        codec_par = fmt_ctx->streams[i]->codecpar;
        codec = avcodec_find_decoder(codec_par->codec_id);
        if (codec_par->codec_type == AVMEDIA_TYPE_VIDEO) {
            vid_stream_idx=i;
            break;
        }
    }
    codec_ctx = avcodec_alloc_context3(codec);
    if(codec_ctx == NULL){
        goto fail_codec_alloc;
    }

    rval = avcodec_parameters_to_context(codec_ctx, codec_par);
    if(rval != 0){
        goto fail_codec_open;
    }

	rval = avcodec_open2(codec_ctx, codec, NULL);
    if(rval != 0){
        goto fail_codec_open;
    }


    AVPacket *packet = av_packet_alloc();
    if(packet == NULL){
        goto fail_packet_alloc;
    }
	
    AVFrame *frame = av_frame_alloc();
    if(packet == NULL){
        goto fail_frame_alloc;
    }
	

	while(1){
		while(av_read_frame(fmt_ctx, packet)  >= 0){
			if(packet->stream_index == vid_stream_idx){
				decode_vid_packet(packet, codec_ctx, frame, 
                    &fmt_ctx->streams[vid_stream_idx]->time_base,
                        &tlast, &last_ts);	
			}
			av_packet_unref(packet);
		}
		av_seek_frame(fmt_ctx, vid_stream_idx, 0, AVSEEK_FLAG_ANY);
	}


    
    fail = false;

fail_codec_open:
    avcodec_free_context(&codec_ctx);
fail_codec_alloc:
    av_frame_free(&frame);
fail_frame_alloc:
    av_packet_free(&packet);
fail_packet_alloc:
	avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
fail_open_vid:
    if(fail){
        exit(-1);
    }
    return NULL;
}

int main(int argc, char** argv){
    //uint32_t THREADS = 1;

    pthread_t vid_handle; 
    pthread_t net_handles[THREADS]; //vla

    if(argc < 2){
        printf(" Insufficient params\n");
    }

    pthread_create(&vid_handle, NULL, vidthread, argv[1]);
    for (size_t i = 0; i < THREADS; i++){
        pthread_create(&net_handles[i], NULL, netthread, (void*)i);
    }
    pthread_join(vid_handle, NULL);
    for (size_t i = 0; i < THREADS; i++){
        pthread_join(net_handles[i], NULL);
    }

    return 0;

}
