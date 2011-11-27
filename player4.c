#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include <math.h>

#define MAX_NAME_LEN 1024

#define SDL_AUDIO_BUFFER_SIZE 1024

#define VIDEO_FRAME_QUEUE_SIZE 1
#define MAX_AUDIO_QUEUE_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_QUEUE_SIZE (5 * 256 * 1024)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

#define true 1
#define false 0


typedef struct VideoFrame {
    int width, height;
    int allocated;
    SDL_Overlay *bmp;
} VideoFrame;

typedef struct AudioFrame {
    
} AudioFrame;


typedef struct PacketQueue {
    AVPacketList *header, *end;
    int pktNum;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct FileState {
    AVFormatContext *pFormatCtx;
    char fileName[MAX_NAME_LEN];
} FileState;

typedef struct VideoState {
    int videoTrack;
    PacketQueue videoq;
    AVStream *videoStream;
    AVCodecContext *pCodecCtx;
    VideoFrame vFrameq[VIDEO_FRAME_QUEUE_SIZE];
    int vFrameqSize;
    int vFrameqWindex;
    int vFrameqRindex;
    SDL_mutex *vFrameqMutex;
    SDL_cond *vFrameqCond;
    double videoClk;
} VideoState;

typedef struct AudioState {
    int audioTrack;
    PacketQueue audioq;
    AVStream *audioStream;
    AVCodecContext *pCodecCtx;
    uint8_t audioBuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    uint8_t *audioPktData;
    int audioPktSize;
    AVPacket audio_pkt;
    int audio_hw_buf_size;
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    double audioClk;
    int quit;
} AudioState;

typedef struct State {
    FileState *file;
    VideoState *video;
    AudioState *audio;
} State;

SDL_Surface *screen;

void init_queue(PacketQueue *queue)
{
    memset(queue, 0, sizeof(PacketQueue));
    queue->mutex = SDL_CreateMutex();
    queue->cond = SDL_CreateCond();
}

int add_to_queue(PacketQueue *queue, AVPacket *pkt)
{
    AVPacketList *pkt1;
    pkt1 = av_malloc(sizeof(AVPacketList));
    if(!pkt1)
	return -1;
    if(av_dup_packet(pkt))
	return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(queue->mutex);
    if(!queue->end)
	queue->header = pkt1;
    else
	queue->end->next = pkt1;
    queue->end = pkt1;
    queue->pktNum++;
    queue->size += pkt1->pkt.size;
    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);
    return 0;
}

int get_from_queue(PacketQueue *queue, AVPacket *pkt)
{
    AVPacketList *pkt1;
    int ret;
    SDL_LockMutex(queue->mutex);
    while(true) {
	if((pkt1 = queue->header) != NULL) {
	    queue->header = queue->header->next;
	    if(queue->header == NULL)
		queue->end = NULL;
	    queue->pktNum--;
	    queue->size -= pkt1->pkt.size;
	    *pkt = pkt1->pkt;
	    av_free(pkt1);
	    ret = 1;
	    break;
	}else {
	    printf("waiting for queue...\n");
	    SDL_CondWait(queue->cond, queue->mutex);
	}
    }
    SDL_UnlockMutex(queue->mutex);
    return ret;
}

void queue_flush(PacketQueue *queue)
{
    AVPacketList *pkt, *pkt1;
    SDL_LockMutex(queue->mutex);
    for (pkt = queue->header; pkt != NULL; pkt = pkt1) {
	pkt1 = pkt->next;
	av_free_packet(&pkt->pkt);
	av_freep(&pkt);
    }
    queue->header = NULL;
    queue->end = NULL;
    queue->pktNum = 0;
    queue->size = 0;
    SDL_UnlockMutex(queue->mutex);
}

int get_file_info(FileState *file)
{
    if(av_open_input_file(&file->pFormatCtx, file->fileName, NULL, 0, NULL) != 0)
	return -1;
    if(av_find_stream_info(file->pFormatCtx) < 0)
	return -1;
    dump_format(file->pFormatCtx, 0, file->fileName, 0);
    return 0;
}

void find_av_streams(FileState *file, VideoState *video, AudioState *audio) {
    int i;
    video->videoTrack = -1;
    audio->audioTrack = -1;
    for (i = 0; i < file->pFormatCtx->nb_streams; i++)
    {
	if(file->pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
	    video->videoTrack = i;
	    video->videoStream = file->pFormatCtx->streams[i];
	    video->pCodecCtx = file->pFormatCtx->streams[i]->codec;
            find_video_decoder(video);
	}
	if(file->pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
	    audio->audioTrack = i;
	    audio->audioStream = file->pFormatCtx->streams[i];
	    audio->pCodecCtx = file->pFormatCtx->streams[i]->codec;
            find_audio_decoder(audio);
	}
    }
}

int find_video_decoder(VideoState *video)
{
    AVCodec *pCodec;
    //find decoder for the video stream
    pCodec = avcodec_find_decoder(video->pCodecCtx->codec_id);
    if(pCodec == NULL)
    {
	fprintf(stderr, "Codec not found!\n");
	return 4;
    }
    if(avcodec_open(video->pCodecCtx, pCodec) < 0)
	return 5;
    return 0;
}

int queue_av_pkt(void *arg) {
    AVPacket *pkt = av_mallocz(sizeof(AVPacket));
    State *state = (State *)arg;
    init_queue(&state->audio->audioq);
    init_queue(&state->video->videoq);
    while(true) {
	//if queue is full, wait for eat
	if(state->video->videoq.size > MAX_VIDEO_QUEUE_SIZE || state->audio->audioq.size > MAX_AUDIO_QUEUE_SIZE) {
	    SDL_Delay(5);
	    continue;
	}
	//if packet is valid
	if(av_read_frame(state->file->pFormatCtx, pkt) < 0) {
	    if(url_ferror(state->file->pFormatCtx->pb) == 0) {
		SDL_Delay(100);
		continue;
	    }else {
		break;		// error or end of file
	    }
	}
	//put packet into AV queue
	if(pkt->stream_index == state->video->videoTrack) {
	    add_to_queue(&state->video->videoq, pkt);
	}else if(pkt->stream_index == state->audio->audioTrack) {
	    add_to_queue(&state->audio->audioq, pkt);
	}else {
	    av_free_packet(pkt);
	}
    }
}

int decode_audio(AudioState *audio, uint8_t *audio_buf, int buf_size, double *pts_ptr)
{
  int len1, data_size, n;
  AVPacket *pkt = &audio->audio_pkt;
  double pts;

  for(;;) {
    while(audio->audioPktSize > 0) {
      data_size = buf_size;
      len1 = avcodec_decode_audio3(audio->pCodecCtx, 
			 (int16_t *)audio_buf, &data_size, pkt);
      if(len1 < 0) {
	/* if error, skip frame */
	audio->audioPktSize = 0;
	break;
      }
      audio->audioPktData += len1;
      audio->audioPktSize -= len1;
      if(data_size <= 0) {
	/* No data yet, get more frames */
	continue;
      }
      pts = audio->audioClk;
      *pts_ptr = pts;
      n = 2;//* audio->pCodecCtx->channels;
      audio->audioClk += (double)data_size /
	(double)(n * audio->pCodecCtx->sample_rate);

      /* We have data, return it and come back for more later */
      return data_size;
    }
    if(pkt->data)
      av_free_packet(pkt);

    if(audio->quit) {
      return -1;
    }
    /* next packet */
    if(get_from_queue(&audio->audioq, pkt) < 0) {
      break;
    }
    audio->audioPktData = pkt->data;
    audio->audioPktSize = pkt->size;
    /* if update, update the audio clock w/pts */
    if(pkt->pts != AV_NOPTS_VALUE) {
      audio->audioClk = av_q2d(audio->pCodecCtx->time_base)*pkt->pts;
    }

  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) 
{

  AudioState *audio = userdata;
  int len1, audio_size;
  double pts;

  while(len > 0) {
    if(audio->audio_buf_index >= audio->audio_buf_size) {
      /* We have already sent all our data; get more */
      audio_size = decode_audio(audio, audio->audioBuf, sizeof(audio->audioBuf), &pts);
      if(audio_size < 0) {
	/* If error, output silence */
	audio->audio_buf_size = 1024;
	memset(audio->audioBuf, 0, audio->audio_buf_size);
      } else {
	audio->audio_buf_size = audio_size;
      }
      audio->audio_buf_index = 0;
    }
    len1 = audio->audio_buf_size - audio->audio_buf_index;
    if(len1 > len)
      len1 = len;
    memcpy(stream, (uint8_t *)audio->audioBuf + audio->audio_buf_index, len1);
    len -= len1;
    stream += len1;
    audio->audio_buf_index += len1;
  }
}

int decode_video(void *arg)
{
    VideoState *video = (VideoState *)arg;
    AVFrame *pFrame;
    int frameFinished;
    AVPacket packet;
    AVPacket *pkt = &packet;
	

    pFrame = avcodec_alloc_frame();
    while(true) {
	if(get_from_queue(&video->videoq, pkt) < 0) {
	    break;
	}
	avcodec_decode_video2(video->pCodecCtx, pFrame, &frameFinished, &packet);
	//did we get a video frame?
	if(frameFinished) {
	    if(video_frame_queue(video, pFrame) < 0)
		break;
	}
	av_free_packet(pkt);
    }
    av_free(pFrame);
    return 0;
}

int find_audio_decoder(AudioState *audio)
{
    AVCodec *aCodec;
    SDL_AudioSpec wanted_spec, spec;

    wanted_spec.freq = audio->pCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = audio->pCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = audio;
    
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
    audio->audio_hw_buf_size = spec.size;

    aCodec = avcodec_find_decoder(audio->pCodecCtx->codec_id);
    if(aCodec == NULL)
    {
	fprintf(stderr, "Codec not found!\n");
	return 4;
    }
    if(avcodec_open(audio->pCodecCtx, aCodec) < 0)
	return 5;

    audio->audio_buf_size = 0;
    audio->audio_buf_index = 0;
    memset(&audio->audio_pkt, 0, sizeof(audio->audio_pkt));
    SDL_PauseAudio(0);

    return 0;
}

int video_frame_queue(VideoState *video, AVFrame *pFrame)
{
    AVPicture pict;
    VideoFrame *vf;
    int dstPixFmt;
    static struct SwsContext *imgConvertCtx;
    SDL_LockMutex(video->vFrameqMutex);
    while(video->vFrameqSize >= VIDEO_PICTURE_QUEUE_SIZE) {
	SDL_CondWait(video->vFrameqCond, video->vFrameqMutex);
    }
    SDL_UnlockMutex(video->vFrameqMutex);
    //vFrameqWindex is set to 0 by default
    vf = &video->vFrameq[video->vFrameqWindex];
    // allocate or resize the buffer
    if(!vf->bmp || vf->width != video->videoStream->codec->width || vf->height != video->videoStream->codec->height) {
	SDL_Event event;
	vf->allocated = 0;
	event.type = FF_ALLOC_EVENT;
	event.user.data1 = video;
	SDL_PushEvent(&event);

	SDL_LockMutex(video->vFrameqMutex);
	//wait for frame allocated
	while(!vf->allocated) {
	    SDL_CondWait(video->vFrameqCond, video->vFrameqMutex);
	}
	SDL_UnlockMutex(video->vFrameqMutex);
    }
    //put our pict on the queue
    if(vf->bmp) {
	SDL_LockYUVOverlay(vf->bmp);

	dstPixFmt = PIX_FMT_YUV420P;
	pict.data[0] = vf->bmp->pixels[0];
	pict.data[1] = vf->bmp->pixels[2];
	pict.data[2] = vf->bmp->pixels[1];

	pict.linesize[0] = vf->bmp->pitches[0];
	pict.linesize[1] = vf->bmp->pitches[2];
	pict.linesize[2] = vf->bmp->pitches[1];

	if(imgConvertCtx == NULL) {
	    int w = video->videoStream->codec->width;
	    int h = video->videoStream->codec->height;
	    imgConvertCtx = sws_getContext(w, h, video->videoStream->codec->pix_fmt, w, h, dstPixFmt, SWS_BICUBIC, NULL, NULL, NULL);
	    if(imgConvertCtx == NULL) {
		fprintf(stderr, "Cannot init the conversion context!\n");
		exit(-1);
	    }
	}
	sws_scale(imgConvertCtx, pFrame->data, pFrame->linesize, 0, video->videoStream->codec->height, pict.data, pict.linesize);
	SDL_UnlockYUVOverlay(vf->bmp);
        //vp->pts = pts;
	if(++video->vFrameqWindex == VIDEO_PICTURE_QUEUE_SIZE) {
	    video->vFrameqWindex = 0;
	}
	SDL_LockMutex(video->vFrameqMutex);
	video->vFrameqSize++;
	SDL_UnlockMutex(video->vFrameqMutex);
    }
    return 0;
}

int audio_frame_queue()
{
    
}

int init_screen(VideoState *video)
{
    screen = SDL_SetVideoMode(video->pCodecCtx->width, video->pCodecCtx->height, 0, 0);
    if(!screen) {
	fprintf(stderr, "SDL: cannot init video mode\n");
	exit(1);
    }
    return 0;
}

int init_frame(VideoState *video) {
    VideoFrame *vf;
    vf = &video->vFrameq[video->vFrameqWindex];
    if(vf->bmp) {
	SDL_FreeYUVOverlay(vf->bmp);
    }
    vf->bmp = SDL_CreateYUVOverlay(video->videoStream->codec->width,
				   video->videoStream->codec->height,
				   SDL_YV12_OVERLAY,
				   screen);
    vf->width = video->videoStream->codec->width;
    vf->height = video->videoStream->codec->height;
    SDL_LockMutex(video->vFrameqMutex);
    vf->allocated = 1;
    SDL_CondSignal(video->vFrameqCond);
    SDL_UnlockMutex(video->vFrameqMutex);
}

int play_video(void *arg)
{
    SDL_Rect rect;
    VideoFrame *vf;
    AVPicture pict;
    int w, h, x, y;
    VideoState *video = (VideoState *)arg;
    vf = &video->vFrameq[video->vFrameqRindex];
    rect.x = 0;
    rect.y = 0;
    rect.w = video->pCodecCtx->width;
    rect.h = video->pCodecCtx->height;
    SDL_DisplayYUVOverlay(vf->bmp, &rect);
    if(++video->vFrameqRindex == VIDEO_FRAME_QUEUE_SIZE) {
	video->vFrameqRindex = 0;
    }
    SDL_LockMutex(video->vFrameqMutex);
    video->vFrameqSize--;
    SDL_CondSignal(video->vFrameqCond);
    SDL_UnlockMutex(video->vFrameqMutex);
}

int play_audio()
{
    
}

int main(int argc, char **argv)
{
/*    PacketQueue *a = av_mallocz(sizeof(PacketQueue));
      AVPacket *p = av_mallocz(sizeof(AVPacket));
      AVPacket *b = av_mallocz(sizeof(AVPacket));
      add_to_queue(a, p);
      add_to_queue(a, p);
      add_to_queue(a, p);
      add_to_queue(a, p);
      get_from_queue(a, b);
      add_to_queue(a, p);

      get_from_queue(a, b);
      get_from_queue(a, b);
      get_from_queue(a, b);
      get_from_queue(a, b);
      av_free(a);
      av_free(p);
*/
    

    FileState *file = av_mallocz(sizeof(FileState));
    VideoState *video = av_mallocz(sizeof(VideoState));
    AudioState *audio = av_mallocz(sizeof(AudioState));
    State *state = av_mallocz(sizeof(State));
    SDL_Thread *video_decode_tid;
    SDL_Thread *read_pkt_tid;
    SDL_Thread *play_tid;
    state->file = file;
    state->video = video;
    state->audio = audio;
    
    if(argc < 2) {
	fprintf(stderr, "Usage : play <file>\n");
	exit(1);
    }
    av_register_all();
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
	fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
	exit(1);
    }
    av_strlcpy(file->fileName, argv[1], sizeof(file->fileName));
    video->vFrameqMutex = SDL_CreateMutex();
    video->vFrameqCond = SDL_CreateCond();
    get_file_info(file);
    find_av_streams(file, video, audio);
    
    //find_audio_decoder(audio);
    init_screen(video);

    read_pkt_tid = SDL_CreateThread(queue_av_pkt, state);
    init_frame(video);

    video_decode_tid = SDL_CreateThread(decode_video, video);
/*
    video->frame_timer = (double)av_gettime() / 1000000.0;
    video->frame_last_delay = 40e-3;

    video->pCodecCtx->get_buffer = our_get_buffer;
    video->pCodecCtx->release_buffer = our_release_buffer;*/
//    play_tid = SDL_CreateThread(play_video, video);
    while(true) {
//	decode_video(video);
	play_video(video);	
    }
	

    sleep(10);
    if(!video_decode_tid) {
	av_free(video_decode_tid);
	return -1;
    }
    if(!read_pkt_tid) {
	av_free(read_pkt_tid);
	return -1;
    }
    return 0;
}
