/*
 *  Copyright 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/video_rtp_receiver.h"

#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

#include "api/video/recordable_encoded_frame.h"
#include "pc/video_track.h"
#include "rtc_base/checks.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/task_utils/to_queued_task.h"

namespace webrtc {

VideoRtpReceiver::VideoRtpReceiver(rtc::Thread* worker_thread,
                                   std::string receiver_id,
                                   std::vector<std::string> stream_ids)
    : VideoRtpReceiver(worker_thread,
                       receiver_id,
                       CreateStreamsFromIds(std::move(stream_ids))) {}

VideoRtpReceiver::VideoRtpReceiver(
    rtc::Thread* worker_thread,
    const std::string& receiver_id,
    const std::vector<rtc::scoped_refptr<MediaStreamInterface>>& streams)
    : worker_thread_(worker_thread),
      id_(receiver_id),
      source_(rtc::make_ref_counted<VideoRtpTrackSource>(&source_callback_)),
      track_(VideoTrackProxyWithInternal<VideoTrack>::Create(
          rtc::Thread::Current(),
          worker_thread,
          VideoTrack::Create(receiver_id, source_, worker_thread))),
      cached_track_should_receive_(track_->should_receive()),
      attachment_id_(GenerateUniqueId()),
      worker_thread_safety_(PendingTaskSafetyFlag::CreateDetachedInactive()) {
  RTC_DCHECK(worker_thread_);
  SetStreams(streams);
  track_->RegisterObserver(this);
  RTC_DCHECK_EQ(source_->state(), MediaSourceInterface::kInitializing);
}

VideoRtpReceiver::~VideoRtpReceiver() {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  RTC_DCHECK(!media_channel_);

  track_->UnregisterObserver(this);
}

std::vector<std::string> VideoRtpReceiver::stream_ids() const {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  std::vector<std::string> stream_ids(streams_.size());
  for (size_t i = 0; i < streams_.size(); ++i)
    stream_ids[i] = streams_[i]->id();
  return stream_ids;
}

rtc::scoped_refptr<DtlsTransportInterface> VideoRtpReceiver::dtls_transport()
    const {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  return dtls_transport_;
}

std::vector<rtc::scoped_refptr<MediaStreamInterface>>
VideoRtpReceiver::streams() const {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  return streams_;
}

RtpParameters VideoRtpReceiver::GetParameters() const {
  RTC_DCHECK_RUN_ON(worker_thread_);
  if (!media_channel_)
    return RtpParameters();
  return ssrc_ ? media_channel_->GetRtpReceiveParameters(*ssrc_)
               : media_channel_->GetDefaultRtpReceiveParameters();
}

void VideoRtpReceiver::SetFrameDecryptor(
    rtc::scoped_refptr<FrameDecryptorInterface> frame_decryptor) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  frame_decryptor_ = std::move(frame_decryptor);
  // Special Case: Set the frame decryptor to any value on any existing channel.
  if (media_channel_ && ssrc_) {
    media_channel_->SetFrameDecryptor(*ssrc_, frame_decryptor_);
  }
}

rtc::scoped_refptr<FrameDecryptorInterface>
VideoRtpReceiver::GetFrameDecryptor() const {
  RTC_DCHECK_RUN_ON(worker_thread_);
  return frame_decryptor_;
}

void VideoRtpReceiver::LTRequestKeyFrame() {
  fprintf(stderr, "VideoRtpReceiver::LTRequestKeyFrame\n");
  OnGenerateKeyFrame();
}

void VideoRtpReceiver::SetDepacketizerToDecoderFrameTransformer(
    rtc::scoped_refptr<FrameTransformerInterface> frame_transformer) {
  fprintf(stderr, "VideoRtpReceiver::SetDepacketizerToDecoderFrameTransformer\n");
  RTC_DCHECK_RUN_ON(worker_thread_);
  frame_transformer_ = std::move(frame_transformer);
  if (media_channel_) {
    media_channel_->SetDepacketizerToDecoderFrameTransformer(
        ssrc_.value_or(0), frame_transformer_);
  }
}

void VideoRtpReceiver::Stop() {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  source_->SetState(MediaSourceInterface::kEnded);
  track_->internal()->set_ended();
}

void VideoRtpReceiver::OnChanged() {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  if (cached_track_should_receive_ != track_->should_receive()) {
    cached_track_should_receive_ = track_->should_receive();
    worker_thread_->PostTask(ToQueuedTask(
        worker_thread_safety_,
        [this, receive = cached_track_should_receive_]() {
          RTC_DCHECK_RUN_ON(worker_thread_);
          if(receive) {
            StartMediaChannel();
          } else {
            StopMediaChannel();
          }
        }));
  }
}

void VideoRtpReceiver::StartMediaChannel() {  
  RTC_DCHECK_RUN_ON(worker_thread_);
  if (!media_channel_) {
    return;
  }
  media_channel_->StartReceive(ssrc_.value_or(0));
  OnGenerateKeyFrame();
}

void VideoRtpReceiver::StopMediaChannel() {
  RTC_DCHECK_RUN_ON(worker_thread_);
  if (!media_channel_) {
    return;
  }
  media_channel_->StopReceive(ssrc_.value_or(0));
}

// RTC_RUN_ON(&signaling_thread_checker_)
void VideoRtpReceiver::RestartMediaChannel(absl::optional<uint32_t> ssrc) {
  MediaSourceInterface::SourceState state = source_->state();
  // TODO(tommi): Can we restart the media channel without blocking?
  worker_thread_->Invoke<void>(RTC_FROM_HERE, [&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    RestartMediaChannel_w(std::move(ssrc), state);
  });
  source_->SetState(MediaSourceInterface::kLive);
}

// RTC_RUN_ON(worker_thread_)
void VideoRtpReceiver::RestartMediaChannel_w(
    absl::optional<uint32_t> ssrc,
    MediaSourceInterface::SourceState state) {
  if (!media_channel_) {
    return;  // Can't restart.
  }

  const bool encoded_sink_enabled = saved_encoded_sink_enabled_;

  if (state != MediaSourceInterface::kInitializing) {
    if (ssrc == ssrc_)
      return;

    // Disconnect from a previous ssrc.
    SetSink(nullptr);

    if (encoded_sink_enabled)
      SetEncodedSinkEnabled(false);
  }

  // Set up the new ssrc.
  ssrc_ = std::move(ssrc);
  SetSink(source_->sink());
  if (encoded_sink_enabled) {
    SetEncodedSinkEnabled(true);
  }

  if (frame_transformer_ && media_channel_) {
    media_channel_->SetDepacketizerToDecoderFrameTransformer(
        ssrc_.value_or(0), frame_transformer_);
  }

  if (media_channel_ && ssrc_) {
    if (frame_decryptor_) {
      media_channel_->SetFrameDecryptor(*ssrc_, frame_decryptor_);
    }

    media_channel_->SetBaseMinimumPlayoutDelayMs(*ssrc_, delay_.GetMs());
  }
}

// RTC_RUN_ON(worker_thread_)
void VideoRtpReceiver::SetSink(rtc::VideoSinkInterface<VideoFrame>* sink) {
  if (ssrc_) {
    media_channel_->SetSink(*ssrc_, sink);
  } else {
    media_channel_->SetDefaultSink(sink);
  }
}

void VideoRtpReceiver::SetupMediaChannel(uint32_t ssrc) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  RestartMediaChannel(ssrc);
}

void VideoRtpReceiver::SetupUnsignaledMediaChannel() {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  RestartMediaChannel(absl::nullopt);
}

uint32_t VideoRtpReceiver::ssrc() const {
  RTC_DCHECK_RUN_ON(worker_thread_);
  return ssrc_.value_or(0);
}

void VideoRtpReceiver::set_stream_ids(std::vector<std::string> stream_ids) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  SetStreams(CreateStreamsFromIds(std::move(stream_ids)));
}

void VideoRtpReceiver::set_transport(
    rtc::scoped_refptr<DtlsTransportInterface> dtls_transport) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  dtls_transport_ = std::move(dtls_transport);
}

void VideoRtpReceiver::SetStreams(
    const std::vector<rtc::scoped_refptr<MediaStreamInterface>>& streams) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  
  // Remove remote track from any streams that are going away.
  for (const auto& existing_stream : streams_) {
    bool removed = true;
    for (const auto& stream : streams) {
      if (existing_stream->id() == stream->id()) {
        RTC_DCHECK_EQ(existing_stream.get(), stream.get());
        removed = false;
        break;
      }
    }
    if (removed) {
      existing_stream->RemoveTrack(video_track());
    }
  }
  // Add remote track to any streams that are new.
  for (const auto& stream : streams) {
    bool added = true;
    for (const auto& existing_stream : streams_) {
      if (stream->id() == existing_stream->id()) {
        RTC_DCHECK_EQ(stream.get(), existing_stream.get());
        added = false;
        break;
      }
    }
    if (added) {
      stream->AddTrack(video_track());
    }
  }
  streams_ = streams;
}

void VideoRtpReceiver::SetObserver(RtpReceiverObserverInterface* observer) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  observer_ = observer;
  // Deliver any notifications the observer may have missed by being set late.
  if (received_first_packet_ && observer_) {
    observer_->OnFirstPacketReceived(media_type());
  }
}

void VideoRtpReceiver::SetJitterBufferMinimumDelay(
    absl::optional<double> delay_seconds) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  delay_.Set(delay_seconds);
  if (media_channel_ && ssrc_)
    media_channel_->SetBaseMinimumPlayoutDelayMs(*ssrc_, delay_.GetMs());
}

void VideoRtpReceiver::SetMediaChannel(cricket::MediaChannel* media_channel) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  RTC_DCHECK(media_channel == nullptr ||
             media_channel->media_type() == media_type());

  SetMediaChannel_w(media_channel);
}

// RTC_RUN_ON(worker_thread_)
void VideoRtpReceiver::SetMediaChannel_w(cricket::MediaChannel* media_channel) {
  if (media_channel == media_channel_)
    return;

  if (!media_channel) {
    SetSink(nullptr);
  }

  bool encoded_sink_enabled = saved_encoded_sink_enabled_;
  if (encoded_sink_enabled && media_channel_) {
    // Turn off the old sink, if any.
    SetEncodedSinkEnabled(false);
  }

  media_channel ? worker_thread_safety_->SetAlive()
                : worker_thread_safety_->SetNotAlive();
  media_channel_ = static_cast<cricket::VideoMediaChannel*>(media_channel);

  if (media_channel_) {
    if (saved_generate_keyframe_) {
      // TODO(bugs.webrtc.org/8694): Stop using 0 to mean unsignalled SSRC
      media_channel_->GenerateKeyFrame(ssrc_.value_or(0));
      saved_generate_keyframe_ = false;
    }
    if (encoded_sink_enabled) {
      SetEncodedSinkEnabled(true);
    }
    if (frame_transformer_) {
      media_channel_->SetDepacketizerToDecoderFrameTransformer(
          ssrc_.value_or(0), frame_transformer_);
    }
  }

  if (!media_channel)
    source_->ClearCallback();
}

void VideoRtpReceiver::NotifyFirstPacketReceived() {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  if (observer_) {
    observer_->OnFirstPacketReceived(media_type());
  }
  received_first_packet_ = true;
}

std::vector<RtpSource> VideoRtpReceiver::GetSources() const {
  RTC_DCHECK_RUN_ON(worker_thread_);
  if (!ssrc_ || !media_channel_)
    return std::vector<RtpSource>();
  return media_channel_->GetSources(*ssrc_);
}

void VideoRtpReceiver::SetupMediaChannel(absl::optional<uint32_t> ssrc,
                                         cricket::MediaChannel* media_channel) {
  RTC_DCHECK_RUN_ON(&signaling_thread_checker_);
  RTC_DCHECK(media_channel);
  MediaSourceInterface::SourceState state = source_->state();
  worker_thread_->Invoke<void>(RTC_FROM_HERE, [&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    SetMediaChannel_w(media_channel);
    RestartMediaChannel_w(std::move(ssrc), state);
  });
  source_->SetState(MediaSourceInterface::kLive);
}

void VideoRtpReceiver::OnGenerateKeyFrame() {
  RTC_DCHECK_RUN_ON(worker_thread_);
  if (!media_channel_) {
    RTC_LOG(LS_ERROR)
        << "VideoRtpReceiver::OnGenerateKeyFrame: No video channel exists.";
    return;
  }
  // TODO(bugs.webrtc.org/8694): Stop using 0 to mean unsignalled SSRC
  media_channel_->GenerateKeyFrame(ssrc_.value_or(0));
  // We need to remember to request generation of a new key frame if the media
  // channel changes, because there's no feedback whether the keyframe
  // generation has completed on the channel.
  saved_generate_keyframe_ = true;
}

void VideoRtpReceiver::OnEncodedSinkEnabled(bool enable) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  SetEncodedSinkEnabled(enable);
  // Always save the latest state of the callback in case the media_channel_
  // changes.
  saved_encoded_sink_enabled_ = enable;
}

// RTC_RUN_ON(worker_thread_)
void VideoRtpReceiver::SetEncodedSinkEnabled(bool enable) {
  if (!media_channel_)
    return;

  // TODO(bugs.webrtc.org/8694): Stop using 0 to mean unsignalled SSRC
  const auto ssrc = ssrc_.value_or(0);

  if (enable) {
    media_channel_->SetRecordableEncodedFrameCallback(
        ssrc, [source = source_](const RecordableEncodedFrame& frame) {
          source->BroadcastRecordableEncodedFrame(frame);
        });
  } else {
    media_channel_->ClearRecordableEncodedFrameCallback(ssrc);
  }
}

}  // namespace webrtc
