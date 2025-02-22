// Copyright 2022 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include "communication/bolt/v1/codes.hpp"
#include "communication/bolt/v1/encoder/base_encoder.hpp"

namespace memgraph::communication::bolt {

/**
 * Bolt Client Encoder.
 * Has public interfaces for writing Bolt specific request messages.
 * Supported messages are: Init, Run, DiscardAll, PullAll, AckFailure, Reset
 *
 * @tparam Buffer the output buffer that should be used
 */
template <typename Buffer>
class ClientEncoder : private BaseEncoder<Buffer> {
 private:
  using BaseEncoder<Buffer>::WriteRAW;
  using BaseEncoder<Buffer>::WriteList;
  using BaseEncoder<Buffer>::WriteMap;
  using BaseEncoder<Buffer>::WriteString;
  using BaseEncoder<Buffer>::buffer_;

 public:
  ClientEncoder(Buffer &buffer) : BaseEncoder<Buffer>(buffer) {}

  /**
   * Writes a Init message.
   *
   * From the Bolt v1 documentation:
   *   InitMessage (signature=0x01) {
   *     String clientName
   *     Map<String,Value> authToken
   *   }
   *
   * @param client_name the name of the connected client
   * @param auth_token a map with authentication data
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessageInit(const std::string client_name, const std::map<std::string, Value> &auth_token) {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct2));
    WriteRAW(utils::UnderlyingCast(Signature::Init));
    WriteString(client_name);
    WriteMap(auth_token);
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done.
    return buffer_.Flush();
  }

  /**
   * Writes a Run message.
   *
   * From the Bolt v1 documentation:
   *   RunMessage (signature=0x10) {
   *     String             statement
   *     Map<String,Value>  parameters
   *   }
   *
   * @param statement the statement that should be executed
   * @param parameters parameters that should be used to execute the statement
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessageRun(const std::string &statement, const std::map<std::string, Value> &parameters, bool have_more = true) {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct2));
    WriteRAW(utils::UnderlyingCast(Signature::Run));
    WriteString(statement);
    WriteMap(parameters);
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done. Here we
    // forward the `have_more` flag to indicate if there is more data that will
    // be sent.
    return buffer_.Flush(have_more);
  }

  /**
   * Writes a DiscardAll message.
   *
   * From the Bolt v1 documentation:
   *   DiscardMessage (signature=0x2F) {
   *   }
   *
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessageDiscardAll() {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct));
    WriteRAW(utils::UnderlyingCast(Signature::Discard));
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done.
    return buffer_.Flush();
  }

  /**
   * Writes a PullAll message.
   *
   * From the Bolt v1 documentation:
   *   PullAllMessage (signature=0x3F) {
   *   }
   *
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessagePullAll() {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct));
    WriteRAW(utils::UnderlyingCast(Signature::Pull));
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done.
    return buffer_.Flush();
  }

  /**
   * Writes a AckFailure message.
   *
   * From the Bolt v1 documentation:
   *   AckFailureMessage (signature=0x0E) {
   *   }
   *
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessageAckFailure() {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct));
    WriteRAW(utils::UnderlyingCast(Signature::AckFailure));
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done.
    return buffer_.Flush();
  }

  /**
   * Writes a Reset message.
   *
   * From the Bolt v1 documentation:
   *   ResetMessage (signature=0x0F) {
   *   }
   *
   * @returns true if the data was successfully sent to the client
   *          when flushing, false otherwise
   */
  bool MessageReset() {
    WriteRAW(utils::UnderlyingCast(Marker::TinyStruct));
    WriteRAW(utils::UnderlyingCast(Signature::Reset));
    // Try to flush all remaining data in the buffer, but tell it that we will
    // send more data (the end of message chunk).
    if (!buffer_.Flush(true)) return false;
    // Flush an empty chunk to indicate that the message is done.
    return buffer_.Flush();
  }
};
}  // namespace memgraph::communication::bolt
