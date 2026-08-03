#pragma once

#include <mutex>

#include "algorithm/private/bmrt_utils.hpp"

namespace tdl_app {
namespace bmrt_runtime {

// Owns one VPSS group and its model input buffer. It deliberately leaves the
// process-wide CVI_SYS/VB lifecycle to the media layer.
class VpssPreprocessor {
 public:
  struct Config {
    int width = 0;
    int height = 0;
    bool rgb = true;
    // NHWC uint8 models consume packed RGB/BGR bytes instead of planar CHW.
    bool interleaved = false;
    bool keep_aspect_ratio = false;
    std::array<uint8_t, 3> padding{{0, 0, 0}};
    bm_data_type_t input_dtype = BM_INT8;
    float input_scale = 1.0f;
    int input_zero_point = 0;
    std::array<float, 3> mean{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> scale{{1.0f, 1.0f, 1.0f}};
  };

  // Coordinates are relative to the source VIDEO_FRAME_INFO_S.  Detector
  // metadata stays on the CPU while VPSS performs the actual crop.
  struct Roi {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  VpssPreprocessor() = default;
  ~VpssPreprocessor() { close(); }

  VpssPreprocessor(const VpssPreprocessor &) = delete;
  VpssPreprocessor &operator=(const VpssPreprocessor &) =
      delete;

  bool open(bm_handle_t handle, const Config &config, std::string *error) {
    close();
    if (!handle || config.width <= 0 || config.height <= 0) {
      setError(error, "invalid VPSS preprocessor configuration");
      return false;
    }
    if (config.input_dtype != BM_INT8 && config.input_dtype != BM_UINT8) {
      setError(error, "VPSS preprocessor requires int8 or uint8 input");
      return false;
    }

    handle_ = handle;
    config_ = config;
    const std::size_t tensor_bytes =
        static_cast<std::size_t>(config.width) * config.height * kInputChannels;
    if (bm_malloc_device_byte(handle_, &tensor_memory_, tensor_bytes) !=
        BM_SUCCESS) {
      setError(error, "bm_malloc_device_byte failed for VPSS input tensor");
      close();
      return false;
    }
    tensor_allocated_ = true;
    unsigned long long mapped = 0;
    if (bm_mem_mmap_device_mem(handle_, &tensor_memory_, &mapped) != BM_SUCCESS) {
      setError(error, "bm_mem_mmap_device_mem failed for VPSS input tensor");
      close();
      return false;
    }
    tensor_virtual_ = reinterpret_cast<uint8_t *>(mapped);
    if (!prepareOutputBuffer(error)) {
      close();
      return false;
    }
    opened_ = true;
    return true;
  }

  bool preprocess(void *native_frame, std::string *error) {
    return preprocess(native_frame, nullptr, error);
  }

  bool preprocess(void *native_frame, const Roi *roi, std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!opened_) {
      setError(error, "VPSS preprocessor is not initialized");
      return false;
    }
    if (!native_frame) {
      setError(error, "native VIDEO_FRAME_INFO_S is null");
      return false;
    }

    VIDEO_FRAME_INFO_S *source = static_cast<VIDEO_FRAME_INFO_S *>(native_frame);
    const VIDEO_FRAME_S &src = source->stVFrame;
    if (src.u32Width == 0 || src.u32Height == 0 || src.u64PhyAddr[0] == 0) {
      setError(error, "native VIDEO_FRAME_INFO_S is incomplete");
      return false;
    }
    if (roi && (roi->x < 0 || roi->y < 0 || roi->width <= 0 ||
                roi->height <= 0 ||
                roi->x + roi->width > static_cast<int>(src.u32Width) ||
                roi->y + roi->height > static_cast<int>(src.u32Height))) {
      setError(error, "VPSS ROI is outside the source frame");
      return false;
    }
    if (!group_created_) {
      if (!createGroup(*source, error)) {
        return false;
      }
    } else if (source_width_ != static_cast<int>(src.u32Width) ||
               source_height_ != static_cast<int>(src.u32Height) ||
               source_format_ != static_cast<int>(src.enPixelFormat)) {
      setError(error,
               "camera frame format changed after VPSS model initialization");
      return false;
    }

    VPSS_CROP_INFO_S crop{};
    if (roi) {
      crop.bEnable = CVI_TRUE;
      crop.stCropRect.s32X = roi->x;
      crop.stCropRect.s32Y = roi->y;
      crop.stCropRect.u32Width = static_cast<CVI_U32>(roi->width);
      crop.stCropRect.u32Height = static_cast<CVI_U32>(roi->height);
    }
    const int crop_ret = CVI_VPSS_SetChnCrop(group_id_, 0, &crop);
    if (crop_ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_SetChnCrop failed, ret=" +
                          std::to_string(crop_ret));
      return false;
    }

    VIDEO_FRAME_INFO_S output{};
    VIDEO_FRAME_S &dst = output.stVFrame;
    dst.enCompressMode = COMPRESS_MODE_NONE;
    dst.enPixelFormat = outputFormat();
    dst.enVideoFormat = VIDEO_FORMAT_LINEAR;
    dst.enColorGamut = COLOR_GAMUT_BT709;
    dst.enDynamicRange = DYNAMIC_RANGE_SDR8;
    dst.u32Width = static_cast<CVI_U32>(config_.width);
    dst.u32Height = static_cast<CVI_U32>(config_.height);
    dst.u32Stride[0] = output_layout_.u32MainStride;
    dst.u32Length[0] = output_layout_.u32MainYSize;
    if (output_layout_.plane_num > 1) {
      dst.u32Stride[1] = output_layout_.u32CStride;
      dst.u32Length[1] = output_layout_.u32MainCSize;
    }
    if (output_layout_.plane_num > 2) {
      dst.u32Stride[2] = output_layout_.u32CStride;
      dst.u32Length[2] = output_layout_.u32MainCSize;
    }
    const bm_device_mem_t &output_memory =
        direct_tensor_ ? tensor_memory_ : staging_memory_;
    uint8_t *output_virtual =
        direct_tensor_ ? tensor_virtual_ : staging_virtual_;
    dst.u64PhyAddr[0] = bm_mem_get_device_addr(output_memory);
    dst.pu8VirAddr[0] = output_virtual;
    for (int plane = 1; plane < output_layout_.plane_num; ++plane) {
      dst.u64PhyAddr[plane] =
          dst.u64PhyAddr[plane - 1] + dst.u32Length[plane - 1];
      dst.pu8VirAddr[plane] =
          dst.pu8VirAddr[plane - 1] + dst.u32Length[plane - 1];
    }

    int ret = CVI_VPSS_SendChnFrame(group_id_, 0, &output, -1);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_SendChnFrame failed, ret=" +
                          std::to_string(ret));
      return false;
    }
    // The source belongs to the MMF reader.  Some CVI VPSS paths update the
    // supplied frame structure, so pass a copy and preserve the exact frame
    // metadata required by CVI_VPSS_ReleaseChnFrame in the caller.
    VIDEO_FRAME_INFO_S source_copy = *source;
    ret = CVI_VPSS_SendFrame(group_id_, &source_copy, -1);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_SendFrame failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_VPSS_GetChnFrame(group_id_, 0, &output, -1);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_GetChnFrame failed, ret=" + std::to_string(ret));
      return false;
    }
    // Every acquired VPSS output must be released before the caller releases
    // its source frame or the algorithm destroys this group.
    struct ChannelFrameLease {
      int group_id = -1;
      VIDEO_FRAME_INFO_S *frame = nullptr;
      ~ChannelFrameLease() {
        if (frame) {
          CVI_VPSS_ReleaseChnFrame(group_id, 0, frame);
        }
      }
    } output_lease{group_id_, &output};

    if (!direct_tensor_) {
      if (bm_mem_invalidate_device_mem(handle_, &staging_memory_) != BM_SUCCESS) {
        setError(error, "failed to invalidate VPSS staging memory");
        return false;
      }
      const std::size_t plane_bytes = static_cast<std::size_t>(config_.width) *
                                      config_.height;
      std::size_t source_offset = 0;
      for (int plane = 0; plane < output_layout_.plane_num; ++plane) {
        const uint8_t *src_plane = staging_virtual_ + source_offset;
        const std::size_t row_bytes =
            config_.interleaved ? static_cast<std::size_t>(config_.width) * 3
                                : static_cast<std::size_t>(config_.width);
        uint8_t *dst_plane = config_.interleaved
                                 ? tensor_virtual_
                                 : tensor_virtual_ + plane * plane_bytes;
        for (int row = 0; row < config_.height; ++row) {
          std::memcpy(dst_plane + static_cast<std::size_t>(row) * row_bytes,
                      src_plane + static_cast<std::size_t>(row) *
                                      output.stVFrame.u32Stride[plane],
                      row_bytes);
        }
        source_offset += output.stVFrame.u32Length[plane];
      }
      if (bm_mem_flush_device_mem(handle_, &tensor_memory_) != BM_SUCCESS) {
        setError(error, "failed to flush VPSS input tensor memory");
        return false;
      }
    }
    return true;
  }

  void close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (channel_enabled_) {
      CVI_VPSS_DisableChn(group_id_, 0);
      channel_enabled_ = false;
    }
    if (group_started_) {
      CVI_VPSS_StopGrp(group_id_);
      group_started_ = false;
    }
    if (group_created_) {
      CVI_VPSS_DestroyGrp(group_id_);
      group_created_ = false;
    }
    group_id_ = -1;
    source_width_ = 0;
    source_height_ = 0;
    source_format_ = -1;

    if (handle_ && staging_virtual_) {
      bm_mem_unmap_device_mem(handle_, staging_virtual_, staging_memory_.size);
      staging_virtual_ = nullptr;
    }
    if (handle_ && staging_allocated_) {
      bm_free_device(handle_, staging_memory_);
      staging_memory_ = bm_device_mem_t{};
      staging_allocated_ = false;
    }
    if (handle_ && tensor_virtual_) {
      bm_mem_unmap_device_mem(handle_, tensor_virtual_, tensor_memory_.size);
      tensor_virtual_ = nullptr;
    }
    if (handle_ && tensor_allocated_) {
      bm_free_device(handle_, tensor_memory_);
      tensor_memory_ = bm_device_mem_t{};
      tensor_allocated_ = false;
    }
    handle_ = nullptr;
    config_ = Config{};
    output_layout_ = VB_CAL_CONFIG_S{};
    direct_tensor_ = false;
    opened_ = false;
  }

  bool isOpen() const { return opened_; }
  bm_device_mem_t inputMemory() const { return tensor_memory_; }
  int groupId() const { return group_id_; }
  std::size_t deviceBytes() const {
    return tensor_memory_.size + staging_memory_.size;
  }

 private:
  PIXEL_FORMAT_E outputFormat() const {
    if (config_.interleaved) {
      return config_.rgb ? PIXEL_FORMAT_RGB_888 : PIXEL_FORMAT_BGR_888;
    }
    if (config_.input_dtype == BM_UINT8) {
      return PIXEL_FORMAT_UINT8_C3_PLANAR;
    }
    return config_.rgb ? PIXEL_FORMAT_RGB_888_PLANAR
                       : PIXEL_FORMAT_BGR_888_PLANAR;
  }

  bool prepareOutputBuffer(std::string *error) {
    COMMON_GetPicBufferConfig(
        static_cast<CVI_U32>(config_.width),
        static_cast<CVI_U32>(config_.height), outputFormat(), DATA_BITWIDTH_8,
        COMPRESS_MODE_NONE, DEFAULT_ALIGN, &output_layout_);
    if (output_layout_.plane_num == 0 || output_layout_.plane_num > 3 ||
        output_layout_.u32MainStride == 0 || output_layout_.u32MainYSize == 0 ||
        (output_layout_.plane_num > 1 && output_layout_.u32MainCSize == 0)) {
      setError(error, "unsupported VPSS output layout");
      return false;
    }

    const std::size_t tensor_bytes =
        static_cast<std::size_t>(config_.width) * config_.height * kInputChannels;
    const std::size_t output_bytes =
        static_cast<std::size_t>(output_layout_.u32MainYSize) +
        (output_layout_.plane_num > 1
             ? static_cast<std::size_t>(output_layout_.u32MainCSize) *
                   (output_layout_.plane_num - 1)
             : 0);
    const std::size_t expected_stride =
        config_.interleaved ? static_cast<std::size_t>(config_.width) * 3
                            : static_cast<std::size_t>(config_.width);
    direct_tensor_ =
        output_layout_.u32MainStride == expected_stride &&
        (output_layout_.plane_num == 1 ||
         output_layout_.u32CStride == static_cast<CVI_U32>(config_.width)) &&
        output_bytes == tensor_bytes;
    if (direct_tensor_) {
      return true;
    }

    if (bm_malloc_device_byte(handle_, &staging_memory_, output_bytes) !=
        BM_SUCCESS) {
      setError(error, "bm_malloc_device_byte failed for VPSS staging frame");
      return false;
    }
    staging_allocated_ = true;
    unsigned long long mapped = 0;
    if (bm_mem_mmap_device_mem(handle_, &staging_memory_, &mapped) != BM_SUCCESS) {
      setError(error, "bm_mem_mmap_device_mem failed for VPSS staging frame");
      return false;
    }
    staging_virtual_ = reinterpret_cast<uint8_t *>(mapped);
    return true;
  }

  bool createGroup(const VIDEO_FRAME_INFO_S &source, std::string *error) {
    // CVI_VPSS_GetAvailableGrp and CVI_VPSS_CreateGrp must be one operation
    // when separate algorithm instances initialize concurrently.
    static std::mutex group_create_mutex;
    std::lock_guard<std::mutex> group_lock(group_create_mutex);
    group_id_ = CVI_VPSS_GetAvailableGrp();
    if (group_id_ < 0) {
      setError(error, "no free VPSS group is available");
      return false;
    }

    VPSS_GRP_ATTR_S group_attr{};
    group_attr.stFrameRate.s32SrcFrameRate = -1;
    group_attr.stFrameRate.s32DstFrameRate = -1;
    group_attr.enPixelFormat = source.stVFrame.enPixelFormat;
    group_attr.u32MaxW = source.stVFrame.u32Width;
    group_attr.u32MaxH = source.stVFrame.u32Height;
    group_attr.u8VpssDev = 0;
    int ret = CVI_VPSS_CreateGrp(group_id_, &group_attr);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_CreateGrp failed, ret=" + std::to_string(ret));
      group_id_ = -1;
      return false;
    }
    group_created_ = true;
    ret = CVI_VPSS_ResetGrp(group_id_);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_ResetGrp failed, ret=" + std::to_string(ret));
      close();
      return false;
    }

    VPSS_CHN_ATTR_S channel_attr{};
    channel_attr.u32Width = static_cast<CVI_U32>(config_.width);
    channel_attr.u32Height = static_cast<CVI_U32>(config_.height);
    channel_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    channel_attr.enPixelFormat = outputFormat();
    channel_attr.stFrameRate.s32SrcFrameRate = -1;
    channel_attr.stFrameRate.s32DstFrameRate = -1;
    channel_attr.u32Depth = 1;
    channel_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    channel_attr.stAspectRatio.bEnableBgColor = CVI_FALSE;
    if (config_.keep_aspect_ratio) {
      const float ratio = std::min(
          static_cast<float>(config_.width) / source.stVFrame.u32Width,
          static_cast<float>(config_.height) / source.stVFrame.u32Height);
      const int resized_width = std::max(
          1, std::min(config_.width, static_cast<int>(
                                     std::round(source.stVFrame.u32Width * ratio))));
      const int resized_height = std::max(
          1, std::min(config_.height, static_cast<int>(
                                      std::round(source.stVFrame.u32Height * ratio))));
      channel_attr.stAspectRatio.enMode = ASPECT_RATIO_MANUAL;
      channel_attr.stAspectRatio.bEnableBgColor = CVI_TRUE;
      channel_attr.stAspectRatio.stVideoRect.s32X =
          (config_.width - resized_width) / 2;
      channel_attr.stAspectRatio.stVideoRect.s32Y =
          (config_.height - resized_height) / 2;
      channel_attr.stAspectRatio.stVideoRect.u32Width =
          static_cast<CVI_U32>(resized_width);
      channel_attr.stAspectRatio.stVideoRect.u32Height =
          static_cast<CVI_U32>(resized_height);
      channel_attr.stAspectRatio.u32BgColor =
          RGB_8BIT(config_.padding[0], config_.padding[1], config_.padding[2]);
    }
    channel_attr.stNormalize.bEnable = CVI_TRUE;
    const bool has_quantization_scale = config_.input_scale != 0.0f;
    const float qfactor = !has_quantization_scale
                              ? 1.0f
                              : (std::fabs(config_.input_scale) > 1.0f
                                     ? config_.input_scale
                                     : 1.0f / config_.input_scale);
    const float qzero = has_quantization_scale
                            ? static_cast<float>(config_.input_zero_point)
                            : 0.0f;
    for (int i = 0; i < 3; ++i) {
      channel_attr.stNormalize.factor[i] = config_.scale[i] * qfactor;
      channel_attr.stNormalize.mean[i] =
          -config_.mean[i] * config_.scale[i] * qfactor + qzero;
    }
    channel_attr.stNormalize.rounding = VPSS_ROUNDING_TO_EVEN;

    ret = CVI_VPSS_SetChnAttr(group_id_, 0, &channel_attr);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_SetChnAttr failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    ret = CVI_VPSS_EnableChn(group_id_, 0);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_EnableChn failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    channel_enabled_ = true;
    ret = CVI_VPSS_StartGrp(group_id_);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VPSS_StartGrp failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    group_started_ = true;
    source_width_ = static_cast<int>(source.stVFrame.u32Width);
    source_height_ = static_cast<int>(source.stVFrame.u32Height);
    source_format_ = static_cast<int>(source.stVFrame.enPixelFormat);
    return true;
  }

  bm_handle_t handle_ = nullptr;
  Config config_;
  bm_device_mem_t tensor_memory_{};
  bm_device_mem_t staging_memory_{};
  uint8_t *tensor_virtual_ = nullptr;
  uint8_t *staging_virtual_ = nullptr;
  VB_CAL_CONFIG_S output_layout_{};
  int group_id_ = -1;
  int source_width_ = 0;
  int source_height_ = 0;
  int source_format_ = -1;
  bool opened_ = false;
  bool group_created_ = false;
  bool channel_enabled_ = false;
  bool group_started_ = false;
  bool direct_tensor_ = false;
  bool tensor_allocated_ = false;
  bool staging_allocated_ = false;
  std::recursive_mutex mutex_;
};

}  // namespace bmrt_runtime
}  // namespace tdl_app

