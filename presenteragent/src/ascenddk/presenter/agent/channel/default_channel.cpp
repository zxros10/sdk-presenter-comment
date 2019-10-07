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

#include <chrono>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <sstream>
#include <cstddef>

#include "proto/presenter_message.pb.h"

#include "ascenddk/presenter/agent/channel/default_channel.h"
#include "ascenddk/presenter/agent/net/raw_socket_factory.h"
#include "ascenddk/presenter/agent/util/logging.h"

using namespace std;
using namespace google::protobuf;

namespace {
const int HEARTBEAT_INTERVAL = 1500;  // 1.5s
}

namespace ascend {
namespace presenter {

//新建一条通道
DefaultChannel* DefaultChannel::NewChannel(
    const std::string& host_ip, uint16_t port,
    std::shared_ptr<InitChannelHandler> handler) {
  DefaultChannel *channel = nullptr;
  //创建RawSocketFactory实例(仅仅只是保存了ip和port)，实例指针赋值对fac.
  //RawSocketFactory是SocketFactory的子类,所以这个地方可以使用SocketFactory类型的指针指向RawSocketFactory实例
  std::shared_ptr<SocketFactory> fac(
      new (std::nothrow) RawSocketFactory(host_ip, port));
  if (fac != nullptr) {
	//如果创建RawSocketFactory成功,则使用该RawSocketFactory创建一个通道对象,
	//RawSocketFactory实例将赋给DefaultChannel的socket_factory_成员以创建socket
    channel = new (std::nothrow) DefaultChannel(fac);
    if (channel != nullptr && handler != nullptr) {
	  //将一个InitChannelHandler实例挂载到channel的init_channel_handler_指针.在channel调用Open成员函数
	  //打开通道时,通过InitChannelHandler的CreateInitRequest成员函数创建一个初始化请求,发给presenterserver；
	  //在收到presenterserver对初始化请求的回应后，调用InitChannelHandler的CheckInitResponse成员函数处理回应
      channel->SetInitChannelHandler(handler);
    }
  }

  return channel;
}

DefaultChannel::DefaultChannel(std::shared_ptr<SocketFactory> socket_factory)
    : socket_factory_(socket_factory),
      open_(false),
      disposed_(false) {
}

DefaultChannel::~DefaultChannel() {
  disposed_ = true;
  if (heartbeat_thread_ != nullptr) {
    heartbeat_thread_->join();
  }
}

void DefaultChannel::SetInitChannelHandler(
    std::shared_ptr<InitChannelHandler> handler) {
  init_channel_handler_ = handler;
}

shared_ptr<const InitChannelHandler> DefaultChannel::GetInitChannelHandler() {
  return init_channel_handler_;
}

PresenterErrorCode DefaultChannel::HandleInitialization(
    const Message& message) {
  // send init request
  PresenterErrorCode error_code = conn_->SendMessage(message);
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to send init request, %d", error_code);
    return error_code;
  }

  // receive init response
  unique_ptr<Message> resp;
  error_code = conn_->ReceiveMessage(resp);
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to send init response, %d", error_code);
    return error_code;
  }

  // check response
  if (!init_channel_handler_->CheckInitResponse(*resp)) {
    AGENT_LOG_ERROR("App check response failed");
    return PresenterErrorCode::kAppDefinedError;
  }

  return PresenterErrorCode::kNone;
}

PresenterErrorCode DefaultChannel::Open() {
  //check request generation before connection
  unique_ptr<Message> message;
  if (init_channel_handler_ != nullptr) {
	//生成一条初始化请求消息赋给message
    message.reset(init_channel_handler_->CreateInitRequest());
    if (message == nullptr) {
      AGENT_LOG_ERROR("App create init request failed");
      return PresenterErrorCode::kAppDefinedError;
    }
  }
  //创建socket,并连接presenter server.这里Create()返回的是一个RawSocket指针, RawSocket是Socket的子类
  Socket* sock = socket_factory_->Create();
  //获取创建socket的错误码
  PresenterErrorCode error_code = socket_factory_->GetErrorCode();
  //如果错误码显示创建异常则创建失败
  if (error_code != PresenterErrorCode::kNone) {
    AGENT_LOG_ERROR("Failed to create socket, %d", error_code);
    return error_code;
  }
  //创建一个连接（Connection）实例.Connection的构造函数只是保存了sock,无其他操作
  Connection* conn = Connection::New(sock);
  if (conn == nullptr) {
    delete sock;
    return PresenterErrorCode::kBadAlloc;
  }
  //将创建的连接实例赋值给this->conn_
  this->conn_.reset(conn);

  //perform init process
  if (message != nullptr) {
	//将初始化请求消息通过前面创建的socket发给presenter server,等待回应并处理
    error_code = HandleInitialization(*message);
    if (error_code != PresenterErrorCode::kNone) {
	  //初始化请求失败
      conn_.reset(nullptr);
      return error_code;
    }
  }
  //设置open标记表示当前通道打开成功
  open_ = true;
  // prevent from starting multiple thread
  if (heartbeat_thread_ == nullptr) {
	//创建心跳保活线程监测agent和server之间的连接情况,并防止因长时间无数据收发而触发的socket连接自动中断
    StartHeartbeatThread();
  }

  return PresenterErrorCode::kNone;
}

void DefaultChannel::StartHeartbeatThread() {
  this->heartbeat_thread_.reset(
	  //创建一个thread实例.这里传递了一个bind实例参数.如果实例被调用,等同于执行DefaultChannel::KeepAlive（this）
      new (nothrow) thread(bind(&DefaultChannel::KeepAlive, this)));

  if (heartbeat_thread_ != nullptr) {
    AGENT_LOG_INFO("heartbeat thread started");
  }
}

void DefaultChannel::KeepAlive() {
  chrono::milliseconds heartbeatInterval(HEARTBEAT_INTERVAL);
  while (!disposed_) {
    SendHeartbeat();

    // interruptable wait
    unique_lock<mutex> lock(mtx_);
    cv_shutdown_.wait_for(lock, heartbeatInterval,
                         [this]() {return disposed_.load();});
  }

  AGENT_LOG_DEBUG("heartbeat thread ended");
}

void DefaultChannel::SendHeartbeat() {
  // reopen channel if disconnected
  if (!open_) {
    if (Open() != PresenterErrorCode::kNone) {
      return;
    }
  }

  // construct a heartbeat message then send it
  proto::HeartbeatMessage heartbeat_msg;
  SendMessage(heartbeat_msg);
}

PresenterErrorCode DefaultChannel::SendMessage(const Message& message) {
  PartialMessageWithTlvs msg;
  string msg_name = message.GetDescriptor()->full_name();
  AGENT_LOG_DEBUG("To send message: %s", msg_name.c_str());
  msg.message = &message;
  return SendMessage(msg);
}

PresenterErrorCode DefaultChannel::SendMessage(
    const PartialMessageWithTlvs& message) {
  //判断通道是否open,这个标记在创建通道调用Open函数的时候设置的
  if (!open_) {
    AGENT_LOG_ERROR("Channel is not open, send message failed");
    return PresenterErrorCode::kConnection;
  }

  PresenterErrorCode errorCode = PresenterErrorCode::kOther;
  try {
	//调用PresenterErrorCode Connection::SendMessage发送消息.conn_在创建通道并Open成功后创建的,保存有agent和server之间的tcp socket
    errorCode = conn_->SendMessage(message);
    //connect error, set is_open to false, enable retry
    if (errorCode == PresenterErrorCode::kConnection) {
      open_ = false;
    }
  } catch (std::exception &e) {  // protobuf may throw FatalException
    AGENT_LOG_ERROR("Protobuf error: %s", e.what());
    open_ = false;
  }

  return errorCode;
}

PresenterErrorCode DefaultChannel::ReceiveMessage(
    unique_ptr<Message>& message) {
  AGENT_LOG_DEBUG("To receive message");
  //通道必须为打开状态
  if (!open_) {
    AGENT_LOG_ERROR("Channel is not open, receive message failed");
    return PresenterErrorCode::kConnection;
  }

  PresenterErrorCode error_code = PresenterErrorCode::kOther;
  try {
	//调用Connection类的ReceiveMessage函数接收消息
    error_code = conn_->ReceiveMessage(message);
    // connect error and codec error, set is_open to false, enable retry
    if (error_code == PresenterErrorCode::kConnection
        || error_code == PresenterErrorCode::kCodec) {
      open_ = false;
    }

  } catch (std::exception &e) {  // protobuf may throw FatalException
    AGENT_LOG_ERROR("Protobuf error: %s", e.what());
    open_ = false;
  }

  return error_code;
}

PresenterErrorCode DefaultChannel::SendMessage(
    const google::protobuf::Message& message,
    std::unique_ptr<google::protobuf::Message> &response) {
  string msg_name = message.GetDescriptor()->full_name();
  AGENT_LOG_DEBUG("To send message: %s", msg_name.c_str());
  PresenterErrorCode error_code = SendMessage(message);
  if (error_code == PresenterErrorCode::kNone) {
    error_code = ReceiveMessage(response);
  }

  return error_code;
}

PresenterErrorCode DefaultChannel::SendMessage(
    const PartialMessageWithTlvs& message,
    std::unique_ptr<google::protobuf::Message> &response) {
  //获取（proto文件中定义的）原始发送数据的类型名称.PartialMessageWithTlvs的打包层次:
  //APP数据-->proto定义的数据格式-->PartialMessageWithTlvs的tlv.在proto格式打包为PartialMessageWithTlvs的时候,proto数据的引用赋给了message成员
  string msg_name = message.message->GetDescriptor()->full_name();
  AGENT_LOG_DEBUG("To send message: %s", msg_name.c_str());
  //发送PartialMessageWithTlvs数据给server端：PresenterErrorCode DefaultChannel::SendMessage(const PartialMessageWithTlvs& message)
  PresenterErrorCode error_code = SendMessage(message);
  if (error_code == PresenterErrorCode::kNone) {
	//如果数据发送成功,则等待server给出回应
    error_code = ReceiveMessage(response);
  }

  return error_code;
}

const std::string& DefaultChannel::GetDescription() const {
  return this->description_;
}

void DefaultChannel::SetDescription(const std::string& desc) {
  this->description_ = desc;
}

}
/* namespace presenter */
} /* namespace ascend */
