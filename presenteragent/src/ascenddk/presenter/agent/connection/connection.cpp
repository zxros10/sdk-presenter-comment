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

#include "ascenddk/presenter/agent/connection/connection.h"

#include <netinet/in.h>
#include <sstream>

#include "ascenddk/presenter/agent/codec/message_codec.h"
#include "ascenddk/presenter/agent/net/raw_socket_factory.h"
#include "ascenddk/presenter/agent/util/byte_buffer.h"
#include "ascenddk/presenter/agent/util/logging.h"
#include "ascenddk/presenter/agent/util/mem_utils.h"


namespace {
  const uint32_t kMaxPacketSize = 1024 * 1024 * 10; //10MB
}

namespace ascend {
namespace presenter {

using google::protobuf::Message;
using namespace std;

Connection::Connection(Socket* socket)
    : socket_(socket) {
}

Connection* Connection::New(Socket* socket) {
  if (socket == nullptr) {
    AGENT_LOG_ERROR("socket is null");
    return nullptr;
  }

  return new (nothrow) Connection(socket);
}

PresenterErrorCode Connection::SendTlvList(const std::vector<Tlv>& tlv_list) {
  if (tlv_list.empty()) {
    return PresenterErrorCode::kNone;
  }

  for (auto it = tlv_list.begin(); it != tlv_list.end(); ++it) {
    SharedByteBuffer tlv_buf = codec_.EncodeTagAndLength(*it);
    if (tlv_buf.IsEmpty()) {
      AGENT_LOG_ERROR("Failed to encode TLV");
      return PresenterErrorCode::kCodec;
    }

    //send tag and length 发送TLV封装数据
    PresenterErrorCode error_code = socket_->Send(tlv_buf.Get(),
                                                  tlv_buf.Size());
    if (error_code != PresenterErrorCode::kNone) {
      AGENT_LOG_ERROR("Failed to send TLV tag and length");
      return error_code;
    }

    //send value 发送实际的TLV(图片)数据
    error_code = socket_->Send(it->value, it->length);
    if (error_code != PresenterErrorCode::kNone) {
      AGENT_LOG_ERROR("Failed to send TLV value");
      return error_code;
    }
  }

  return PresenterErrorCode::kNone;
}

PresenterErrorCode Connection::SendMessage(
    const PartialMessageWithTlvs& proto_message) {
  if (proto_message.message == nullptr) {
    AGENT_LOG_ERROR("message is null");
    return PresenterErrorCode::kInvalidParam;
  }
  // lock for encoding and sending
  // 任意时刻只能由一个线程通过本连接（Connection）发送消息
  unique_lock<mutex> lock(mtx_);
  //获取PartialMessageWithTlvs打包的proto数据格式类型名称
  const char* msg_name = proto_message.message->GetDescriptor()->name().c_str();
  //将proto数据序列化为二进制字节流(包括推理结果,但是不包括图片数据）
  SharedByteBuffer buffer = codec_.EncodeMessage(proto_message);
  if (buffer.IsEmpty()) {
    AGENT_LOG_ERROR("Failed to encode message: %s", msg_name);
    return PresenterErrorCode::kCodec;
  }

  // send message 发送数据
  PresenterErrorCode error_code = socket_->Send(buffer.Get(), buffer.Size());
  // if send success and has more to send..
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to send message: %s", msg_name);
    return error_code;
  }
  //发送(由图片数据打包而成)的tlv数据,
  return SendTlvList(proto_message.tlv_list);
}

PresenterErrorCode Connection::SendMessage(const Message& message) {
  PartialMessageWithTlvs msg;
  msg.message = &message;
  return SendMessage(msg);
}

PresenterErrorCode Connection::ReceiveMessage(
    unique_ptr<::google::protobuf::Message>& message) {
  // read 4 bytes header
  char *buf = recv_buf_;
  //接收消息
  PresenterErrorCode error_code = socket_->Recv(
      buf, MessageCodec::kPacketLengthSize);
  //如果接收超时,没有收到消息返回socket超时
  if (error_code == PresenterErrorCode::kSocketTimeout) {
    AGENT_LOG_INFO("Read message header timeout");
    return PresenterErrorCode::kSocketTimeout;
  }
  //如果是其他错误
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to read message header");
    return error_code;
  }

  // parse length 解析收到的回应数据.回应数据的开头sizeof(uint32_t)字节为回应数据长度
  uint32_t total_size = ntohl(*((uint32_t*) buf));

  // read the remaining data 回应数据除去sizeof(uint32_t)头部以外的数据
  uint32_t remaining_size = total_size - MessageCodec::kPacketLengthSize;
  if (remaining_size == 0 || remaining_size > kMaxPacketSize) {
    AGENT_LOG_ERROR("received malformed message, size field = %u", total_size);
    return PresenterErrorCode::kCodec;
  }

  int pack_size = static_cast<int>(remaining_size);
  unique_ptr<char[]> unique_buf; // ensure release allocated buffer
  //如果剩余数据超过kBufferSize(1K),则扩大接收buf
  if (remaining_size > kBufferSize) {
    buf = memutils::NewArray<char>(remaining_size);
    if (buf == nullptr) {
      return PresenterErrorCode::kBadAlloc;
    }

    unique_buf.reset(memutils::NewArray<char>(remaining_size));
  }

  // packSize must be within [1, MAX_PACKET_SIZE],
  // Recv() can not cause buffer overflow
  // 接收回应数据
  error_code = socket_->Recv(buf, pack_size);
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to read whole message");
    return PresenterErrorCode::kConnection;
  }

  // Decode message 将接收到的数据返序列化为Message对象
  Message* msg = codec_.DecodeMessage(buf, pack_size);
  if (msg == nullptr) {
    return PresenterErrorCode::kCodec;
  }
  //将回应数据体作为输出参数返回
  message.reset(msg);
  string name = message->GetDescriptor()->name();
  AGENT_LOG_DEBUG("Message received, name = %s", name.c_str());
  return PresenterErrorCode::kNone;
}

} /* namespace presenter */
} /* namespace ascend */
