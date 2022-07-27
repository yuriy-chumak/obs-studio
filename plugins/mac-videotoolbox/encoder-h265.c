#define VT_LOG_ENCODER_NAME "hevc"
#include "encoder.h"

#include <obs-hevc.h>

// Clipped from NSApplication as it is in a ObjC header
extern const double NSAppKitVersionNumber;
#define NSAppKitVersionNumber10_8 1187

static DARRAY(struct vt_encoder_type_data) vt_encoder_type_datas;

// Get around missing symbol on 10.8 during compilation
enum {
	kCMFormatDescriptionBridgeError_InvalidParameter_ = -12712,
};

static bool is_appkit10_9_or_greater()
{
	return floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_8;
}

static void *vt_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct vt_encoder *enc = bzalloc(sizeof(struct vt_encoder));

	OSStatus code;

	enc->encoder = encoder;
	enc->vt_encoder_id = obs_encoder_get_id(encoder);

	if (!update_params(enc, settings))
		goto fail;

	STATUS_CHECK(CMSimpleQueueCreate(NULL, 100, &enc->queue));

	if (!create_encoder(enc, kCMVideoCodecType_HEVC, true))
		goto fail;

	dump_encoder_info(enc);

	enc->handle_priority = false;
	enc->get_parameter_set_at_index =
		CMVideoFormatDescriptionGetHEVCParameterSetAtIndex;

	return enc;

fail:
	vt_destroy(enc);
	return NULL;
}

#undef STATUS_CHECK
#undef CFNUM_INT

static const char *vt_getname(void *data)
{
	const char *disp_name =
		vt_encoder_type_datas.array[(long)data].disp_name;

	if (strcmp("Apple HEVC (HW)", disp_name) == 0) {
		return obs_module_text("VTH265EncHW");
	}
	return disp_name;
}

static bool limit_bitrate_modified(obs_properties_t *ppts, obs_property_t *p,
				   obs_data_t *settings)
{
	bool use_max_bitrate = obs_data_get_bool(settings, "limit_bitrate");
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, use_max_bitrate);
	p = obs_properties_get(ppts, "max_bitrate_window");
	obs_property_set_visible(p, use_max_bitrate);
	return true;
}

static obs_properties_t *vt_properties(void *unused, void *data)
{
	UNUSED_PARAMETER(unused);
	struct vt_encoder_type_data *type_data = data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_int(props, "bitrate", TEXT_BITRATE, 50, 10000000,
				   50);
	obs_property_int_set_suffix(p, " Kbps");

	p = obs_properties_add_bool(props, "limit_bitrate",
				    TEXT_USE_MAX_BITRATE);
	obs_property_set_modified_callback(p, limit_bitrate_modified);

	p = obs_properties_add_int(props, "max_bitrate", TEXT_MAX_BITRATE, 50,
				   10000000, 50);
	obs_property_int_set_suffix(p, " Kbps");

	obs_properties_add_float(props, "max_bitrate_window",
				 TEXT_MAX_BITRATE_WINDOW, 0.10f, 10.0f, 0.25f);

	obs_properties_add_int(props, "keyint_sec", TEXT_KEYINT_SEC, 0, 20, 1);

	p = obs_properties_add_list(props, "profile", TEXT_PROFILE,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, TEXT_NONE, "");
	obs_property_list_add_string(p, "main", "main");
	obs_property_list_add_string(p, "main10", "main10");
	obs_properties_add_bool(props, "bframes", TEXT_BFRAMES);

	return props;
}

static void vt_defaults(obs_data_t *settings, void *data)
{
	struct vt_encoder_type_data *type_data = data;

	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_bool(settings, "limit_bitrate", false);
	obs_data_set_default_int(settings, "max_bitrate", 2500);
	obs_data_set_default_double(settings, "max_bitrate_window", 1.5f);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_string(settings, "profile", "main");
	obs_data_set_default_bool(settings, "bframes", true);
}

void encoder_list_create()
{
	CFArrayRef encoder_list;
	VTCopyVideoEncoderList(NULL, &encoder_list);
	CFIndex size = CFArrayGetCount(encoder_list);

	for (CFIndex i = 0; i < size; i++) {
		CFDictionaryRef encoder_dict =
			CFArrayGetValueAtIndex(encoder_list, i);

		// skip software encoders
		CFBooleanRef hardware_ref = CFDictionaryGetValue(
			encoder_dict,
			kVTVideoEncoderList_IsHardwareAccelerated);
		bool hardware_accelerated = false;
		if (hardware_ref)
			hardware_accelerated = CFBooleanGetValue(hardware_ref);
		if (!hardware_accelerated)
			continue;

		// continue filling encoder info
#define VT_DICTSTR(key, name)                                             \
	CFStringRef name##_ref = CFDictionaryGetValue(encoder_dict, key); \
	CFIndex name##_len = CFStringGetLength(name##_ref);               \
	char *name = bzalloc(name##_len + 1);                             \
	CFStringGetFileSystemRepresentation(name##_ref, name, name##_len);

		VT_DICTSTR(kVTVideoEncoderList_CodecName, codec_name);
		if (strcmp("HEVC", codec_name) != 0) {
			bfree(codec_name);
			continue;
		}

		VT_DICTSTR(kVTVideoEncoderList_EncoderName, name);
		VT_DICTSTR(kVTVideoEncoderList_EncoderID, id);
		VT_DICTSTR(kVTVideoEncoderList_DisplayName, disp_name);
		
#undef VT_DICTSTR

		struct vt_encoder_type_data enc = {
			.name = name,
			.id = id,
			.disp_name = disp_name,
			.codec_name = codec_name,
		};
		da_push_back(vt_encoder_type_datas, &enc);
	}

	CFRelease(encoder_list);
}

void encoder_list_destroy()
{
	for (size_t i = 0; i < vt_encoder_type_datas.num; i++) {
		bfree((char *)vt_encoder_type_datas.array[i].name);
		bfree((char *)vt_encoder_type_datas.array[i].id);
		bfree((char *)vt_encoder_type_datas.array[i].codec_name);
		bfree((char *)vt_encoder_type_datas.array[i].disp_name);
	}
	da_free(vt_encoder_type_datas);
}

void register_encoders()
{
	struct obs_encoder_info info = {.type = OBS_ENCODER_VIDEO,
					.codec = "hevc",
					.get_name = vt_getname,
					.create = vt_create,
					.destroy = vt_destroy,
					.encode = vt_encode,
					.update = vt_update,
					.get_properties2 = vt_properties,
					.get_defaults2 = vt_defaults,
					.get_extra_data = vt_extra_data,
					.caps = OBS_ENCODER_CAP_DYN_BITRATE};

	for (size_t i = 0; i < vt_encoder_type_datas.num; i++) {
		info.id = vt_encoder_type_datas.array[i].id;
		info.type_data = (void *)i;
		obs_register_encoder(&info);
	}
}

bool obs_module_load_h265(void)
{
	if (!is_appkit10_9_or_greater()) {
		VT_LOG(LOG_WARNING, "Not adding VideoToolbox HEVC encoder; "
				    "AppKit must be version 10.9 or greater");
		return false;
	}

	VT_LOG(LOG_INFO, "Adding VideoToolbox HEVC encoders");

	encoder_list_create();
	register_encoders();

	return true;
}

void obs_module_unload_h265(void)
{
	encoder_list_destroy();
}
