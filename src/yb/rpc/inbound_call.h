// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#ifndef YB_RPC_INBOUND_CALL_H_
#define YB_RPC_INBOUND_CALL_H_

#include <string>
#include <vector>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/rpc/connection_types.h"
#include "yb/rpc/remote_method.h"
#include "yb/rpc/rpc_call.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/transfer.h"
#include "yb/sql/sql_session.h"
#include "yb/util/faststring.h"
#include "yb/util/monotime.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

namespace yb {

class Histogram;
class Trace;

namespace rpc {

class Connection;
class DumpRunningRpcsRequestPB;
class RpcCallInProgressPB;
class RpcSidecar;
class UserCredentials;

struct InboundCallTiming {
  MonoTime time_received;   // Time the call was first accepted.
  MonoTime time_handled;    // Time the call handler was kicked off.
  MonoTime time_completed;  // Time the call handler completed.
};

// Inbound call on server
class InboundCall : public RpcCall {
 public:
  InboundCall();
  virtual ~InboundCall();

  // Parse an inbound call message.
  //
  // This only deserializes the call header, populating the 'header_' and
  // 'serialized_request_' member variables. The actual call parameter is
  // not deserialized, as this may be CPU-expensive, and this is called
  // from the reactor thread.
  virtual CHECKED_STATUS ParseFrom(gscoped_ptr<AbstractInboundTransfer> transfer) = 0;

  // Return the serialized request parameter protobuf.
  const Slice &serialized_request() const {
    return serialized_request_;
  }

  const RemoteMethod& remote_method() const {
    return remote_method_;
  }

  // Serializes 'response' into the InboundCall's internal buffer, and marks
  // the call as a success. Enqueues the response back to the connection
  // that made the call.
  //
  // This method deletes the InboundCall object, so no further calls may be
  // made after this one.
  void RespondSuccess(const google::protobuf::MessageLite& response);

  // Serializes a failure response into the internal buffer, marking the
  // call as a failure. Enqueues the response back to the connection that
  // made the call.
  //
  // This method deletes the InboundCall object, so no further calls may be
  // made after this one.
  void RespondFailure(ErrorStatusPB::RpcErrorCodePB error_code,
                      const Status &status);

  void RespondApplicationError(int error_ext_id, const std::string& message,
                               const google::protobuf::MessageLite& app_error_pb);

  // Serialize the response packet for the finished call.
  // The resulting slices refer to memory in this object.
  virtual void SerializeResponseTo(std::vector<Slice>* slices) const = 0;

  // Convert an application error extension to an ErrorStatusPB.
  // These ErrorStatusPB objects are what are returned in application error responses.
  static void ApplicationErrorToPB(int error_ext_id, const std::string& message,
                                   const google::protobuf::MessageLite& app_error_pb,
                                   ErrorStatusPB* err);

  // See RpcContext::AddRpcSidecar()
  CHECKED_STATUS AddRpcSidecar(gscoped_ptr<RpcSidecar> car, int* idx);

  virtual std::string ToString() const = 0;

  virtual void DumpPB(const DumpRunningRpcsRequestPB& req, RpcCallInProgressPB* resp) = 0;

  virtual const UserCredentials& user_credentials() const;

  const Sockaddr& remote_address() const;

  const scoped_refptr<Connection> connection() const;

  Trace* trace();

  // When this InboundCall was received (instantiated).
  // Should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  void RecordCallReceived();

  // When RPC call Handle() was called on the server side.
  // Updates the Histogram with time elapsed since the call was received,
  // and should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  virtual void RecordHandlingStarted(scoped_refptr<Histogram> incoming_queue_time);

  // When RPC call Handle() completed execution on the server side.
  // Updates the Histogram with time elapsed since the call was started,
  // and should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  void RecordHandlingCompleted(scoped_refptr<Histogram> handler_run_time);

  // Return true if the deadline set by the client has already elapsed.
  // In this case, the server may stop processing the call, since the
  // call response will be ignored anyway.
  bool ClientTimedOut() const;

  // Return an upper bound on the client timeout deadline. This does not
  // account for transmission delays between the client and the server.
  // If the client did not specify a deadline, returns MonoTime::Max().
  virtual MonoTime GetClientDeadline() const = 0;
 protected:
  // Serialize and queue the response.
  void Respond(const google::protobuf::MessageLite& response,
               bool is_success);

  // Returns a ptr to the Connection for use.
  virtual scoped_refptr<Connection> get_connection() = 0;
  virtual const scoped_refptr<Connection> get_connection() const = 0;

  // Queues the response to the Connection implementation.
  virtual void QueueResponseToConnection() = 0;

  // Serialize a response message for either success or failure. If it is a success,
  // 'response' should be the user-defined response type for the call. If it is a
  // failure, 'response' should be an ErrorStatusPB instance.
  virtual CHECKED_STATUS SerializeResponseBuffer(const google::protobuf::MessageLite& response,
                                         bool is_success) = 0;

  // Log a WARNING message if the RPC response was slow enough that the
  // client likely timed out. This is based on the client-provided timeout
  // value.
  // Also can be configured to log _all_ RPC traces for help debugging.
  virtual void LogTrace() const = 0;

  // The serialized bytes of the request param protobuf. Set by ParseFrom().
  // This references memory held by 'transfer_'.
  Slice serialized_request_;

  // The transfer that produced the call.
  // This is kept around because it retains the memory referred to
  // by 'serialized_request_' above.
  gscoped_ptr<AbstractInboundTransfer> transfer_;

  // Vector of additional sidecars that are tacked on to the call's response
  // after serialization of the protobuf. See rpc/rpc_sidecar.h for more info.
  std::vector<RpcSidecar*> sidecars_;
  ElementDeleter sidecars_deleter_;

  // The trace buffer.
  scoped_refptr<Trace> trace_;

  // Timing information related to this RPC call.
  InboundCallTiming timing_;

  // Proto service this calls belongs to. Used for routing.
  // This field is filled in when the inbound request header is parsed.
  RemoteMethod remote_method_;

 private:
  DISALLOW_COPY_AND_ASSIGN(InboundCall);
};

typedef scoped_refptr<InboundCall> InboundCallPtr;

}  // namespace rpc
}  // namespace yb

#endif  // YB_RPC_INBOUND_CALL_H_
