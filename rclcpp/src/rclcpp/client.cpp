// Copyright 2015 Open Source Robotics Foundation, Inc.
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

#include "rclcpp/client.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "rcutils/logging_macros.h"

#include "rcl/graph.h"
#include "rcl/node.h"
#include "rcl/wait.h"
#include "rclcpp/exceptions.hpp"
#include "rclcpp/node_interfaces/node_base_interface.hpp"
#include "rclcpp/node_interfaces/node_graph_interface.hpp"
#include "rclcpp/utilities.hpp"

using rclcpp::ClientBase;
using rclcpp::exceptions::InvalidNodeError;
using rclcpp::exceptions::throw_from_rcl_error;

ClientBase::ClientBase(
  rclcpp::node_interfaces::NodeBaseInterface * node_base,
  rclcpp::node_interfaces::NodeGraphInterface::SharedPtr node_graph,
  const std::string & service_name)
: node_graph_(node_graph),
  node_handle_(node_base->get_shared_rcl_node_handle()),
  service_name_(service_name)
{
  client_handle_ = std::shared_ptr<rcl_client_t>(
    new rcl_client_t, [ = ](rcl_client_t * client)
    {
      if (rcl_client_fini(client, node_handle_.get()) != RCL_RET_OK) {
        RCUTILS_LOG_ERROR_NAMED(
          "rclcpp",
          "Error in destruction of rcl client handle: %s", rcl_get_error_string_safe());
        rcl_reset_error();
        delete client;
      }
    });
  *client_handle_.get() = rcl_get_zero_initialized_client();
}

ClientBase::~ClientBase() {}

const std::string &
ClientBase::get_service_name() const
{
  return this->service_name_;
}

std::shared_ptr<rcl_client_t>
ClientBase::get_client_handle()
{
  return client_handle_;
}

std::shared_ptr<const rcl_client_t>
ClientBase::get_client_handle() const
{
  return client_handle_;
}

bool
ClientBase::service_is_ready() const
{
  bool is_ready;
  rcl_ret_t ret =
    rcl_service_server_is_available(this->get_rcl_node_handle(), client_handle_.get(), &is_ready);
  if (ret != RCL_RET_OK) {
    throw_from_rcl_error(ret, "rcl_service_server_is_available failed");
  }
  return is_ready;
}

bool
ClientBase::wait_for_service_nanoseconds(std::chrono::nanoseconds timeout)
{
  auto start = std::chrono::steady_clock::now();
  // make an event to reuse, rather than create a new one each time
  auto node_ptr = node_graph_.lock();
  if (!node_ptr) {
    throw InvalidNodeError();
  }
  auto event = node_ptr->get_graph_event();
  // check to see if the server is ready immediately
  if (this->service_is_ready()) {
    return true;
  }
  if (timeout == std::chrono::nanoseconds(0)) {
    // check was non-blocking, return immediately
    return false;
  }
  // update the time even on the first loop to account for time spent in the first call
  // to this->server_is_ready()
  std::chrono::nanoseconds time_to_wait = timeout - (std::chrono::steady_clock::now() - start);
  if (timeout > std::chrono::nanoseconds(0) && time_to_wait < std::chrono::nanoseconds(0)) {
    // Do not allow the time_to_wait to become negative when timeout was originally positive.
    // Setting time_to_wait to 0 will allow one non-blocking wait because of the do-while.
    time_to_wait = std::chrono::nanoseconds(0);
  }
  // continue forever if timeout is negative, otherwise continue until out of time_to_wait
  do {
    if (!rclcpp::ok()) {
      return false;
    }
    node_ptr->wait_for_graph_change(event, time_to_wait);
    event->check_and_clear();  // reset the event

    // always check if the service is ready, even if the graph event wasn't triggered
    // this is needed to avoid a race condition that is specific to the Connext RMW implementation
    // (see https://github.com/ros2/rmw_connext/issues/201)
    if (this->service_is_ready()) {
      return true;
    }
    // server is not ready, loop if there is time left
    time_to_wait = timeout - (std::chrono::steady_clock::now() - start);
  } while (time_to_wait > std::chrono::nanoseconds(0) || timeout < std::chrono::nanoseconds(0));
  return false;  // timeout exceeded while waiting for the server to be ready
}

rcl_node_t *
ClientBase::get_rcl_node_handle()
{
  return node_handle_.get();
}

const rcl_node_t *
ClientBase::get_rcl_node_handle() const
{
  return node_handle_.get();
}
