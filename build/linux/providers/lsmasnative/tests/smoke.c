#include "lsmas_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){ fprintf(stderr,"FAIL %s:%d %s error=%s\n",__FILE__,__LINE__,#x,err?err:"");exit(1);} }while(0)
static uint64_t hash(const uint8_t*p,size_t n){uint64_t v=1469598103934665603ULL;while(n--)v=(v^*p++)*1099511628211ULL;return v;}
int main(int argc,char**argv){
 char *err=NULL; CHECK(argc>1);CHECK(lsmas_get_api_version()==LSMAS_NATIVE_API_VERSION);
 char*vers=lsmas_get_versions_json_utf8(&err);CHECK(vers);puts(vers);lsmas_free(vers);
 for(int a=1;a<argc;a++)for(int pass=0;pass<2;pass++){
  char*probe=lsmas_probe_streams_json_utf8(argv[a],&err);CHECK(probe);CHECK(strstr(probe,"video"));lsmas_free(probe);
  lsmas_video_open_options_t vo={0};vo.stream_index=-1;vo.threads=2;vo.seek_threshold=10;vo.fpsden=1;vo.cache_index=1;vo.soft_reset=1;vo.repeat=1;vo.ff_loglevel=-8;
  lsmas_handle_t*h=lsmas_video_open_utf8(argv[a],&vo,&err);CHECK(h);
  lsmas_video_info_t vi;CHECK(lsmas_video_get_info(h,&vi,&err)==0);CHECK(vi.width>0&&vi.height>0&&vi.num_frames>1);
  int32_t n=0,tn=0,td=0;CHECK(lsmas_video_get_source_frame_count(h,&n,&err)==0);CHECK(n>0);
  CHECK(lsmas_video_get_time_base(h,&tn,&td,&err)==0);CHECK(tn>0&&td>0);
  int64_t *pts=calloc(vi.num_frames,sizeof(*pts));CHECK(lsmas_video_get_pts_list(h,pts,vi.num_frames,&err)==vi.num_frames);
  uint8_t *keys=calloc(n,1);CHECK(lsmas_video_get_source_keyframe_flags(h,keys,n,&err)>=0);free(keys);free(pts);
  for(int f=0;f<=5;f++){
   int order[]={0,vi.num_frames-1,vi.num_frames/2,0};uint64_t first=0;
   for(int i=0;i<4;i++){
    lsmas_video_frame_buffer_layout_t l;int64_t size=lsmas_video_get_frame(h,order[i],f,NULL,0,&l,&err);CHECK(size>0);CHECK(l.width==vi.width&&l.height==vi.height);
    uint8_t*b=malloc(size);CHECK(lsmas_video_get_frame(h,order[i],f,b,0,&l,&err)==size);uint64_t sum=hash(b,size);free(b);if(i==0)first=sum;if(i==3)CHECK(first==sum);
   }
  }
  lsmas_video_frame_t*frame=NULL;CHECK(lsmas_video_acquire_avframe(h,vi.num_frames/2,&frame,&err)==0);CHECK(frame&&lsmas_video_frame_get_avframe(frame));
  lsmas_video_frame_props_t fp;CHECK(lsmas_video_frame_get_props(frame,&fp,&err)==0);CHECK(fp.width==vi.width&&fp.height==vi.height);
  lsmas_video_format_info_t fi;CHECK(lsmas_video_frame_get_format_info(frame,&fi,&err)==0);CHECK(fi.plane_count>0);
  for(int p=0;p<fi.plane_count;p++){const uint8_t*data;int32_t stride,w,ht;CHECK(lsmas_video_frame_get_plane(frame,p,&data,&stride,&w,&ht,&err)==0);CHECK(data&&stride>0&&w>0&&ht>0);}
  CHECK(lsmas_video_frame_get_pix_fmt_name(frame));lsmas_video_release_frame(frame);lsmas_video_close(h);
  lsmas_audio_open_options_t ao={0};ao.stream_index=-1;ao.threads=2;ao.av_sync=1;ao.ff_loglevel=-8;ao.cache_index=1;ao.sample_format=LSMAS_AUDIO_S16;
  h=lsmas_audio_open_utf8(argv[a],&ao,&err);CHECK(h);lsmas_audio_info_t ai;CHECK(lsmas_audio_get_info(h,&ai,&err)==0);CHECK(ai.total_samples>1024&&ai.block_align>0);
  int64_t starts[]={0,ai.total_samples-1024,ai.total_samples/2,0};uint64_t first=0;
  for(int i=0;i<4;i++){uint8_t*b=calloc(1024,ai.block_align);CHECK(lsmas_audio_get_samples(h,b,starts[i],1024,&err)==1024);uint64_t sum=hash(b,1024*ai.block_align);int nz=0;for(int j=0;j<1024*ai.block_align;j++)nz|=b[j];CHECK(nz);free(b);if(i==0)first=sum;if(i==3)CHECK(first==sum);}
  lsmas_audio_close(h);printf("PASS %s cache-pass=%d video=%dx%d/%d audio=%dHz/%dch/%lldsamples outputs=native,gray8,bgra,rgba,gray8p16,yuv420p8\n",argv[a],pass,vi.width,vi.height,vi.num_frames,ai.sample_rate,ai.channels,(long long)ai.total_samples);
 }
 lsmas_handle_t*bad=lsmas_video_open_utf8("/definitely/not/a/media-file",NULL,&err);CHECK(!bad&&err);lsmas_free(err);err=NULL;puts("PASS invalid-path handling");return 0;
}
