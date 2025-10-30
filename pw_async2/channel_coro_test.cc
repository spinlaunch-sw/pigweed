// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_allocator/testing.h"
#include "pw_async2/channel.h"
#include "pw_async2/coro.h"
#include "pw_async2/coro_or_else_task.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/try.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::Coro;
using pw::async2::CoroContext;
using pw::async2::CoroOrElseTask;
using pw::async2::experimental::CreateDynamicChannel;
using pw::async2::experimental::Receiver;
using pw::async2::experimental::Sender;
using pw::async2::experimental::StaticChannel;

Coro<pw::Status> Producer(CoroContext&,
                          Sender<int> sender,
                          int start,
                          int end) {
  for (int i = start; i <= end; ++i) {
    if (!co_await sender.Send(i)) {
      co_return pw::Status::Cancelled();
    }
  }
  co_return pw::OkStatus();
}

Coro<pw::Status> Consumer(CoroContext&,
                          Receiver<int> receiver,
                          pw::Vector<int>& out) {
  while (true) {
    std::optional<int> value = co_await receiver.Receive();
    if (!value.has_value()) {
      break;
    }
    out.push_back(*value);
  }
  co_return pw::OkStatus();
}

Coro<pw::Status> DisconnectingConsumer(CoroContext&,
                                       Receiver<int> receiver,
                                       size_t disconnect_after) {
  for (size_t i = 0; i < disconnect_after; ++i) {
    std::optional<int> value = co_await receiver.Receive();
    if (!value.has_value()) {
      break;
    }
  }
  receiver.Disconnect();
  co_return pw::OkStatus();
}

TEST(DynamicChannel, SingleProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::Dispatcher dispatcher;

  CoroContext coro_cx(alloc);

  auto channel = CreateDynamicChannel<int>(alloc, 3);
  ASSERT_TRUE(channel.has_value());

  pw::Vector<int, 10> out;

  auto producer = CoroOrElseTask(
      Producer(coro_cx, channel->CreateSender(), 1, 6), [](pw::Status) {});
  auto consumer = CoroOrElseTask(
      Consumer(coro_cx, channel->CreateReceiver(), out), [](pw::Status) {});

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
  EXPECT_EQ(out[3], 4);
  EXPECT_EQ(out[4], 5);
  EXPECT_EQ(out[5], 6);
}

TEST(DynamicChannel, MultiProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::Dispatcher dispatcher;

  CoroContext coro_cx(alloc);

  auto channel = CreateDynamicChannel<int>(alloc, 3);
  ASSERT_TRUE(channel.has_value());

  pw::Vector<int, 10> out;

  auto producer_1 = CoroOrElseTask(
      Producer(coro_cx, channel->CreateSender(), 1, 3), [](pw::Status) {});
  auto producer_2 = CoroOrElseTask(
      Producer(coro_cx, channel->CreateSender(), 4, 6), [](pw::Status) {});
  auto consumer = CoroOrElseTask(
      Consumer(coro_cx, channel->CreateReceiver(), out), [](pw::Status) {});

  dispatcher.Post(producer_1);
  dispatcher.Post(producer_2);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  std::stable_partition(out.begin(), out.end(), [](int x) { return x < 4; });
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(out[i], static_cast<int>(i + 1));
  }
}

TEST(DynamicChannel, ReceiverDisconnects) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::Dispatcher dispatcher;

  CoroContext coro_cx(alloc);

  auto channel = CreateDynamicChannel<int>(alloc, 3);
  ASSERT_TRUE(channel.has_value());

  pw::Status producer_status;
  auto producer =
      CoroOrElseTask(Producer(coro_cx, channel->CreateSender(), 1, 10),
                     [&](pw::Status status) { producer_status = status; });
  auto consumer = CoroOrElseTask(
      DisconnectingConsumer(coro_cx, channel->CreateReceiver(), 3),
      [](pw::Status) {});

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  EXPECT_EQ(producer_status, pw::Status::Cancelled());
}

TEST(StaticChannel, SingleProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::Dispatcher dispatcher;

  CoroContext coro_cx(alloc);

  StaticChannel<int, 3> channel;
  pw::Vector<int, 10> out;

  auto producer = CoroOrElseTask(
      Producer(coro_cx, channel.CreateSender(), 1, 6), [](pw::Status) {});
  auto consumer = CoroOrElseTask(
      Consumer(coro_cx, channel.CreateReceiver(), out), [](pw::Status) {});

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
  EXPECT_EQ(out[3], 4);
  EXPECT_EQ(out[4], 5);
  EXPECT_EQ(out[5], 6);
}

}  // namespace
