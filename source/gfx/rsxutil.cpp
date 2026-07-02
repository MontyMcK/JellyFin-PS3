#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <sysutil/video.h>

#include "rsxutil.h"

extern void crash_log(const char *msg);

#define GCM_LABEL_INDEX		255

videoResolution res;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;

u32 display_width;
u32 display_height;

u32 depth_pitch;
u32 depth_offset;
u32 *depth_buffer;

u32 color_pitch;
u32 color_offset[2];
u32 *color_buffer[2];

static u32 sLabelVal = 1;

static void waitFinish()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);

	rsxFlushBuffer(context);

	while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX)!=sLabelVal)
		usleep(30);

	++sLabelVal;
}

static void waitRSXIdle()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);
	rsxSetWaitLabel(context,GCM_LABEL_INDEX,sLabelVal);

	++sLabelVal;

	waitFinish();
}

void setRenderTarget(u32 index)
{
	gcmSurface sf;

	sf.colorFormat		= GCM_SURFACE_X8R8G8B8;
	sf.colorTarget		= GCM_SURFACE_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index];
	sf.colorPitch[0]	= color_pitch;

	sf.colorLocation[1]	= GCM_LOCATION_RSX;
	sf.colorLocation[2]	= GCM_LOCATION_RSX;
	sf.colorLocation[3]	= GCM_LOCATION_RSX;
	sf.colorOffset[1]	= 0;
	sf.colorOffset[2]	= 0;
	sf.colorOffset[3]	= 0;
	sf.colorPitch[1]	= 64;
	sf.colorPitch[2]	= 64;
	sf.colorPitch[3]	= 64;

	sf.depthFormat		= GCM_SURFACE_ZETA_Z16;
	sf.depthLocation	= GCM_LOCATION_RSX;
	sf.depthOffset		= depth_offset;
	sf.depthPitch		= depth_pitch;

	sf.type				= GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias		= GCM_SURFACE_CENTER_1;

	sf.width			= display_width;
	sf.height			= display_height;
	sf.x				= 0;
	sf.y				= 0;

	rsxSetSurface(context,&sf);

	// Map clip-space [-1,1] to the full framebuffer.  Without an explicit
	// viewport the RSX keeps a stale/default transform on real hardware — GPU
	// geometry (e.g. the XMB wave) then renders mis-scaled and rides up the
	// screen, even though RPCS3 defaults to a full-surface viewport so it looks
	// fine in the emulator.  Same scale/offset the video player sets.
	float vp_sc[4]  = { display_width * 0.5f, -(float)display_height * 0.5f, 0.5f, 0.0f };
	float vp_off[4] = { display_width * 0.5f,  (float)display_height * 0.5f, 0.5f, 0.0f };
	rsxSetViewport(context, 0, 0, (u16)display_width, (u16)display_height,
	               0.0f, 1.0f, vp_sc, vp_off);
}

void init_screen(void *host_addr,u32 size)
{
	crash_log("2.1 rsxInit");
	rsxInit(&context,CB_SIZE,size,host_addr);

	crash_log("2.2 videoGetState");
	videoState state;
	videoGetState(0,0,&state);

	videoGetResolution(state.displayMode.resolution,&res);

	videoConfiguration vconfig;
	memset(&vconfig,0,sizeof(videoConfiguration));

	vconfig.resolution = state.displayMode.resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width*sizeof(u32);

	waitRSXIdle();

	crash_log("2.3 videoConfigure");
	videoConfigure(0,&vconfig,NULL,0);
	videoGetState(0,0,&state);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	display_width = res.width;
	display_height = res.height;
	{
		char b[64];
		snprintf(b,sizeof(b),"2.4 res %ux%u",display_width,display_height);
		crash_log(b);
	}

	color_pitch = display_width*sizeof(u32);
	crash_log("2.5 rsxMemalign color");
	color_buffer[0] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	color_buffer[1] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	crash_log((color_buffer[0]&&color_buffer[1]) ? "2.5b color ok" : "2.5b color NULL");

	rsxAddressToOffset(color_buffer[0],&color_offset[0]);
	rsxAddressToOffset(color_buffer[1],&color_offset[1]);

	gcmSetDisplayBuffer(0,color_offset[0],color_pitch,display_width,display_height);
	gcmSetDisplayBuffer(1,color_offset[1],color_pitch,display_width,display_height);

	depth_pitch = display_width*sizeof(u32);
	crash_log("2.6 rsxMemalign depth");
	depth_buffer = (u32*)rsxMemalign(64,(display_height*depth_pitch)*2);
	rsxAddressToOffset(depth_buffer,&depth_offset);
	crash_log("2.7 init_screen done");
}

void waitflip()
{
	// Poll at 50 µs (was 200 µs) — tighter interval reduces post-vblank entry
	// latency, giving the display thread more of the frame interval for render work.
	while(gcmGetFlipStatus()!=0)
		usleep(50);
	gcmResetFlipStatus();
}

void rsxSync(void)
{
	rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
	rsxFlushBuffer(context);
	int iters = 3334; // ~100 ms at 30 us/poll
	while (*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal) {
		usleep(30);
		if (--iters <= 0) break;
	}
	++sLabelVal;
}

void flip()
{
	if(first_fb) gcmResetFlipStatus();

	gcmSetFlip(context,curr_fb);
	rsxFlushBuffer(context);

	gcmSetWaitFlip(context);

	curr_fb ^= 1;
	setRenderTarget(curr_fb);

	first_fb = 0;
}
