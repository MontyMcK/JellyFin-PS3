#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <sysutil/video.h>

#include "rsxutil.h"
#include "plog.h"

extern void crash_log(const char *msg);

#define GCM_LABEL_INDEX		255

videoResolution res;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;

u32 display_width;
u32 display_height;

u32 display_par_num = 1;
u32 display_par_den = 1;

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

	// Pixel aspect = display aspect (4:3 or 16:9) divided by the framebuffer
	// aspect.  1:1 on HD modes; 720×576/720×480 SD modes come out non-square.
	{
		u32 dar_w = 16, dar_h = 9;
		if (state.displayMode.aspect == VIDEO_ASPECT_4_3) { dar_w = 4; dar_h = 3; }
		display_par_num = dar_w * display_height;
		display_par_den = dar_h * display_width;
		if (display_par_num == display_par_den)
			display_par_num = display_par_den = 1;
	}
	{
		char b[80];
		snprintf(b,sizeof(b),"2.4 res %ux%u aspect=%u par=%u/%u",
		         display_width,display_height,
		         (unsigned)state.displayMode.aspect,
		         display_par_num,display_par_den);
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

bool waitflip_timeout(u32 timeout_us)
{
	u32 waited = 0;
	while (gcmGetFlipStatus() != 0) {
		if (waited >= timeout_us) return false;
		usleep(50);
		waited += 50;
	}
	gcmResetFlipStatus();
	return true;
}

void rsx_rebind_display(void)
{
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	gcmSetDisplayBuffer(0,color_offset[0],color_pitch,display_width,display_height);
	gcmSetDisplayBuffer(1,color_offset[1],color_pitch,display_width,display_height);
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

// 24Hz OUTPUT.  Read docs/24P_OUTPUT.md before touching the display mode.
//
// 2026-09-18: videoConfigure() with resolution id 0x82 configured "cleanly"
// (1920x1080, refresh bit 0x40), the panel showed no picture, flip() never
// returned and the console needed a hard power-off.  Two corrections to what
// was concluded from that at the time:
//
//   * 0x82 was never a 24Hz mode.  The panel advertises it with ONLY bit 0x40.
//     The 24Hz family is bits 0x10|0x20: the only rates of id 0x83 (1080p
//     frame packing, defined by HDMI 1.4 at 23.98/24 alone), and also present
//     on id 1 -- plain 1080 -- on this TV.  What 0x82/0x40 is remains unknown.
//   * flip() hanging means the RSX stopped completing flips, i.e. the scan-out
//     side, not "the TV could not lock": vblank comes from the console's own
//     display timing, not from the panel.
//
// And the finding that stands: videoConfigure() REJECTS id 0x83 with
// 0x8002b226 (UNSUPPORTED_DISPLAY_MODE) although availability says yes --
// consistent with the v1 call being unable to select a 24Hz-family rate at
// all.  The v1 struct has no refresh field; cellVideoOutConfigure2 is the only
// candidate and its ABI is unknown (avconf_capture.cpp exists to read it).
//
// If a switch is ever attempted: never draw or flip while the new mode is up,
// verify it by timing the vblank handler's counter against the timebase (no
// flip involved), and write the abort marker BEFORE changing anything.

// Called from main() once plog is open.  This USED to run inside
// init_screen(), which happens long before logging exists, so every
// line was thrown away and the log showed nothing at all.
void video_log_capabilities(void)
{
	videoState state;
	memset(&state, 0, sizeof(state));
	videoGetState(0, 0, &state);
	// Ask the DISPLAY what it can do, rather than trusting a header.
	//
	// 24fps film on a 60Hz output needs 3:2 pulldown, and that is the judder.
	// A Blu-ray player avoids it by switching the TV to 1080p24 -- firmware
	// 3.30 added that mode for HDMI 1.4 3D.  But neither PSL1GHT nor the real
	// SDK enum (see RPCS3's cellVideoOut.h) has ANY 24 Hz refresh value:
	// AUTO/59.94/50/60/30 is the whole list, and videoConfiguration carries
	// no refresh field to request one with anyway.
	//
	// So this dumps every mode the panel advertises. If a rate bit outside
	// the known four ever shows up, that is the lead worth chasing; if it
	// never does, the answer is settled by the hardware instead of by me.
	// videoGetResolutionAvailability() then probes the resolution ids that
	// are NOT in PSL1GHT's list, including the 3D frame-packing ones, since
	// probing is read-only and cannot blank the screen the way configuring
	// an unsupported mode would.
	{
		videoDeviceInfo di;
		memset(&di, 0, sizeof(di));
		if (videoGetDeviceInfo(0, 0, &di) == 0) {
			char b[160];
			snprintf(b, sizeof(b),
			         "video: current res=%u rate=0x%02x, %u modes advertised"
			         " (rate bits: 1=59.94 2=50 4=60 8=30)",
			         (unsigned)state.displayMode.resolution,
			         (unsigned)state.displayMode.refreshRates,
			         (unsigned)di.availableModeCount);
			plog(b);
			int n = di.availableModeCount;
			if (n > 32) n = 32;
			for (int i = 0; i < n; i++) {
				snprintf(b, sizeof(b),
				         "video:   mode[%d] res=%u rates=0x%04x aspect=%u",
				         i, (unsigned)di.availableModes[i].resolution,
				         (unsigned)di.availableModes[i].refreshRates,
				         (unsigned)di.availableModes[i].aspect);
				plog(b);
			}
		}
		// Resolution ids the SDK defines but PSL1GHT does not name, plus the
		// 3D frame-packing ones.  Availability is a query, not a change.
		// 0x82/0x83 are the ids the panel's own list advertises beyond the
		// SDK's: 0x83 (1920x2205, 1080p frame packing) with ONLY rate bits
		// 0x10|0x20 -- the 24Hz family, which of the two is 23.976 is not
		// established -- and 0x82 (1920x1080) with only 0x40, meaning
		// unknown.  Availability is a query, not a change.
		static const u32 kProbe[] = {
			1 /*1080*/, 2 /*720*/, 0x81 /*720 3D FP*/,
			0x82 /*1920x1080, rate 0x40 only*/, 0x83 /*1080p FP, 24Hz family*/,
			0x88, 0x89, 0x8a, 0x8b,
			0x91 /*720 dualview*/, 0x98, 0x99,
		};
		char b[160]; int n = 0;
		char list[128]; list[0] = '\0';
		for (unsigned i = 0; i < sizeof(kProbe)/sizeof(kProbe[0]); i++) {
			if (videoGetResolutionAvailability(0, kProbe[i],
			                                   VIDEO_ASPECT_AUTO, 0) == 1) {
				n += snprintf(list + n, sizeof(list) - n, "0x%02x ",
				              (unsigned)kProbe[i]);
				if (n >= (int)sizeof(list) - 8) break;
			}
		}
		snprintf(b, sizeof(b), "video: resolutions available: %s", list);
		plog(b);
	}
}
