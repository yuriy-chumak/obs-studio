#define VT_LOG_ENCODER_NAME "h264"
#include "encoder.h"

#include <obs-avc.h>

static void *vt_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct vt_encoder *enc = bzalloc(sizeof(struct vt_encoder));

	OSStatus code;

	enc->encoder = encoder;
	enc->vt_encoder_id = obs_encoder_get_id(encoder);

	if (!update_params(enc, settings))
		goto fail;

	STATUS_CHECK(CMSimpleQueueCreate(NULL, 100, &enc->queue));

	if (!create_encoder(enc, kCMVideoCodecType_H264, false))
		goto fail;

	dump_encoder_info(enc);

	enc->handle_priority = true;
	enc->get_parameter_set_at_index =
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
	return enc;

fail:
	vt_destroy(enc);
	return NULL;
}

#undef STATUS_CHECK
#undef CFNUM_INT

static const char *vt_getname(void *data)
{
	struct vt_encoder_type_data *type_data = data;

	if (strcmp("Apple H.264 (HW)", type_data->disp_name) == 0) {
		return obs_module_text("VTH264EncHW");
	} else if (strcmp("Apple H.264 (SW)", type_data->disp_name) == 0) {
		return obs_module_text("VTH264EncSW");
	}
	return type_data->disp_name;
}

static bool rate_control_limit_bitrate_modified(obs_properties_t *ppts,
						obs_property_t *p,
						obs_data_t *settings)
{
	bool has_bitrate = true;
	bool can_limit_bitrate = true;
	bool use_limit_bitrate = obs_data_get_bool(settings, "limit_bitrate");
	const char *rate_control =
		obs_data_get_string(settings, "rate_control");

	if (strcmp(rate_control, "CBR") == 0) {
		can_limit_bitrate = false;
		has_bitrate = true;
	} else if (strcmp(rate_control, "CRF") == 0) {
		can_limit_bitrate = true;
		has_bitrate = false;
	} else if (strcmp(rate_control, "ABR") == 0) {
		can_limit_bitrate = true;
		has_bitrate = true;
	}

	p = obs_properties_get(ppts, "limit_bitrate");
	obs_property_set_visible(p, can_limit_bitrate);
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, can_limit_bitrate && use_limit_bitrate);
	p = obs_properties_get(ppts, "max_bitrate_window");
	obs_property_set_visible(p, can_limit_bitrate && use_limit_bitrate);

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, has_bitrate);
	p = obs_properties_get(ppts, "quality");
	obs_property_set_visible(p, !has_bitrate);
	return true;
}

static obs_properties_t *vt_properties(void *unused, void *data)
{
	UNUSED_PARAMETER(unused);
	struct vt_encoder_type_data *type_data = data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control", TEXT_RATE_CONTROL,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

	if (__builtin_available(macOS 13.0, *))
		if (type_data->hardware_accelerated
#ifndef __aarch64__
		    && (os_get_emulation_status() == true)
#endif
		)
			obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "ABR", "ABR");
	if (type_data->hardware_accelerated
#ifndef __aarch64__
	    && (os_get_emulation_status() == true)
#endif
	)
		obs_property_list_add_string(p, "CRF", "CRF");
	obs_property_set_modified_callback(p,
					   rate_control_limit_bitrate_modified);

	p = obs_properties_add_int(props, "bitrate", TEXT_BITRATE, 50, 10000000,
				   50);
	obs_property_int_set_suffix(p, " Kbps");
	obs_properties_add_int_slider(props, "quality", TEXT_QUALITY, 0, 100,
				      1);

	p = obs_properties_add_bool(props, "limit_bitrate",
				    TEXT_USE_MAX_BITRATE);
	obs_property_set_modified_callback(p,
					   rate_control_limit_bitrate_modified);

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
	obs_property_list_add_string(p, "baseline", "baseline");
	obs_property_list_add_string(p, "main", "main");
	obs_property_list_add_string(p, "high", "high");

	obs_properties_add_bool(props, "bframes", TEXT_BFRAMES);

	return props;
}

static void vt_defaults(obs_data_t *settings, void *data)
{
	struct vt_encoder_type_data *type_data = data;

	obs_data_set_default_string(settings, "rate_control", "ABR");
	if (__builtin_available(macOS 13.0, *))
		if (type_data->hardware_accelerated
#ifndef __aarch64__
		    && (os_get_emulation_status() == true)
#endif
		)
			obs_data_set_default_string(settings, "rate_control",
						    "CBR");
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "quality", 60);
	obs_data_set_default_bool(settings, "limit_bitrate", false);
	obs_data_set_default_int(settings, "max_bitrate", 2500);
	obs_data_set_default_double(settings, "max_bitrate_window", 1.5f);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_string(settings, "profile", "");
	obs_data_set_default_bool(settings, "bframes", true);
}

static void vt_free_type_data(void *data)
{
	struct vt_encoder_type_data *type_data = data;

	bfree((char *)type_data->disp_name);
	bfree((char *)type_data->id);
	bfree(type_data);
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mac-videotoolbox", "en-US")

bool obs_module_load_h264(void)
{
	struct obs_encoder_info info = {
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = vt_getname,
		.create = vt_create,
		.destroy = vt_destroy,
		.encode = vt_encode,
		.update = vt_update,
		.get_properties2 = vt_properties,
		.get_defaults2 = vt_defaults,
		.get_extra_data = vt_extra_data,
		.free_type_data = vt_free_type_data,
		.caps = OBS_ENCODER_CAP_DYN_BITRATE,
	};

	CFArrayRef encoder_list;
	VTCopyVideoEncoderList(NULL, &encoder_list);
	CFIndex size = CFArrayGetCount(encoder_list);

	for (CFIndex i = 0; i < size; i++) {
		CFDictionaryRef encoder_dict =
			CFArrayGetValueAtIndex(encoder_list, i);

#define VT_DICTSTR(key, name)                                             \
	CFStringRef name##_ref = CFDictionaryGetValue(encoder_dict, key); \
	CFIndex name##_len = CFStringGetLength(name##_ref);               \
	char *name = bzalloc(name##_len + 1);                             \
	CFStringGetFileSystemRepresentation(name##_ref, name, name##_len);

		VT_DICTSTR(kVTVideoEncoderList_CodecName, codec_name);
		if (strcmp("H.264", codec_name) != 0) {
			bfree(codec_name);
			continue;
		}
		bfree(codec_name);
		VT_DICTSTR(kVTVideoEncoderList_EncoderID, id);
		VT_DICTSTR(kVTVideoEncoderList_DisplayName, disp_name);

		CFBooleanRef hardware_ref = CFDictionaryGetValue(
			encoder_dict,
			kVTVideoEncoderList_IsHardwareAccelerated);
		bool hardware_accelerated = false;
		if (hardware_ref)
			hardware_accelerated = CFBooleanGetValue(hardware_ref);

		info.id = id;
		struct vt_encoder_type_data *type_data =
			bzalloc(sizeof(struct vt_encoder_type_data));
		type_data->disp_name = disp_name;
		type_data->id = id;
		type_data->hardware_accelerated = hardware_accelerated;
		info.type_data = type_data;

		obs_register_encoder(&info);
#undef VT_DICTSTR
	}

	CFRelease(encoder_list);

	VT_LOG(LOG_INFO, "Adding VideoToolbox encoders");

	return true;
}
