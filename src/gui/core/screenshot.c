#include "screenshot.h"
#include "vga.h"
#include "mouse.h"
#include "timer.h"
#include "fs.h"
#include "gui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { SS_IDLE, SS_CHOOSE, SS_REGION } ss_mode_t;
static ss_mode_t mode = SS_IDLE;
static int sx, sy;
static bool pending;
static int px0, py0, px1, py1;
static unsigned serial_no;

static void le16(unsigned char *p, unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void le32(unsigned char *p, unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
static void clamp_point(int *x,int *y){if(*x<0)*x=0;if(*y<0)*y=0;if((uint64_t)*x>=SCREEN_W)*x=(int)SCREEN_W-1;if((uint64_t)*y>=SCREEN_H)*y=(int)SCREEN_H-1;}
static void request_capture(int x0,int y0,int x1,int y1){clamp_point(&x0,&y0);clamp_point(&x1,&y1);px0=x0< x1?x0:x1;px1=x0< x1?x1:x0;py0=y0< y1?y0:y1;py1=y0< y1?y1:y0;pending=true;mode=SS_IDLE;}
static void save_bmp(int x0,int y0,int x1,int y1){int w=x1-x0+1,h=y1-y0+1;if(w<1||h<1)return;uint32_t stride=(uint32_t)w*4u;uint32_t pad=0;uint32_t data=stride*(uint32_t)h;uint32_t total=54u+data;unsigned char *b=malloc(total);if(!b)return;memset(b,0,total);b[0]='B';b[1]='M';le32(b+2,total);le32(b+10,54);le32(b+14,40);le32(b+18,(unsigned)w);le32(b+22,(unsigned)h);le16(b+26,1);le16(b+28,32);le32(b+34,data);for(int y=0;y<h;y++){unsigned char *row=b+54+(size_t)y*(stride+pad);int syy=y1-y;for(int x=0;x<w;x++){uint32_t c=(uint32_t)vga_get_pixel(x0+x,syy);row[x*4]=(unsigned char)c;row[x*4+1]=(unsigned char)(c>>8);row[x*4+2]=(unsigned char)(c>>16);row[x*4+3]=255;}}char name[64];snprintf(name,sizeof(name),"Screenshot-%u.bmp",++serial_no);if(!fs_write_file_at("/screenshots",name,(const char*)b,total))gui_notify("Screenshot could not be saved",1800);else gui_notify("Screenshot saved to /screenshots",1800);free(b);}
void screenshot_begin(void){(void)fs_create_dir_at("/","screenshots");mode=SS_CHOOSE;pending=false;gui_notify("Screenshot: click Full screen or Select area",2200);}
void screenshot_handle_mouse(void){if(mode==SS_CHOOSE&&mouse.left_click){if(mouse.x>=((int)SCREEN_W/2-190)&&mouse.x<((int)SCREEN_W/2-20)&&mouse.y>=80&&mouse.y<130){request_capture(0,0,(int)SCREEN_W-1,(int)SCREEN_H-1);return;}if(mouse.x>=((int)SCREEN_W/2+20)&&mouse.x<((int)SCREEN_W/2+190)&&mouse.y>=80&&mouse.y<130){mode=SS_REGION;gui_notify("Drag a rectangle, then release",2200);return;}}else if(mode==SS_REGION){if(mouse.left_click){sx=(int)mouse.x;sy=(int)mouse.y;}if(mouse.left_release){request_capture(sx,sy,(int)mouse.x,(int)mouse.y);}}}
void screenshot_service_before_overlay(void){if(pending){save_bmp(px0,py0,px1,py1);pending=false;}}

void screenshot_draw_overlay(void){if(mode==SS_CHOOSE){int cx=(int)SCREEN_W/2;vga_fill_rect(cx-210,62,420,84,0xE91B2430);vga_draw_rect(cx-210,62,420,84,0xFF5B6B86);vga_fill_rect(cx-190,80,170,50,0xFF2563EB);vga_fill_rect(cx+20,80,170,50,0xFF334155);vga_draw_string(cx-174,99,"Full screen",0xFFFFFFFF,0xFF2563EB);vga_draw_string(cx+38,99,"Select area",0xFFFFFFFF,0xFF334155);}else if(mode==SS_REGION&&mouse.left){int x0=sx<(int)mouse.x?sx:(int)mouse.x,x1=sx<(int)mouse.x?(int)mouse.x:sx;int y0=sy<(int)mouse.y?sy:(int)mouse.y,y1=sy<(int)mouse.y?(int)mouse.y:sy;vga_draw_rect(x0,y0,x1-x0+1,y1-y0+1,0xFF38BDF8);}}
