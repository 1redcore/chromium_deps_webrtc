/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include <algorithm>  // std::max

#include "webrtc/base/checks.h"
#include "webrtc/base/logging.h"
#include "webrtc/common_types.h"
#include "webrtc/common_video/libyuv/include/webrtc_libyuv.h"
#include "webrtc/modules/video_coding/include/video_codec_interface.h"
#include "webrtc/modules/video_coding/encoded_frame.h"
#include "webrtc/modules/video_coding/utility/quality_scaler.h"
#include "webrtc/modules/video_coding/video_coding_impl.h"
#include "webrtc/system_wrappers/include/clock.h"

namespace webrtc {
namespace vcm {

VideoSender::VideoSender(Clock* clock,
                         EncodedImageCallback* post_encode_callback,
                         VideoEncoderRateObserver* encoder_rate_observer,
                         VCMSendStatisticsCallback* send_stats_callback)
    : clock_(clock),
      _encoder(nullptr),
      _mediaOpt(clock_),
      _encodedFrameCallback(post_encode_callback, &_mediaOpt),
      send_stats_callback_(send_stats_callback),
      _codecDataBase(encoder_rate_observer, &_encodedFrameCallback),
      frame_dropper_enabled_(true),
      _sendStatsTimer(1000, clock_),
      current_codec_(),
      encoder_params_({0, 0, 0, 0}),
      encoder_has_internal_source_(false),
      next_frame_types_(1, kVideoFrameDelta) {
  _mediaOpt.Reset();
  // Allow VideoSender to be created on one thread but used on another, post
  // construction. This is currently how this class is being used by at least
  // one external project (diffractor).
  main_thread_.DetachFromThread();
}

VideoSender::~VideoSender() {}

void VideoSender::Process() {
  if (_sendStatsTimer.TimeUntilProcess() == 0) {
    // |_sendStatsTimer.Processed()| must be called. Otherwise
    // VideoSender::Process() will be called in an infinite loop.
    _sendStatsTimer.Processed();
    if (send_stats_callback_) {
      uint32_t bitRate = _mediaOpt.SentBitRate();
      uint32_t frameRate = _mediaOpt.SentFrameRate();
      std::string encoder_name;
      {
        rtc::CritScope cs(&params_crit_);
        // Copy the string here so that we don't hold |params_crit_| in the CB.
        encoder_name = encoder_name_;
      }
      send_stats_callback_->SendStatistics(bitRate, frameRate, encoder_name);
    }
  }

  {
    rtc::CritScope cs(&params_crit_);
    // Force an encoder parameters update, so that incoming frame rate is
    // updated even if bandwidth hasn't changed.
    encoder_params_.input_frame_rate = _mediaOpt.InputFrameRate();
  }
}

int64_t VideoSender::TimeUntilNextProcess() {
  return _sendStatsTimer.TimeUntilProcess();
}

// Register the send codec to be used.
int32_t VideoSender::RegisterSendCodec(const VideoCodec* sendCodec,
                                       uint32_t numberOfCores,
                                       uint32_t maxPayloadSize) {
  RTC_DCHECK(main_thread_.CalledOnValidThread());
  rtc::CritScope lock(&encoder_crit_);
  if (sendCodec == nullptr) {
    return VCM_PARAMETER_ERROR;
  }

  bool ret =
      _codecDataBase.SetSendCodec(sendCodec, numberOfCores, maxPayloadSize);

  // Update encoder regardless of result to make sure that we're not holding on
  // to a deleted instance.
  _encoder = _codecDataBase.GetEncoder();
  // Cache the current codec here so they can be fetched from this thread
  // without requiring the _sendCritSect lock.
  current_codec_ = *sendCodec;

  if (!ret) {
    LOG(LS_ERROR) << "Failed to initialize set encoder with payload name '"
                  << sendCodec->plName << "'.";
    return VCM_CODEC_ERROR;
  }

  // SetSendCodec succeeded, _encoder should be set.
  RTC_DCHECK(_encoder);

  int numLayers;
  if (sendCodec->codecType == kVideoCodecVP8) {
    numLayers = sendCodec->codecSpecific.VP8.numberOfTemporalLayers;
  } else if (sendCodec->codecType == kVideoCodecVP9) {
    numLayers = sendCodec->codecSpecific.VP9.numberOfTemporalLayers;
  } else {
    numLayers = 1;
  }

  // If we have screensharing and we have layers, we disable frame dropper.
  bool disable_frame_dropper =
      numLayers > 1 && sendCodec->mode == kScreensharing;
  if (disable_frame_dropper) {
    _mediaOpt.EnableFrameDropper(false);
  } else if (frame_dropper_enabled_) {
    _mediaOpt.EnableFrameDropper(true);
  }
  {
    rtc::CritScope cs(&params_crit_);
    next_frame_types_.clear();
    next_frame_types_.resize(VCM_MAX(sendCodec->numberOfSimulcastStreams, 1),
                             kVideoFrameKey);
    // Cache InternalSource() to have this available from IntraFrameRequest()
    // without having to acquire encoder_crit_ (avoid blocking on encoder use).
    encoder_has_internal_source_ = _encoder->InternalSource();
  }

  LOG(LS_VERBOSE) << " max bitrate " << sendCodec->maxBitrate
                  << " start bitrate " << sendCodec->startBitrate
                  << " max frame rate " << sendCodec->maxFramerate
                  << " max payload size " << maxPayloadSize;
  _mediaOpt.SetEncodingData(sendCodec->maxBitrate * 1000,
                            sendCodec->startBitrate * 1000, sendCodec->width,
                            sendCodec->height, sendCodec->maxFramerate,
                            numLayers, maxPayloadSize);
  return VCM_OK;
}

// Register an external decoder object.
// This can not be used together with external decoder callbacks.
void VideoSender::RegisterExternalEncoder(VideoEncoder* externalEncoder,
                                          uint8_t payloadType,
                                          bool internalSource /*= false*/) {
  RTC_DCHECK(main_thread_.CalledOnValidThread());

  rtc::CritScope lock(&encoder_crit_);

  if (externalEncoder == nullptr) {
    bool wasSendCodec = false;
    RTC_CHECK(
        _codecDataBase.DeregisterExternalEncoder(payloadType, &wasSendCodec));
    if (wasSendCodec) {
      // Make sure the VCM doesn't use the de-registered codec
      rtc::CritScope params_lock(&params_crit_);
      _encoder = nullptr;
      encoder_has_internal_source_ = false;
    }
    return;
  }
  _codecDataBase.RegisterExternalEncoder(externalEncoder, payloadType,
                                         internalSource);
}

// Get encode bitrate
int VideoSender::Bitrate(unsigned int* bitrate) const {
  RTC_DCHECK(main_thread_.CalledOnValidThread());
  // Since we're running on the thread that's the only thread known to modify
  // the value of _encoder, we don't need to grab the lock here.

  if (!_encoder)
    return VCM_UNINITIALIZED;
  *bitrate = _encoder->GetEncoderParameters().target_bitrate;
  return 0;
}

// Get encode frame rate
int VideoSender::FrameRate(unsigned int* framerate) const {
  RTC_DCHECK(main_thread_.CalledOnValidThread());
  // Since we're running on the thread that's the only thread known to modify
  // the value of _encoder, we don't need to grab the lock here.

  if (!_encoder)
    return VCM_UNINITIALIZED;

  *framerate = _encoder->GetEncoderParameters().input_frame_rate;
  return 0;
}

int32_t VideoSender::SetChannelParameters(uint32_t target_bitrate,
                                          uint8_t lossRate,
                                          int64_t rtt) {
  uint32_t target_rate =
      _mediaOpt.SetTargetRates(target_bitrate, lossRate, rtt);

  uint32_t input_frame_rate = _mediaOpt.InputFrameRate();

  EncoderParameters encoder_params = {target_rate, lossRate, rtt,
                                      input_frame_rate};
  bool encoder_has_internal_source;
  {
    rtc::CritScope cs(&params_crit_);
    encoder_params_ = encoder_params;
    encoder_has_internal_source = encoder_has_internal_source_;
  }

  // For encoders with internal sources, we need to tell the encoder directly,
  // instead of waiting for an AddVideoFrame that will never come (internal
  // source encoders don't get input frames).
  if (encoder_has_internal_source) {
    rtc::CritScope cs(&encoder_crit_);
    if (_encoder) {
      SetEncoderParameters(encoder_params);
    }
  }

  return VCM_OK;
}

void VideoSender::SetEncoderParameters(EncoderParameters params) {
  // |target_bitrate == 0 | means that the network is down or the send pacer is
  // full.
  // TODO(perkj): Consider setting |target_bitrate| == 0 to the encoders.
  // Especially if |encoder_has_internal_source_ | == true.
  if (params.target_bitrate == 0)
    return;

  if (params.input_frame_rate == 0) {
    // No frame rate estimate available, use default.
    params.input_frame_rate = current_codec_.maxFramerate;
  }
  if (_encoder != nullptr)
    _encoder->SetEncoderParameters(params);
}

// Deprecated:
// TODO(perkj): Remove once no projects call this method. It currently do
// nothing.
int32_t VideoSender::RegisterProtectionCallback(
    VCMProtectionCallback* protection_callback) {
  // Deprecated:
  // TODO(perkj): Remove once no projects call this method. It currently do
  // nothing.
  return VCM_OK;
}

// Add one raw video frame to the encoder, blocking.
int32_t VideoSender::AddVideoFrame(const VideoFrame& videoFrame,
                                   const CodecSpecificInfo* codecSpecificInfo) {
  EncoderParameters encoder_params;
  std::vector<FrameType> next_frame_types;
  {
    rtc::CritScope lock(&params_crit_);
    encoder_params = encoder_params_;
    next_frame_types = next_frame_types_;
  }
  rtc::CritScope lock(&encoder_crit_);
  if (_encoder == nullptr)
    return VCM_UNINITIALIZED;
  SetEncoderParameters(encoder_params);
  if (_mediaOpt.DropFrame()) {
    LOG(LS_VERBOSE) << "Drop Frame "
                    << "target bitrate " << encoder_params.target_bitrate
                    << " loss rate " << encoder_params.loss_rate << " rtt "
                    << encoder_params.rtt << " input frame rate "
                    << encoder_params.input_frame_rate;
    _encoder->OnDroppedFrame();
    return VCM_OK;
  }
  // TODO(pbos): Make sure setting send codec is synchronized with video
  // processing so frame size always matches.
  if (!_codecDataBase.MatchesCurrentResolution(videoFrame.width(),
                                               videoFrame.height())) {
    LOG(LS_ERROR) << "Incoming frame doesn't match set resolution. Dropping.";
    return VCM_PARAMETER_ERROR;
  }
  VideoFrame converted_frame = videoFrame;
  if (converted_frame.video_frame_buffer()->native_handle() &&
      !_encoder->SupportsNativeHandle()) {
    // This module only supports software encoding.
    // TODO(pbos): Offload conversion from the encoder thread.
    rtc::scoped_refptr<VideoFrameBuffer> converted_buffer(
        converted_frame.video_frame_buffer()->NativeToI420Buffer());

    if (!converted_buffer) {
      LOG(LS_ERROR) << "Frame conversion failed, dropping frame.";
      return VCM_PARAMETER_ERROR;
    }
    converted_frame = VideoFrame(converted_buffer,
                                 converted_frame.timestamp(),
                                 converted_frame.render_time_ms(),
                                 converted_frame.rotation());
  }
  int32_t ret =
      _encoder->Encode(converted_frame, codecSpecificInfo, next_frame_types);
  if (ret < 0) {
    LOG(LS_ERROR) << "Failed to encode frame. Error code: " << ret;
    return ret;
  }

  {
    rtc::CritScope lock(&params_crit_);
    encoder_name_ = _encoder->ImplementationName();

    // Change all keyframe requests to encode delta frames the next time.
    for (size_t i = 0; i < next_frame_types_.size(); ++i) {
      // Check for equality (same requested as before encoding) to not
      // accidentally drop a keyframe request while encoding.
      if (next_frame_types[i] == next_frame_types_[i])
        next_frame_types_[i] = kVideoFrameDelta;
    }
  }
  return VCM_OK;
}

int32_t VideoSender::IntraFrameRequest(size_t stream_index) {
  {
    rtc::CritScope lock(&params_crit_);
    if (stream_index >= next_frame_types_.size()) {
      return -1;
    }
    next_frame_types_[stream_index] = kVideoFrameKey;
    if (!encoder_has_internal_source_)
      return VCM_OK;
  }
  // TODO(pbos): Remove when InternalSource() is gone. Both locks have to be
  // held here for internal consistency, since _encoder could be removed while
  // not holding encoder_crit_. Checks have to be performed again since
  // params_crit_ was dropped to not cause lock-order inversions with
  // encoder_crit_.
  rtc::CritScope lock(&encoder_crit_);
  rtc::CritScope params_lock(&params_crit_);
  if (stream_index >= next_frame_types_.size())
    return -1;
  if (_encoder != nullptr && _encoder->InternalSource()) {
    // Try to request the frame if we have an external encoder with
    // internal source since AddVideoFrame never will be called.
    if (_encoder->RequestFrame(next_frame_types_) == WEBRTC_VIDEO_CODEC_OK) {
      // Try to remove just-performed keyframe request, if stream still exists.
      next_frame_types_[stream_index] = kVideoFrameDelta;
    }
  }
  return VCM_OK;
}

int32_t VideoSender::EnableFrameDropper(bool enable) {
  rtc::CritScope lock(&encoder_crit_);
  frame_dropper_enabled_ = enable;
  _mediaOpt.EnableFrameDropper(enable);
  return VCM_OK;
}

void VideoSender::SuspendBelowMinBitrate() {
  RTC_DCHECK(main_thread_.CalledOnValidThread());
  int threshold_bps;
  if (current_codec_.numberOfSimulcastStreams == 0) {
    threshold_bps = current_codec_.minBitrate * 1000;
  } else {
    threshold_bps = current_codec_.simulcastStream[0].minBitrate * 1000;
  }
  // Set the hysteresis window to be at 10% of the threshold, but at least
  // 10 kbps.
  int window_bps = std::max(threshold_bps / 10, 10000);
  _mediaOpt.SuspendBelowMinBitrate(threshold_bps, window_bps);
}

bool VideoSender::VideoSuspended() const {
  return _mediaOpt.IsVideoSuspended();
}
}  // namespace vcm
}  // namespace webrtc
