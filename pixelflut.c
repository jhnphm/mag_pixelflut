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

//#define PIXELFLUT_ADDR "10.13.38.233"
#define PIXELFLUT_ADDR "localhost"
#define PIXELFLUT_PORT "1234"

#define PIXELFLUT_MOVIE "shining.mp4"

#define MAX_DIM 10000 
#define RECV_SZ (4+1+5+1+5+1) // actual size needed is 24, assuming 5 chars for res (overkill)
#define SEND_SZ (2+1+5+1+5+1+8+1)  // actual size needed is 24, assuming 8 chars for res (overkill)



struct dimensions {
    uint32_t w;
    uint32_t h;
};


inline int bw_shader(void* param, int x, int y){
    struct dimensions* dim = param;

    const int w = dim->w;
    const int h = dim->h;
    const int cw= w/2;
    const int ch= h/2;

    int gg = 0;
    int ii = (x-cw);
    int jj  = (y-ch);

    if(ii*ii + jj*jj < 50){
        gg = 0xFF;
    }
    return gg;
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
    const int w = dim->w;
    const int h = dim->h;
    const size_t SENDBUF_SZ = SEND_SZ*w*h;
    int retval = -1;
    char* sendbuf = malloc(SENDBUF_SZ);

    while(1){
        size_t offset = 0;
        for(int j = 0; j< h; j++){
            for(int i = 0; i< w; i++){
                // essentially a shader prog
                int gg = bw_shader (dim, i,j);

                // endprog
                const size_t remaining = SENDBUF_SZ - offset;
                retval = snprintf(sendbuf + offset, remaining, "PX %" PRId32 " %" PRId32 " %02" PRIx32 "\n", i, j , gg);
                if(retval < 0 || retval > remaining){
          //          printf("%d\n",retval);
                    break;
                }
                offset += retval;
                if(offset > SENDBUF_SZ){
           //         printf("%zd\n",offset);
                    break;
                }
            }
        }
        sendbuf[offset] = 0;
        //puts(sendbuf);
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
    if(rval != 0){
        goto failsize;
    }

    printf("w: %" PRIu32 ", h: %" PRIu32 "\n", gfxsize.w, gfxsize.h);

    render(sockfd, &gfxsize);
failsize:

    close(sockfd);

failconn:
	freeaddrinfo(res);

failaddr:
    return NULL;
    

}


//void* vidthread(void* params){
//    char* fname = params;
//}

int main(void){
    uint32_t numofcpus = sysconf(_SC_NPROCESSORS_ONLN);

    pthread_t handles[numofcpus]; //vla

    for (size_t i = 0; i < numofcpus; i++){
        pthread_create(&handles[i], NULL, netthread, (void*)i);
    }
    for (size_t i = 0; i < numofcpus; i++){
        pthread_join(handles[i], NULL);
    }

    return 0;

}


