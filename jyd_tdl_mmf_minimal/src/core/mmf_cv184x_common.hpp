#ifndef MMF_CV184X_COMMON_HPP
#define MMF_CV184X_COMMON_HPP

#include "mmf_cvi_audio.hpp"
#include "mmf_cvi_camera.hpp"
#include "mmf_cvi_codec.hpp"

namespace mmf_cv184x {
constexpr uint32_t kJpegOneshotVencChannel = 0;
constexpr uint32_t kJpegHttpVencChannel = 1;
extern std::mutex g_error_mutex;
extern std::string g_last_error;
void set_last_error(const std::string& error);
mmf_result_t ok_or_error(bool ok, const std::string& error);
mmf_cvi::CameraSourceId to_native_source(mmf_camera_source_t source);
bool source_to_vpss(mmf_camera_source_t source, mmf_camera_device_t device,
                    int* group, int* channel);
mmf_scale_mode_t from_vpss_scale(const VPSS_CHN_ATTR_S& attr);
void apply_vpss_scale(mmf_scale_mode_t mode, VPSS_CHN_ATTR_S* attr);
mmf_pixel_format_t from_native_pixfmt(int format);
int to_native_pixfmt(mmf_pixel_format_t format);
mmf_cvi::AudioBitWidth to_native_bit_width(mmf_audio_format_t format);
mmf_audio_format_t from_native_bit_width(mmf_cvi::AudioBitWidth bit_width);
size_t bytes_per_sample(mmf_audio_format_t format);
mmf_cvi::AudioSampleRate to_native_sample_rate(uint32_t rate);
mmf_cvi::AudioPayloadType to_native_audio_payload(mmf_codec_t codec);
mmf_codec_t from_native_audio_payload(mmf_cvi::AudioPayloadType codec);
mmf_cvi::AudioInput::Config to_input_config(const mmf_audio_input_config_t& cfg);
mmf_cvi::AudioOutput::Config to_output_config(const mmf_audio_output_config_t& cfg);
mmf_cvi::AudioTalkVqeConfig to_talk_vqe(const mmf_audio_3a_config_t& cfg, uint32_t sample_rate);
void fill_default_3a(mmf_audio_3a_config_t* config);
void audio_3a_note_input_open(const mmf_audio_3a_config_t& config, bool enabled, bool configured);
void audio_3a_note_input_config(const mmf_audio_3a_config_t& config, bool enabled, bool configured);
void audio_3a_note_input_close(bool enabled, bool configured);
void audio_3a_note_output_open(bool provide_reference);
void audio_3a_note_output_close(bool provide_reference);
mmf_result_t copy_native_video_frame(const mmf_video_frame_t& src, mmf_video_frame_t* dst,
                                     std::vector<uint8_t>* storage);
}  // namespace mmf_cv184x

#endif
