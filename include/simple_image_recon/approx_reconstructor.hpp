// -*-c++-*---------------------------------------------------------------------------------------
// Copyright 2023 Bernd Pfrommer <bernd.pfrommer@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SIMPLE_IMAGE_RECON__APPROX_RECONSTRUCTOR_HPP_
#define SIMPLE_IMAGE_RECON__APPROX_RECONSTRUCTOR_HPP_

#include <event_array_codecs/decoder_factory.h>
#include <event_array_codecs/event_processor.h>

#include <memory>
#include <simple_image_recon_lib/simple_image_reconstructor.hpp>
#include <string>

#include "simple_image_recon/check_endian.hpp"
#include "simple_image_recon/frame_handler.hpp"

#define SEC_TO_NSEC(X) (X*1000000000)

namespace simple_image_recon
{
template <
  typename EventArrayT, typename EventArrayConstSharedPtrT, typename ImageT,
  typename ImageConstPtrT>
class ApproxReconstructor : public event_array_codecs::EventProcessor
{
public:
  using EventArray = EventArrayT;
  explicit ApproxReconstructor(
    FrameHandler<ImageConstPtrT> * fh, const std::string & topic,
    int cutoffNumEvents = 30, double fps = 25.0, double fillRatio = 0.6,
    int tileSize = 2, int64_t offset = 0, std::vector<int64_t> frameTimes = {})
  : frameHandler_(fh),
    topic_(topic),
    cutoffNumEvents_(cutoffNumEvents),
    fillRatio_(fillRatio),
    tileSize_(tileSize)
  {
    if ( frameTimes.size() > 0 ){
      useSliceInterval_ = false;
      addFrameTimes(frameTimes);
    }

    timeOffset_ = static_cast<int64_t>(offset);
    sliceInterval_ = static_cast<int64_t>(SEC_TO_NSEC(1) / std::abs(fps));
    imageMsgTemplate_.height = 0;
  }

  // ---------- inherited from EventProcessor
  inline void eventCD(
    uint64_t t, uint16_t ex, uint16_t ey, uint8_t polarity) override
  {
    simpleReconstructor_.event(t, ex, ey, polarity);
    while ( (int64_t)t + timeOffset_ > nextFrameTime_) {
      emitFrame();
      setNextTime();
    }
  }
  void eventExtTrigger(uint64_t, uint8_t, uint8_t) override {}
  void finished() override{};
  void rawData(const char *, size_t) override{};
  // --------- end of inherited from EventProcessor

  int64_t getT0() const { return (t0_); }

  void processMsg(EventArrayConstSharedPtrT msg)
  {
    if (imageMsgTemplate_.height == 0) {
      imageMsgTemplate_.header = msg->header;
      imageMsgTemplate_.width = msg->width;
      imageMsgTemplate_.height = msg->height;
      imageMsgTemplate_.encoding = "mono8";
      imageMsgTemplate_.is_bigendian = check_endian::isBigEndian();
      imageMsgTemplate_.step = imageMsgTemplate_.width;
      // imageMsgTemplate_.data.resize(msg->width * msg->height, 0);
      FirstMsgProcessor firstMsgProcessor;
      event_array_codecs::DecoderFactory<FirstMsgProcessor> firstFactory;
      auto firstDecoder =
        firstFactory.getInstance(msg->encoding, msg->width, msg->height);
      firstDecoder->decode(
        &(msg->events[0]), msg->events.size(), &firstMsgProcessor);
      std::cout << "First timestamp " << firstMsgProcessor.getFirstTimeStamp() << std::endl;
      t0_ = (int64_t)firstMsgProcessor.getFirstTimeStamp() + timeOffset_;
      std::cout << "First time " << t0_ << std::endl;
      setFirstTime();
      simpleReconstructor_.initialize(
        msg->width, msg->height,
        static_cast<uint32_t>(std::abs(cutoffNumEvents_)), tileSize_,
        fillRatio_);
      decoder_ =
        decoderFactory_.getInstance(msg->encoding, msg->width, msg->height);
      if (!decoder_) {
        std::cerr << "invalid encoding: " << msg->encoding << std::endl;
        throw(std::runtime_error("invalid encoding!"));
      }
    }
    decoder_->decode(&(msg->events[0]), msg->events.size(), this);
  }

private:
  // special handling for first message
  class FirstMsgProcessor : public event_array_codecs::EventProcessor
  {
    // ---------- inherited from EventProcessor
  public:
    void eventCD(uint64_t t, uint16_t, uint16_t, uint8_t) override
    {
      if (firstTimeStamp_ == 0) {
        firstTimeStamp_ = t;
      }
    }
    void eventExtTrigger(uint64_t, uint8_t, uint8_t) override {}
    void finished() override{};
    void rawData(const char *, size_t) override{};
    // --------- end of inherited from EventProcessor
    int64_t getFirstTimeStamp() const { return (firstTimeStamp_); }
    // --------- variables -----------
  private:
    int64_t firstTimeStamp_{0};
  };

  void setFirstTime()
  {
    if(useSliceInterval_){
      nextFrameTime_ = (t0_ / sliceInterval_) * sliceInterval_;
    } else {
      setNextTime();
      while(nextFrameTime_ < t0_){
        setNextTime();
      }
    }
  }

  void setNextTime()
  {
    if(useSliceInterval_){
      nextFrameTime_ += sliceInterval_;
    } else {
      if (sliceTimes_.empty()){
        nextFrameTime_ += sliceInterval_;
      } else {
        nextFrameTime_ = sliceTimes_.front();
        sliceTimes_.pop();
      }
    }
  }

  void addFrameTime(int64_t t)
  {
    sliceTimes_.push(t);
  }

  void addFrameTimes(std::vector<int64_t>& ts)
  {
    for(int64_t& t : ts)
      addFrameTime(t);
  }

  bool hasFrameTimes()
  {
    return !sliceTimes_.empty();
  }

  void emitFrame()
  {
    auto msg = std::make_unique<ImageT>(imageMsgTemplate_);
    msg->data.resize(msg->height * msg->step);
    simpleReconstructor_.getImage(&(msg->data[0]), msg->step);
#ifdef USING_ROS_1
    ros::Time t;
    t.fromNSec(nextFrameTime_);
    msg->header.stamp = t;
#else
    msg->header.stamp = rclcpp::Time(nextFrameTime_, RCL_SYSTEM_TIME);
#endif
    frameHandler_->frame(std::move(msg), topic_);
  }

  // ------------------------  variables ------------------------------
  FrameHandler<ImageConstPtrT> * frameHandler_{nullptr};
  std::string topic_;
  ImageT imageMsgTemplate_;
  int cutoffNumEvents_{0};
  int64_t sliceInterval_{0};
  std::queue<int64_t> sliceTimes_;
  bool useSliceInterval_{true};
  int64_t nextFrameTime_{0};
  int64_t t0_{0};
  double fillRatio_{0};
  int tileSize_{0};
  int64_t timeOffset_{0};
  event_array_codecs::Decoder<ApproxReconstructor> * decoder_{0};
  event_array_codecs::DecoderFactory<ApproxReconstructor> decoderFactory_;
  simple_image_recon_lib::SimpleImageReconstructor simpleReconstructor_;
};
}  // namespace simple_image_recon
#endif  // SIMPLE_IMAGE_RECON__APPROX_RECONSTRUCTOR_HPP_
