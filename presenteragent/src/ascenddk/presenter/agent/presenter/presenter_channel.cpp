/**
 * ============================================================================
 *
 * Copyright (C) 2018, Hisilicon Technologies Co., Ltd. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1 Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   2 Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 *   3 Neither the names of the copyright holders nor the names of the
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * ============================================================================
 */

#include "ascenddk/presenter/agent/presenter_channel.h"

#include <memory>
#include <sstream>

#include "ascenddk/presenter/agent/channel/default_channel.h"
#include "ascenddk/presenter/agent/net/raw_socket_factory.h"
#include "ascenddk/presenter/agent/presenter/presenter_channel_init_handler.h"
#include "ascenddk/presenter/agent/presenter/presenter_message_helper.h"
#include "ascenddk/presenter/agent/util/logging.h"

using namespace std;
using namespace google::protobuf;

namespace ascend {
namespace presenter {
//创建一个通道(Channel)实例. channel 为创建的通道句柄,param 为创建通道的参数.这里只是Channel实例,并没有连接server的socket
PresenterErrorCode CreateChannel(Channel *&channel,
                                 const OpenChannelParam &param) {
  std::shared_ptr<PresentChannelInitHandler> handler = make_shared<
      PresentChannelInitHandler>(param);
  //创建一个DefaultChannel
  DefaultChannel *ch = DefaultChannel::NewChannel(param.host_ip, param.port, handler);
  if (ch == nullptr) {
    AGENT_LOG_ERROR("Channel new() error");
    return PresenterErrorCode::kBadAlloc;
  }

  // OpenChannelParam to string
  //将创建通道的参数转化为字符串存放到创建的通道对象实例中.这个字符串仅仅只用于日志打印,便于调试和定位
  std::stringstream ss;
  ss << "PresenterChannelImpl: {";
  ss << "server: " << param.host_ip << ":" << param.port;
  ss << ", channel: " << param.channel_name;
  ss << ", content_type: " << static_cast<int>(param.content_type);
  ss << "}";
  ch->SetDescription(ss.str());
  channel = ch;
  return PresenterErrorCode::kNone;
}

PresenterErrorCode OpenChannel(Channel *&channel,
                               const OpenChannelParam &param) {

  // If the channel is not NULL, we cannot know whether it is actually
  // point to something. We cannot be sure whether it is safe to simply
  // delete that, so a kPresenterErrorInvalidParams will be returned
  if (channel != nullptr) {
    AGENT_LOG_ERROR("channel is not NULL");
    return PresenterErrorCode::kInvalidParam;
  }

  // allocate channel object
  //创建一个Channel实例
  PresenterErrorCode error_code = CreateChannel(channel, param);
  if (error_code != PresenterErrorCode::kNone) {
    return error_code;
  }
  //channel 创建的参数字符串写日志
  string channelDesc = channel->GetDescription();

  // Try Open Channel
  AGENT_LOG_INFO("To Open channel: %s", channelDesc.c_str());
  //创建连接socket并连接presenter server,给presenter server发送初始化请求,等待并处理请求的回应
  error_code = channel->Open();

  // If failed, the channel object need to be released
  if (error_code != PresenterErrorCode::kNone) {
	//如果Open失败,获取失败错误码,记录日志,释放channel实例,返回失败
    if (error_code == PresenterErrorCode::kAppDefinedError) {
	  //这一堆代码就是为了调用PresentChannelInitHandler的GetErrorCode()方法获取错误码.channel是Channel类型，需要强转为子类型DefaultChannel;
	  //DefaultChannel的GetInitChannelHandler()返回的是InitChannelHandler类型,需要抢转为子类型PresentChannelInitHandler才能调用GetErrorCode()方法
      DefaultChannel *ch = dynamic_cast<DefaultChannel*>(channel);
      error_code = dynamic_pointer_cast<const PresentChannelInitHandler>(
          ch->GetInitChannelHandler())->GetErrorCode();
    }

    AGENT_LOG_ERROR("OpenChannel Failed, channel = %s, error_code = %d",
                    channelDesc.c_str(), error_code);
    delete channel;
    channel = nullptr;
    return error_code;
  }

  AGENT_LOG_INFO("Channel opened, channel = %s", channelDesc.c_str());
  return PresenterErrorCode::kNone;
}
//presenter agent给presenter server发送数据的接口,channel是发数据的通道,image是待发送的数据
PresenterErrorCode PresentImage(Channel *channel, const ImageFrame &image) {
  if (channel == nullptr) {
    AGENT_LOG_ERROR("channel is NULL");
    return PresenterErrorCode::kInvalidParam;
  }

  proto::PresentImageRequest req;
  //将image打包成proto::PresentImageRequest格式的数据,proto::PresentImageRequest的定义
  //参见proto/presenter_message.proto中的message PresentImageRequest
  if (!PresenterMessageHelper::InitPresentImageRequest(req, image)) {
    return PresenterErrorCode::kInvalidParam;
  }

  //将图片数据打包成TLV格式：TLV是tag, length和value的缩写. Tag是数据类型标记(编号), length是value域的长度. Value打包的数据
  Tlv tlv;
  tlv.tag = proto::PresentImageRequest::kDataFieldNumber;
  tlv.length = image.size;//注意这里size仅仅只是图像数据的长度,都不包括推理结果,并不是整个proto::PresentImageRequest的数据长度
  tlv.value = reinterpret_cast<char *>(image.data);//图片数据

  PartialMessageWithTlvs message;
  message.message = &req;
  message.tlv_list.push_back(tlv);

  std::unique_ptr<Message> recv_message;
  //将打包的数据发给presenter server,并等待和返回server的对该数据包的回应
  PresenterErrorCode error_code = channel->SendMessage(message, recv_message);
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to present image, error = %d", error_code);
    return error_code;
  }
  //从server发回的回应中获取错误码,并翻译成对应的agent定义的错误码,该错误码表示数据发送是否成功
  return PresenterMessageHelper::CheckPresentImageResponse(*recv_message);
}

PresenterErrorCode SendMessage(
        Channel *channel, const google::protobuf::Message& message) {
    if (channel == nullptr) {
        AGENT_LOG_ERROR("channel is NULL");
        return PresenterErrorCode::kInvalidParam;
    }

    unique_ptr<google::protobuf::Message> resp;
    PresenterErrorCode error_code = channel->SendMessage(message, resp);
    if (error_code != PresenterErrorCode::kNone) {
        AGENT_LOG_ERROR("Failed to present image, error = %d", error_code);
        return error_code;
    }

    return PresenterMessageHelper::CheckPresentImageResponse(*resp);
}

}
}

