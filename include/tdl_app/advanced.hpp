#pragma once

#include "tdl_app/tdl_app.hpp"

#include "tdl_app/audio_decoder.hpp"
#include "tdl_app/audio_encoder.hpp"
#include "tdl_app/audio_input.hpp"
#include "tdl_app/audio_output.hpp"
#include "tdl_app/audio_system.hpp"
#include "tdl_app/audio_types.hpp"
#include "tdl_app/frame_reader.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/graphic_vo_layer.hpp"
#include "tdl_app/media_ipc_client.hpp"
#include "tdl_app/media_ipc_server.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/media_system.hpp"
#include "tdl_app/mipi_device.hpp"
#include "tdl_app/mmf.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/region_overlay.hpp"
#include "tdl_app/sensor_media.hpp"
#include "tdl_app/sys_context.hpp"
#include "tdl_app/vdec_channel.hpp"
#include "tdl_app/venc_channel.hpp"
#include "tdl_app/video_buffer.hpp"
#include "tdl_app/vi_channel.hpp"
#include "tdl_app/vo_output.hpp"
#include "tdl_app/voice_activity_detector.hpp"
#include "tdl_app/vpss_group.hpp"

// Advanced API is an umbrella header for demos and higher-level applications.
// It intentionally aggregates both algorithm and media wrapper entry points.
