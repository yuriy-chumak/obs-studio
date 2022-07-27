#include "encoder.h"

bool obs_module_load_h264(void);

#ifdef ENABLE_HEVC
bool obs_module_load_h265(void);
void obs_module_unload_h265(void);
#endif

bool obs_module_load(void)
{
	obs_module_load_h264();
#ifdef ENABLE_HEVC
	obs_module_load_h265();
#endif
	return true;
}

void obs_module_unload(void)
{
#ifdef ENABLE_HEVC
	obs_module_unload_h265();
#endif
}

//////////

void log_osstatus(int log_level, struct vt_encoder *enc, const char *context,
		  OSStatus code)
{
	char *c_str = NULL;
	CFErrorRef err = CFErrorCreate(kCFAllocatorDefault,
				       kCFErrorDomainOSStatus, code, NULL);
	CFStringRef str = CFErrorCopyDescription(err);

	c_str = cfstr_copy_cstr(str, kCFStringEncodingUTF8);
	if (c_str) {
		if (enc)
			VT_BLOG(log_level, "Error in %s: %s", context, c_str);
		else
			VT_LOG(log_level, "Error in %s: %s", context, c_str);
	}

	bfree(c_str);
	CFRelease(str);
	CFRelease(err);
}

CFStringRef obs_to_vt_profile(const char *profile)
{
	if (strcmp(profile, "baseline") == 0)
		return kVTProfileLevel_H264_Baseline_AutoLevel;
	else if (strcmp(profile, "main") == 0)
		return kVTProfileLevel_H264_Main_AutoLevel;
	else if (strcmp(profile, "high") == 0)
		return kVTProfileLevel_H264_High_AutoLevel;
	else if (strcmp(profile, "main10") == 0)
		return kVTProfileLevel_HEVC_Main10_AutoLevel;
	else
		return NULL;
}

CFStringRef obs_to_vt_colorspace(enum video_colorspace cs)
{
	if (cs == VIDEO_CS_709)
		return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
	else if (cs == VIDEO_CS_601)
		return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
	else if ((cs == VIDEO_CS_2100_PQ) || (cs == VIDEO_CS_2100_HLG))
		return kCVImageBufferYCbCrMatrix_ITU_R_2020;
	return NULL;
}

OSStatus session_set_prop_float(VTCompressionSessionRef session,
				CFStringRef key, float val)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &val);
	OSStatus code = VTSessionSetProperty(session, key, n);
	CFRelease(n);

	return code;
}

OSStatus session_set_prop_int(VTCompressionSessionRef session, CFStringRef key,
			      int32_t val)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &val);
	OSStatus code = VTSessionSetProperty(session, key, n);
	CFRelease(n);

	return code;
}

OSStatus session_set_prop_str(VTCompressionSessionRef session, CFStringRef key,
			      char *val)
{
	CFStringRef s = CFStringCreateWithFileSystemRepresentation(NULL, val);
	OSStatus code = VTSessionSetProperty(session, key, s);
	CFRelease(s);

	return code;
}

OSStatus session_set_prop(VTCompressionSessionRef session, CFStringRef key,
			  CFTypeRef val)
{
	return VTSessionSetProperty(session, key, val);
}

OSStatus session_set_bitrate(VTCompressionSessionRef session,
			     const char *rate_control, int new_bitrate,
			     float quality, bool limit_bitrate, int max_bitrate,
			     float max_bitrate_window)
{
	OSStatus code;

	bool can_limit_bitrate;
	CFStringRef compressionPropertyKey;

	if (strcmp(rate_control, "CBR") == 0) {
		compressionPropertyKey =
			kVTCompressionPropertyKey_AverageBitRate;
		can_limit_bitrate = true;

		if (__builtin_available(macOS 13.0, *)) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
#ifdef __aarch64__
			if (true) {
#else
			if (os_get_emulation_status() == true) {
#endif
				compressionPropertyKey =
					kVTCompressionPropertyKey_ConstantBitRate;
				can_limit_bitrate = false;
			} else {
				VT_LOG(LOG_WARNING,
				       "CBR support for VideoToolbox encoder requires Apple Silicon. "
				       "Will use ABR instead.");
			}
#else
			VT_LOG(LOG_WARNING,
			       "CBR support for VideoToolbox not available in this build of OBS. "
			       "Will use ABR instead.");
#endif
		} else {
			VT_LOG(LOG_WARNING,
			       "CBR support for VideoToolbox encoder requires macOS 13 or newer. "
			       "Will use ABR instead.");
		}
	} else if (strcmp(rate_control, "ABR") == 0) {
		compressionPropertyKey =
			kVTCompressionPropertyKey_AverageBitRate;
		can_limit_bitrate = true;
	} else if (strcmp(rate_control, "CRF") == 0) {
#ifdef __aarch64__
		if (true) {
#else
		if (os_get_emulation_status() == true) {
#endif
			compressionPropertyKey =
				kVTCompressionPropertyKey_Quality;
			SESSION_CHECK(session_set_prop_float(
				session, compressionPropertyKey, quality));
		} else {
			VT_LOG(LOG_WARNING,
			       "CRF support for VideoToolbox encoder requires Apple Silicon. "
			       "Will use ABR instead.");
			compressionPropertyKey =
				kVTCompressionPropertyKey_AverageBitRate;
		}
		can_limit_bitrate = true;
	} else {
		VT_LOG(LOG_ERROR,
		       "Selected rate control method is not supported: %s",
		       rate_control);
		return kVTParameterErr;
	}

	if (compressionPropertyKey != kVTCompressionPropertyKey_Quality) {
		SESSION_CHECK(session_set_prop_int(
			session, compressionPropertyKey, new_bitrate * 1000));
	}

	if (limit_bitrate && can_limit_bitrate) {
		int32_t cpb_size = max_bitrate * 125 * max_bitrate_window;

		CFNumberRef cf_cpb_size =
			CFNumberCreate(NULL, kCFNumberIntType, &cpb_size);
		CFNumberRef cf_cpb_window_s = CFNumberCreate(
			NULL, kCFNumberFloatType, &max_bitrate_window);

		CFMutableArrayRef rate_control = CFArrayCreateMutable(
			kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);

		CFArrayAppendValue(rate_control, cf_cpb_size);
		CFArrayAppendValue(rate_control, cf_cpb_window_s);

		code = session_set_prop(
			session, kVTCompressionPropertyKey_DataRateLimits,
			rate_control);

		CFRelease(cf_cpb_size);
		CFRelease(cf_cpb_window_s);
		CFRelease(rate_control);

		if (code == kVTPropertyNotSupportedErr) {
			log_osstatus(LOG_WARNING, NULL,
				     "setting DataRateLimits on session", code);
			return noErr;
		}
	}

	return noErr;
}

OSStatus session_set_colorspace(VTCompressionSessionRef session,
				enum video_colorspace cs)
{
	CFStringRef matrix = obs_to_vt_colorspace(cs);
	OSStatus code;

	if (matrix != NULL) {
		if (cs == VIDEO_CS_2100_PQ) {
			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_ColorPrimaries,
				kCVImageBufferColorPrimaries_ITU_R_2020));

			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_TransferFunction,
				kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ));
		} else if (cs == VIDEO_CS_2100_HLG) {
			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_ColorPrimaries,
				kCVImageBufferColorPrimaries_ITU_R_2020));

			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_TransferFunction,
				kCVImageBufferTransferFunction_ITU_R_2100_HLG));
		} else {
			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_ColorPrimaries,
				kCVImageBufferColorPrimaries_ITU_R_709_2));
			SESSION_CHECK(session_set_prop(
				session,
				kVTCompressionPropertyKey_TransferFunction,
				kCVImageBufferTransferFunction_ITU_R_709_2));
		}
		SESSION_CHECK(session_set_prop(
			session, kVTCompressionPropertyKey_YCbCrMatrix,
			matrix));
	}

	return noErr;
}

void sample_encoded_callback(void *data, void *source, OSStatus status,
			     VTEncodeInfoFlags info_flags,
			     CMSampleBufferRef buffer)
{
	UNUSED_PARAMETER(status);
	UNUSED_PARAMETER(info_flags);

	CMSimpleQueueRef queue = data;
	CVPixelBufferRef pixbuf = source;
	if (buffer != NULL) {
		CFRetain(buffer);
		CMSimpleQueueEnqueue(queue, buffer);
	}
	CFRelease(pixbuf);
}
#define ENCODER_ID kVTVideoEncoderSpecification_EncoderID
#define ENABLE_HW_ACCEL \
	kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder
#define REQUIRE_HW_ACCEL \
	kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder
static inline CFMutableDictionaryRef
create_encoder_spec(const char *vt_encoder_id)
{
	CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);

	CFStringRef id =
		CFStringCreateWithFileSystemRepresentation(NULL, vt_encoder_id);
	CFDictionaryAddValue(encoder_spec, ENCODER_ID, id);
	CFRelease(id);

	CFDictionaryAddValue(encoder_spec, ENABLE_HW_ACCEL, kCFBooleanTrue);
	CFDictionaryAddValue(encoder_spec, REQUIRE_HW_ACCEL, kCFBooleanFalse);

	return encoder_spec;
}
#undef ENCODER_ID
#undef REQUIRE_HW_ACCEL
#undef ENABLE_HW_ACCEL

static inline CFMutableDictionaryRef create_pixbuf_spec(struct vt_encoder *enc)
{
	CFMutableDictionaryRef pixbuf_spec = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);

	CFNumberRef n =
		CFNumberCreate(NULL, kCFNumberSInt32Type, &enc->vt_pix_fmt);
	CFDictionaryAddValue(pixbuf_spec, kCVPixelBufferPixelFormatTypeKey, n);
	CFRelease(n);

	n = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc->width);
	CFDictionaryAddValue(pixbuf_spec, kCVPixelBufferWidthKey, n);
	CFRelease(n);

	n = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc->height);
	CFDictionaryAddValue(pixbuf_spec, kCVPixelBufferHeightKey, n);
	CFRelease(n);

	return pixbuf_spec;
}

bool create_encoder(struct vt_encoder *enc, int codec_id, bool realtime)
{
	OSStatus code;

	VTCompressionSessionRef s;

	CFDictionaryRef encoder_spec = create_encoder_spec(enc->vt_encoder_id);
	CFDictionaryRef pixbuf_spec = create_pixbuf_spec(enc);

	STATUS_CHECK(VTCompressionSessionCreate(
		kCFAllocatorDefault, enc->width, enc->height, codec_id,
		encoder_spec, pixbuf_spec, NULL, &sample_encoded_callback,
		enc->queue, &s));

	CFRelease(encoder_spec);
	CFRelease(pixbuf_spec);

	CFBooleanRef b = NULL;
	code = VTSessionCopyProperty(
		s,
		kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
		NULL, &b);

	if (code == noErr && (enc->hw_enc = CFBooleanGetValue(b)))
		VT_BLOG(LOG_INFO, "session created with hardware encoding");
	else
		enc->hw_enc = false;

	if (b != NULL)
		CFRelease(b);

	STATUS_CHECK(session_set_prop_int(
		s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
		enc->keyint));
	STATUS_CHECK(session_set_prop_int(
		s, kVTCompressionPropertyKey_MaxKeyFrameInterval,
		enc->keyint * ((float)enc->fps_num / enc->fps_den)));
	STATUS_CHECK(session_set_prop_float(
		s, kVTCompressionPropertyKey_ExpectedFrameRate,
		(float)enc->fps_num / enc->fps_den));
	STATUS_CHECK(session_set_prop(
		s, kVTCompressionPropertyKey_AllowFrameReordering,
		enc->bframes ? kCFBooleanTrue : kCFBooleanFalse));

	// This can fail depending on hardware configuration
	code = session_set_prop(s, kVTCompressionPropertyKey_RealTime,
				realtime ? kCFBooleanTrue : kCFBooleanFalse);
	if (code != noErr)
		log_osstatus(
			LOG_WARNING, enc,
			"setting kVTCompressionPropertyKey_RealTime failed, "
			"frame delay might be increased",
			code);

	CFStringRef profile = obs_to_vt_profile(enc->profile);
	if (profile == NULL) {
		if (codec_id == kCMVideoCodecType_H264)
			profile = kVTProfileLevel_H264_Main_AutoLevel;
		if (codec_id == kCMVideoCodecType_HEVC)
			profile = kVTProfileLevel_HEVC_Main_AutoLevel;
	}
	STATUS_CHECK(session_set_prop(s, kVTCompressionPropertyKey_ProfileLevel,
				      profile));

	STATUS_CHECK(session_set_bitrate(s, enc->rate_control, enc->bitrate,
					 enc->quality, enc->limit_bitrate,
					 enc->rc_max_bitrate,
					 enc->rc_max_bitrate_window));

	STATUS_CHECK(session_set_colorspace(s, enc->colorspace));

	STATUS_CHECK(VTCompressionSessionPrepareToEncodeFrames(s));

	enc->session = s;

	return true;

fail:
	if (encoder_spec != NULL)
		CFRelease(encoder_spec);
	if (pixbuf_spec != NULL)
		CFRelease(pixbuf_spec);

	return false;
}

void vt_destroy(void *data)
{
	struct vt_encoder *enc = data;

	if (enc) {
		if (enc->session != NULL) {
			VTCompressionSessionInvalidate(enc->session);
			CFRelease(enc->session);
		}
		da_free(enc->packet_data);
		da_free(enc->extra_data);
		bfree(enc);
	}
}

void dump_encoder_info(struct vt_encoder *enc)
{
	VT_BLOG(LOG_INFO,
		"settings:\n"
		"\tvt_encoder_id          %s\n"
		"\trate_control:          %s\n"
		"\tbitrate:               %d (kbps)\n"
		"\tquality:               %f\n"
		"\tfps_num:               %d\n"
		"\tfps_den:               %d\n"
		"\twidth:                 %d\n"
		"\theight:                %d\n"
		"\tkeyint:                %d (s)\n"
		"\tlimit_bitrate:         %s\n"
		"\trc_max_bitrate:        %d (kbps)\n"
		"\trc_max_bitrate_window: %f (s)\n"
		"\thw_enc:                %s\n"
		"\tprofile:               %s\n",
		enc->vt_encoder_id, enc->rate_control, enc->bitrate,
		enc->quality, enc->fps_num, enc->fps_den, enc->width,
		enc->height, enc->keyint, enc->limit_bitrate ? "on" : "off",
		enc->rc_max_bitrate, enc->rc_max_bitrate_window,
		enc->hw_enc ? "on" : "off",
		(enc->profile != NULL && !!strlen(enc->profile)) ? enc->profile
								 : "default");
}

static bool set_video_format(struct vt_encoder *enc, enum video_format format,
			     enum video_range_type range)
{
	bool full_range = range == VIDEO_RANGE_FULL;
	switch (format) {
	case VIDEO_FORMAT_I420:
		enc->vt_pix_fmt =
			full_range
				? kCVPixelFormatType_420YpCbCr8PlanarFullRange
				: kCVPixelFormatType_420YpCbCr8Planar;
		return true;
	case VIDEO_FORMAT_NV12:
		enc->vt_pix_fmt =
			full_range
				? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
				: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
		return true;
	case VIDEO_FORMAT_P010:
		enc->vt_pix_fmt =
			full_range
				? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
				: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
		return true;
	default:
		return false;
	}
}

bool update_params(struct vt_encoder *enc, obs_data_t *settings)
{
	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	if (!set_video_format(enc, voi->format, voi->range)) {
		obs_encoder_set_last_error(
			enc->encoder,
			obs_module_text("ColorFormatUnsupportedH264"));
		VT_BLOG(LOG_WARNING, "Unsupported color format selected");
		return false;
	}

	enc->colorspace = voi->colorspace;
	enc->width = obs_encoder_get_width(enc->encoder);
	enc->height = obs_encoder_get_height(enc->encoder);
	enc->fps_num = voi->fps_num;
	enc->fps_den = voi->fps_den;
	enc->keyint = (uint32_t)obs_data_get_int(settings, "keyint_sec");
	enc->rate_control = obs_data_get_string(settings, "rate_control");
	enc->bitrate = (uint32_t)obs_data_get_int(settings, "bitrate");
	enc->quality = ((float)obs_data_get_int(settings, "quality")) / 100;
	enc->profile = obs_data_get_string(settings, "profile");
	enc->limit_bitrate = obs_data_get_bool(settings, "limit_bitrate");
	enc->rc_max_bitrate = obs_data_get_int(settings, "max_bitrate");
	enc->rc_max_bitrate_window =
		obs_data_get_double(settings, "max_bitrate_window");
	enc->bframes = obs_data_get_bool(settings, "bframes");
	return true;
}

bool vt_update(void *data, obs_data_t *settings)
{
	struct vt_encoder *enc = data;

	uint32_t old_bitrate = enc->bitrate;
	bool old_limit_bitrate = enc->limit_bitrate;

	update_params(enc, settings);

	if (old_bitrate == enc->bitrate &&
	    old_limit_bitrate == enc->limit_bitrate)
		return true;

	OSStatus code = session_set_bitrate(enc->session, enc->rate_control,
					    enc->bitrate, enc->quality,
					    enc->limit_bitrate,
					    enc->rc_max_bitrate,
					    enc->rc_max_bitrate_window);
	if (code != noErr)
		VT_BLOG(LOG_WARNING, "failed to set bitrate to session");

	CFNumberRef n;
	VTSessionCopyProperty(enc->session,
			      kVTCompressionPropertyKey_AverageBitRate, NULL,
			      &n);

	uint32_t session_bitrate;
	CFNumberGetValue(n, kCFNumberIntType, &session_bitrate);
	CFRelease(n);

	if (session_bitrate == old_bitrate) {
		VT_BLOG(LOG_WARNING,
			"failed to update current session "
			" bitrate from %d->%d",
			old_bitrate, enc->bitrate);
	}

	dump_encoder_info(enc);
	return true;
}

static const uint8_t annexb_startcode[4] = {0, 0, 0, 1};

static void packet_put(struct darray *packet, const uint8_t *buf, size_t size)
{
	darray_push_back_array(sizeof(uint8_t), packet, buf, size);
}

static void packet_put_startcode(struct darray *packet, int size)
{
	assert(size == 3 || size == 4);

	packet_put(packet, &annexb_startcode[4 - size], size);
}

static void convert_block_nals_to_annexb(struct vt_encoder *enc,
					 struct darray *packet,
					 CMBlockBufferRef block,
					 int nal_length_bytes)
{
	size_t block_size;
	uint8_t *block_buf;

	CMBlockBufferGetDataPointer(block, 0, NULL, &block_size,
				    (char **)&block_buf);

	size_t bytes_remaining = block_size;

	while (bytes_remaining > 0) {
		uint32_t nal_size;
		if (nal_length_bytes == 1)
			nal_size = block_buf[0];
		else if (nal_length_bytes == 2)
			nal_size = CFSwapInt16BigToHost(
				((uint16_t *)block_buf)[0]);
		else if (nal_length_bytes == 4)
			nal_size = CFSwapInt32BigToHost(
				((uint32_t *)block_buf)[0]);
		else
			return;

		bytes_remaining -= nal_length_bytes;
		block_buf += nal_length_bytes;

		if (bytes_remaining < nal_size) {
			VT_BLOG(LOG_ERROR, "invalid nal block");
			return;
		}

		packet_put_startcode(packet, 3);
		packet_put(packet, block_buf, nal_size);

		bytes_remaining -= nal_size;
		block_buf += nal_size;
	}
}

static bool handle_keyframe(struct vt_encoder *enc,
			    CMFormatDescriptionRef format_desc,
			    size_t param_count, struct darray *packet,
			    struct darray *extra_data)
{
	OSStatus code;
	const uint8_t *param;
	size_t param_size;

	for (size_t i = 0; i < param_count; i++) {
		code = enc->get_parameter_set_at_index(format_desc, i, &param,
						       &param_size, NULL, NULL);
		if (code != noErr) {
			log_osstatus(LOG_ERROR, enc,
				     "getting NAL parameter "
				     "at index",
				     code);
			return false;
		}

		packet_put_startcode(packet, 4);
		packet_put(packet, param, param_size);
	}

	// if we were passed an extra_data array, fill it with
	// SPS, PPS, etc.
	if (extra_data != NULL)
		packet_put(extra_data, packet->array, packet->num);

	return true;
}

static bool convert_sample_to_annexb(struct vt_encoder *enc,
				     struct darray *packet,
				     struct darray *extra_data,
				     CMSampleBufferRef buffer, bool keyframe)
{
	OSStatus code;
	CMFormatDescriptionRef format_desc =
		CMSampleBufferGetFormatDescription(buffer);

	size_t param_count;
	int nal_length_bytes;
	code = enc->get_parameter_set_at_index(format_desc, 0, NULL, NULL,
					       &param_count, &nal_length_bytes);
	// it is not clear what errors this function can return
	// so we check the two most reasonable
	if (code == kCMFormatDescriptionBridgeError_InvalidParameter ||
	    code == kCMFormatDescriptionError_InvalidParameter) {
		VT_BLOG(LOG_WARNING, "assuming 2 parameter sets "
				     "and 4 byte NAL length header");
		param_count = 2;
		nal_length_bytes = 4;

	} else if (code != noErr) {
		log_osstatus(LOG_ERROR, enc,
			     "getting parameter count from sample", code);
		return false;
	}

	if (keyframe &&
	    !handle_keyframe(enc, format_desc, param_count, packet, extra_data))
		return false;

	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buffer);
	convert_block_nals_to_annexb(enc, packet, block, nal_length_bytes);

	return true;
}

static bool is_sample_keyframe(CMSampleBufferRef buffer)
{
	CFArrayRef attachments =
		CMSampleBufferGetSampleAttachmentsArray(buffer, false);
	if (attachments != NULL) {
		CFDictionaryRef attachment;
		CFBooleanRef has_dependencies;
		attachment =
			(CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
		has_dependencies = (CFBooleanRef)CFDictionaryGetValue(
			attachment, kCMSampleAttachmentKey_DependsOnOthers);
		return has_dependencies == kCFBooleanFalse;
	}

	return false;
}

static bool parse_sample(struct vt_encoder *enc, CMSampleBufferRef buffer,
			 struct encoder_packet *packet, CMTime off)
{
	int type;

	CMTime pts = CMSampleBufferGetPresentationTimeStamp(buffer);
	CMTime dts = CMSampleBufferGetDecodeTimeStamp(buffer);

	if (CMTIME_IS_INVALID(dts))
		dts = pts;
	// imitate x264's negative dts when bframes might have pts < dts
	else if (enc->bframes)
		dts = CMTimeSubtract(dts, off);

	pts = CMTimeMultiply(pts, enc->fps_num);
	dts = CMTimeMultiply(dts, enc->fps_num);

	bool keyframe = is_sample_keyframe(buffer);

	da_resize(enc->packet_data, 0);

	// If we are still looking for extra data
	struct darray *extra_data = NULL;
	if (enc->extra_data.num == 0)
		extra_data = &enc->extra_data.da;

	if (!convert_sample_to_annexb(enc, &enc->packet_data.da, extra_data,
				      buffer, keyframe))
		goto fail;

	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = (int64_t)(CMTimeGetSeconds(pts));
	packet->dts = (int64_t)(CMTimeGetSeconds(dts));
	packet->data = enc->packet_data.array;
	packet->size = enc->packet_data.num;
	packet->keyframe = keyframe;

	// VideoToolbox produces packets with priority lower than the RTMP code
	// expects, which causes it to be unable to recover from frame drops.
	// Fix this by manually adjusting the priority.
	if (enc->handle_priority) {
		uint8_t *start = enc->packet_data.array;
		uint8_t *end = start + enc->packet_data.num;

		start = (uint8_t *)obs_avc_find_startcode(start, end);
		while (true) {
			while (start < end && !*(start++))
				;

			if (start == end)
				break;

			type = start[0] & 0x1F;
			if (type == OBS_NAL_SLICE_IDR ||
			    type == OBS_NAL_SLICE) {
				uint8_t prev_type = (start[0] >> 5) & 0x3;
				start[0] &= ~(3 << 5);

				if (type == OBS_NAL_SLICE_IDR)
					start[0] |= OBS_NAL_PRIORITY_HIGHEST
						    << 5;
				else if (type == OBS_NAL_SLICE &&
					 prev_type !=
						 OBS_NAL_PRIORITY_DISPOSABLE)
					start[0] |= OBS_NAL_PRIORITY_HIGH << 5;
				else
					start[0] |= prev_type << 5;
			}

			start = (uint8_t *)obs_avc_find_startcode(start, end);
		}
	}

	CFRelease(buffer);
	return true;

fail:
	CFRelease(buffer);
	return false;
}

bool get_cached_pixel_buffer(struct vt_encoder *enc, CVPixelBufferRef *buf)
{
	OSStatus code;
	CVPixelBufferPoolRef pool =
		VTCompressionSessionGetPixelBufferPool(enc->session);
	if (!pool)
		return kCVReturnError;

	CVPixelBufferRef pixbuf;
	STATUS_CHECK(CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixbuf));

	// Why aren't these already set on the pixel buffer?
	// I would have expected pixel buffers from the session's
	// pool to have the correct color space stuff set

	CFStringRef matrix = obs_to_vt_colorspace(enc->colorspace);

	CVBufferSetAttachment(pixbuf, kCVImageBufferYCbCrMatrixKey, matrix,
			      kCVAttachmentMode_ShouldPropagate);
	switch (enc->colorspace) {
	case VIDEO_CS_2100_PQ:
		CVBufferSetAttachment(pixbuf, kCVImageBufferColorPrimariesKey,
					kCVImageBufferColorPrimaries_ITU_R_2020,
					kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
			pixbuf, kCVImageBufferTransferFunctionKey,
			kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ,
			kCVAttachmentMode_ShouldPropagate);
		break;
	case VIDEO_CS_2100_HLG:
		CVBufferSetAttachment(pixbuf, kCVImageBufferColorPrimariesKey,
				      kCVImageBufferColorPrimaries_ITU_R_2020,
				      kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
			pixbuf, kCVImageBufferTransferFunctionKey,
			kCVImageBufferTransferFunction_ITU_R_2100_HLG,
			kCVAttachmentMode_ShouldPropagate);
		break;
	default:
		CVBufferSetAttachment(pixbuf, kCVImageBufferColorPrimariesKey,
				      kCVImageBufferColorPrimaries_ITU_R_709_2,
				      kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
			pixbuf, kCVImageBufferTransferFunctionKey,
			kCVImageBufferTransferFunction_ITU_R_709_2,
			kCVAttachmentMode_ShouldPropagate);
		break;
	}

	*buf = pixbuf;
	return true;

fail:
	return false;
}


bool vt_encode(void *data, struct encoder_frame *frame,
	       struct encoder_packet *packet, bool *received_packet)
{
	struct vt_encoder *enc = data;

	OSStatus code;

	CMTime dur = CMTimeMake(enc->fps_den, enc->fps_num);
	CMTime off = CMTimeMultiply(dur, 2);
	CMTime pts = CMTimeMake(
		frame->pts, enc->fps_num); // CMTimeMultiply(dur, frame->pts);

	CVPixelBufferRef pixbuf = NULL;

	if (!get_cached_pixel_buffer(enc, &pixbuf)) {
		VT_BLOG(LOG_ERROR, "Unable to create pixel buffer");
		goto fail;
	}

	STATUS_CHECK(CVPixelBufferLockBaseAddress(pixbuf, 0));

	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (frame->data[i] == NULL)
			break;
		uint8_t *p = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(
			pixbuf, i);
		uint8_t *f = frame->data[i];
		size_t plane_linesize =
			CVPixelBufferGetBytesPerRowOfPlane(pixbuf, i);
		size_t plane_height = CVPixelBufferGetHeightOfPlane(pixbuf, i);

		for (size_t j = 0; j < plane_height; j++) {
			memcpy(p, f, frame->linesize[i]);
			p += plane_linesize;
			f += frame->linesize[i];
		}
	}

	STATUS_CHECK(CVPixelBufferUnlockBaseAddress(pixbuf, 0));

	STATUS_CHECK(VTCompressionSessionEncodeFrame(enc->session, pixbuf, pts,
						     dur, NULL, pixbuf, NULL));

	CMSampleBufferRef buffer =
		(CMSampleBufferRef)CMSimpleQueueDequeue(enc->queue);

	// No samples waiting in the queue
	if (buffer == NULL)
		return true;

	*received_packet = true;
	return parse_sample(enc, buffer, packet, off);

fail:
	return false;
}

bool vt_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct vt_encoder *enc = (struct vt_encoder *)data;
	*extra_data = enc->extra_data.array;
	*size = enc->extra_data.num;
	return true;
}
