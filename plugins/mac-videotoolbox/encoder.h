#include "obs.h"
#include <obs-module.h>

#include <util/darray.h>
#include <util/platform.h>

#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <VideoToolbox/VTVideoEncoderList.h>
#include <CoreMedia/CoreMedia.h>

#include <util/apple/cfstring-utils.h>

#include <obs-avc.h>
#include <assert.h>

#define TEXT_VT_ENCODER obs_module_text("VTEncoder")
#define TEXT_BITRATE obs_module_text("Bitrate")
#define TEXT_QUALITY obs_module_text("Quality")
#define TEXT_USE_MAX_BITRATE obs_module_text("UseMaxBitrate")
#define TEXT_MAX_BITRATE obs_module_text("MaxBitrate")
#define TEXT_MAX_BITRATE_WINDOW obs_module_text("MaxBitrateWindow")
#define TEXT_KEYINT_SEC obs_module_text("KeyframeIntervalSec")
#define TEXT_PROFILE obs_module_text("Profile")
#define TEXT_NONE obs_module_text("None")
#define TEXT_DEFAULT obs_module_text("DefaultEncoder")
#define TEXT_BFRAMES obs_module_text("UseBFrames")
#define TEXT_RATE_CONTROL obs_module_text("RateControl")

struct vt_encoder_type_data {
	const char *name;
	const char *disp_name;
	const char *id;
	const char *codec_name;

	bool hardware_accelerated;
};

typedef OSStatus (*GETParameterSetAtIndex)(
	CMFormatDescriptionRef videoDesc, size_t parameterSetIndex,
	const uint8_t **parameterSetPointerOut, size_t *parameterSetSizeOut,
	size_t *parameterSetCountOut, int *NALUnitHeaderLengthOut);

struct vt_encoder {
	obs_encoder_t *encoder;

	const char *vt_encoder_id;
	uint32_t width;
	uint32_t height;
	uint32_t keyint;
	uint32_t fps_num;
	uint32_t fps_den;
	const char *rate_control;
	uint32_t bitrate;
	float quality;
	bool limit_bitrate;
	uint32_t rc_max_bitrate;
	float rc_max_bitrate_window;
	const char *profile;
	bool bframes;

	int vt_pix_fmt;
	enum video_colorspace colorspace;

	// manually adjusting the priority?
	bool handle_priority;

	VTCompressionSessionRef session;
	GETParameterSetAtIndex get_parameter_set_at_index;
	CMSimpleQueueRef queue;
	bool hw_enc;
	DARRAY(uint8_t) packet_data;
	DARRAY(uint8_t) extra_data;
};

void log_osstatus(int log_level, struct vt_encoder *enc, const char *context,
		  OSStatus code);

#define STATUS_CHECK(c)                         \
	code = c;                                   \
	if (code) {                                 \
		log_osstatus(LOG_ERROR, enc, #c, code); \
		goto fail;                              \
	}

#define SESSION_CHECK(x)       \
	if ((code = (x)) != noErr) \
		return code;

#ifndef VT_LOG_ENCODER_NAME
#define VT_LOG_ENCODER_NAME "video-encoder"
#endif

#define VT_LOG(level, format, ...) \
	blog(level, "[VideoToolbox encoder]: " format, ##__VA_ARGS__)
#define VT_LOG_ENCODER(encoder, level, format, ...)                           \
	blog(level, "[VideoToolbox %s: '" VT_LOG_ENCODER_NAME " ']: " format, \
	     obs_encoder_get_name(encoder), ##__VA_ARGS__)
#define VT_BLOG(level, format, ...) \
	VT_LOG_ENCODER(enc->encoder, level, format, ##__VA_ARGS__)

///////
CFStringRef obs_to_vt_profile(const char *profile);
CFStringRef obs_to_vt_colorspace(enum video_colorspace cs);

OSStatus session_set_prop_float(VTCompressionSessionRef session,
				CFStringRef key, float val);
OSStatus session_set_prop_int(VTCompressionSessionRef session, CFStringRef key,
			      int32_t val);
OSStatus session_set_prop_str(VTCompressionSessionRef session, CFStringRef key,
			      char *val);
OSStatus session_set_prop(VTCompressionSessionRef session, CFStringRef key,
			  CFTypeRef val);

OSStatus session_set_bitrate(VTCompressionSessionRef session,
			     const char *rate_control, int new_bitrate,
			     float quality, bool limit_bitrate, int max_bitrate,
			     float max_bitrate_window);

OSStatus session_set_colorspace(VTCompressionSessionRef session,
				enum video_colorspace cs);

void sample_encoded_callback(void *data, void *source, OSStatus status,
			     VTEncodeInfoFlags info_flags,
			     CMSampleBufferRef buffer);

bool create_encoder(struct vt_encoder *enc, int codec_id, bool realtime);
void vt_destroy(void *data);
void dump_encoder_info(struct vt_encoder *enc);
bool get_cached_pixel_buffer(struct vt_encoder *enc, CVPixelBufferRef *buf);

bool vt_encode(void *data, struct encoder_frame *frame,
	       struct encoder_packet *packet, bool *received_packet);
bool vt_extra_data(void *data, uint8_t **extra_data, size_t *size);
bool update_params(struct vt_encoder *enc, obs_data_t *settings);
bool vt_update(void *data, obs_data_t *settings);
