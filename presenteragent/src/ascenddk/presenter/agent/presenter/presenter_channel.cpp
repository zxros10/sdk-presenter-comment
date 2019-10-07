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
//����һ��ͨ��(Channel)ʵ��. channel Ϊ������ͨ�����,param Ϊ����ͨ���Ĳ���.����ֻ��Channelʵ��,��û������server��socket
PresenterErrorCode CreateChannel(Channel *&channel,
                                 const OpenChannelParam &param) {
  std::shared_ptr<PresentChannelInitHandler> handler = make_shared<
      PresentChannelInitHandler>(param);
  //����һ��DefaultChannel
  DefaultChannel *ch = DefaultChannel::NewChannel(param.host_ip, param.port, handler);
  if (ch == nullptr) {
    AGENT_LOG_ERROR("Channel new() error");
    return PresenterErrorCode::kBadAlloc;
  }

  // OpenChannelParam to string
  //������ͨ���Ĳ���ת��Ϊ�ַ�����ŵ�������ͨ������ʵ����.����ַ�������ֻ������־��ӡ,���ڵ��ԺͶ�λ
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
  //����һ��Channelʵ��
  PresenterErrorCode error_code = CreateChannel(channel, param);
  if (error_code != PresenterErrorCode::kNone) {
    return error_code;
  }
  //channel �����Ĳ����ַ���д��־
  string channelDesc = channel->GetDescription();

  // Try Open Channel
  AGENT_LOG_INFO("To Open channel: %s", channelDesc.c_str());
  //��������socket������presenter server,��presenter server���ͳ�ʼ������,�ȴ�����������Ļ�Ӧ
  error_code = channel->Open();

  // If failed, the channel object need to be released
  if (error_code != PresenterErrorCode::kNone) {
	//���Openʧ��,��ȡʧ�ܴ�����,��¼��־,�ͷ�channelʵ��,����ʧ��
    if (error_code == PresenterErrorCode::kAppDefinedError) {
	  //��һ�Ѵ������Ϊ�˵���PresentChannelInitHandler��GetErrorCode()������ȡ������.channel��Channel���ͣ���ҪǿתΪ������DefaultChannel;
	  //DefaultChannel��GetInitChannelHandler()���ص���InitChannelHandler����,��Ҫ��תΪ������PresentChannelInitHandler���ܵ���GetErrorCode()����
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
//presenter agent��presenter server�������ݵĽӿ�,channel�Ƿ����ݵ�ͨ��,image�Ǵ����͵�����
PresenterErrorCode PresentImage(Channel *channel, const ImageFrame &image) {
  if (channel == nullptr) {
    AGENT_LOG_ERROR("channel is NULL");
    return PresenterErrorCode::kInvalidParam;
  }

  proto::PresentImageRequest req;
  //��image�����proto::PresentImageRequest��ʽ������,proto::PresentImageRequest�Ķ���
  //�μ�proto/presenter_message.proto�е�message PresentImageRequest
  if (!PresenterMessageHelper::InitPresentImageRequest(req, image)) {
    return PresenterErrorCode::kInvalidParam;
  }

  //��ͼƬ���ݴ����TLV��ʽ��TLV��tag, length��value����д. Tag���������ͱ��(���), length��value��ĳ���. Value���������
  Tlv tlv;
  tlv.tag = proto::PresentImageRequest::kDataFieldNumber;
  tlv.length = image.size;//ע������size����ֻ��ͼ�����ݵĳ���,��������������,����������proto::PresentImageRequest�����ݳ���
  tlv.value = reinterpret_cast<char *>(image.data);//ͼƬ����

  PartialMessageWithTlvs message;
  message.message = &req;
  message.tlv_list.push_back(tlv);

  std::unique_ptr<Message> recv_message;
  //����������ݷ���presenter server,���ȴ��ͷ���server�ĶԸ����ݰ��Ļ�Ӧ
  PresenterErrorCode error_code = channel->SendMessage(message, recv_message);
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to present image, error = %d", error_code);
    return error_code;
  }
  //��server���صĻ�Ӧ�л�ȡ������,������ɶ�Ӧ��agent����Ĵ�����,�ô������ʾ���ݷ����Ƿ�ɹ�
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

