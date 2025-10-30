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

#include "pw_async2/channel.h"

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/try.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::experimental::CreateDynamicChannel;
using pw::async2::experimental::DynamicChannel;
using pw::async2::experimental::Receiver;
using pw::async2::experimental::Sender;
using pw::async2::experimental::StaticChannel;

class SenderTask : public pw::async2::Task {
 public:
  SenderTask(Sender<int> sender, int start, int end)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        next_(start),
        end_(end) {}

  bool succeeded() const { return success_; }

 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    while (next_ <= end_) {
      if (!future_.has_value()) {
        future_.emplace(sender_.Send(next_));
      }

      PW_TRY_READY_ASSIGN(bool sent, future_->Pend(cx));
      if (!sent) {
        success_ = false;
        return pw::async2::Ready();
      }

      future_.reset();
      next_++;
    }

    success_ = true;
    sender_.Disconnect();
    return pw::async2::Ready();
  }

  Sender<int> sender_;
  std::optional<pw::async2::experimental::SendFuture<int>> future_;
  bool success_ = false;
  int next_;
  int end_;
};

class ReceiverTask : public pw::async2::Task {
 public:
  ReceiverTask(Receiver<int> receiver)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("ReceiverTask")),
        receiver_(std::move(receiver)) {}

  pw::Vector<int>& received() { return received_; }
  const pw::Vector<int>& received() const { return received_; }

 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    while (true) {
      if (!future_.has_value()) {
        future_.emplace(receiver_.Receive());
      }

      PW_TRY_READY_ASSIGN(std::optional<int> value, future_->Pend(cx));
      if (!value.has_value()) {
        break;
      }

      received_.push_back(*value);
      future_.reset();
    }

    receiver_.Disconnect();
    return pw::async2::Ready();
  }

  Receiver<int> receiver_;
  std::optional<pw::async2::experimental::ReceiveFuture<int>> future_;
  pw::Vector<int, 10> received_;
};

class DisconnectingReceiverTask : public pw::async2::Task {
 public:
  DisconnectingReceiverTask(Receiver<int> receiver, size_t disconnect_after)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("DisconnectingReceiverTask")),
        receiver_(std::move(receiver)),
        disconnect_after_(disconnect_after) {}

 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    while (disconnect_after_ > 0) {
      if (!future_.has_value()) {
        future_.emplace(receiver_.Receive());
      }

      PW_TRY_READY_ASSIGN(std::optional<int> value, future_->Pend(cx));
      if (!value.has_value()) {
        break;
      }
      future_.reset();
      disconnect_after_--;
    }

    receiver_.Disconnect();
    return pw::async2::Ready();
  }

  Receiver<int> receiver_;
  std::optional<pw::async2::experimental::ReceiveFuture<int>> future_;
  size_t disconnect_after_;
};

void ExpectReceived1To6(const pw::Vector<int>& received) {
  ASSERT_EQ(received.size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(received[i], static_cast<int>(i + 1));
  }
}

TEST(StaticChannel, SingleProducerSingleConsumer) {
  pw::async2::Dispatcher dispatcher;

  StaticChannel<int, 2> channel;
  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();

  SenderTask sender_task(std::move(sender), 1, 6);
  ReceiverTask receiver_task(std::move(receiver));

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());
  ExpectReceived1To6(receiver_task.received());
}

TEST(StaticChannel, MultiProducerSingleConsumer) {
  pw::async2::Dispatcher dispatcher;
  StaticChannel<int, 2> channel;

  SenderTask sender_task_1(channel.CreateSender(), 1, 3);
  SenderTask sender_task_2(channel.CreateSender(), 4, 6);
  ReceiverTask receiver_task(channel.CreateReceiver());

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  pw::Vector<int, 10> received(receiver_task.received());
  std::stable_partition(
      received.begin(), received.end(), [](int x) { return x < 4; });
  ExpectReceived1To6(received);
}

TEST(StaticChannel, SingleProducerMultiConsumer) {
  pw::async2::Dispatcher dispatcher;
  StaticChannel<int, 2> channel;

  SenderTask sender_task(channel.CreateSender(), 1, 6);
  ReceiverTask receiver_task_1(channel.CreateReceiver());
  ReceiverTask receiver_task_2(channel.CreateReceiver());

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());

  pw::Vector<int, 10> all_received;
  const auto& received_1 = receiver_task_1.received();
  const auto& received_2 = receiver_task_2.received();
  EXPECT_TRUE(std::is_sorted(received_1.begin(), received_1.end()));
  EXPECT_TRUE(std::is_sorted(received_2.begin(), received_2.end()));
  std::merge(received_1.begin(),
             received_1.end(),
             received_2.begin(),
             received_2.end(),
             std::back_inserter(all_received));
  ExpectReceived1To6(all_received);
}

TEST(StaticChannel, MultiProducerMultiConsumer) {
  pw::async2::Dispatcher dispatcher;
  StaticChannel<int, 2> channel;

  SenderTask sender_task_1(channel.CreateSender(), 1, 3);
  SenderTask sender_task_2(channel.CreateSender(), 4, 6);
  ReceiverTask receiver_task_1(channel.CreateReceiver());
  ReceiverTask receiver_task_2(channel.CreateReceiver());

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  pw::Vector<int, 10> all_received;
  auto& received_1 = receiver_task_1.received();
  auto& received_2 = receiver_task_2.received();
  std::stable_partition(
      received_1.begin(), received_1.end(), [](int x) { return x < 4; });
  std::stable_partition(
      received_2.begin(), received_2.end(), [](int x) { return x < 4; });
  EXPECT_TRUE(std::is_sorted(received_1.begin(), received_1.end()));
  EXPECT_TRUE(std::is_sorted(received_2.begin(), received_2.end()));
  std::merge(received_1.begin(),
             received_1.end(),
             received_2.begin(),
             received_2.end(),
             std::back_inserter(all_received));
  ExpectReceived1To6(all_received);
}

TEST(StaticChannel, NonAsyncTrySend) {
  pw::async2::Dispatcher dispatcher;
  StaticChannel<int, 2> channel;
  Sender<int> sender = channel.CreateSender();

  ReceiverTask receiver_task(channel.CreateReceiver());
  dispatcher.Post(receiver_task);

  EXPECT_TRUE(sender.TrySend(1));
  EXPECT_TRUE(sender.TrySend(2));
  EXPECT_FALSE(sender.TrySend(3));
  EXPECT_EQ(dispatcher.RunUntilStalled(), pw::async2::Pending());

  EXPECT_TRUE(sender.TrySend(3));
  EXPECT_TRUE(sender.TrySend(4));
  EXPECT_FALSE(sender.TrySend(5));
  EXPECT_EQ(dispatcher.RunUntilStalled(), pw::async2::Pending());

  EXPECT_TRUE(sender.TrySend(5));
  EXPECT_TRUE(sender.TrySend(6));
  sender.Disconnect();
  EXPECT_EQ(dispatcher.RunUntilStalled(), pw::async2::Ready());

  ExpectReceived1To6(receiver_task.received());
}

TEST(StaticChannel, ReceiverDisconnects) {
  pw::async2::Dispatcher dispatcher;
  StaticChannel<int, 2> channel;

  SenderTask sender_task_1(channel.CreateSender(), 1, 10);
  SenderTask sender_task_2(channel.CreateSender(), 11, 20);
  DisconnectingReceiverTask receiver_task(channel.CreateReceiver(), 3);

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_FALSE(sender_task_1.succeeded());
  EXPECT_FALSE(sender_task_2.succeeded());
}

TEST(StaticChannel, RemainingCapacity) {
  StaticChannel<int, 2> channel;

  Sender<int> sender = channel.CreateSender();
  EXPECT_EQ(sender.remaining_capacity(), 2u);
  EXPECT_EQ(sender.capacity(), 2u);

  EXPECT_TRUE(sender.TrySend(1));
  EXPECT_EQ(sender.remaining_capacity(), 1u);
  EXPECT_EQ(sender.capacity(), 2u);
}

TEST(DynamicChannel, ForwardsDataAndAutomaticallyDeallocates) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::Dispatcher dispatcher;

  std::optional<DynamicChannel<int>> channel =
      CreateDynamicChannel<int>(alloc, 2);
  ASSERT_TRUE(channel.has_value());

  Sender<int> sender = channel->CreateSender();
  Receiver<int> receiver = channel->CreateReceiver();
  EXPECT_EQ(alloc.metrics().num_allocations.value(), 2u);
  EXPECT_EQ(alloc.metrics().num_deallocations.value(), 0u);

  // Drop the handle, which should not do anything.
  std::destroy_at(&channel);
  EXPECT_EQ(alloc.metrics().num_allocations.value(), 2u);
  EXPECT_EQ(alloc.metrics().num_deallocations.value(), 0u);

  SenderTask sender_task(std::move(sender), 1, 6);
  ReceiverTask receiver_task(std::move(receiver));

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());
  ExpectReceived1To6(receiver_task.received());

  EXPECT_EQ(alloc.metrics().allocated_bytes.value(), 0u);
  EXPECT_EQ(alloc.metrics().num_allocations.value(), 2u);
  EXPECT_EQ(alloc.metrics().num_deallocations.value(), 2u);
}

TEST(DynamicChannel, RemainingCapacity) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  std::optional<DynamicChannel<int>> channel =
      CreateDynamicChannel<int>(alloc, 2);
  ASSERT_TRUE(channel.has_value());

  Sender<int> sender = channel->CreateSender();
  EXPECT_EQ(sender.remaining_capacity(), 2u);
  EXPECT_EQ(sender.capacity(), 2u);

  EXPECT_TRUE(sender.TrySend(1));
  EXPECT_EQ(sender.remaining_capacity(), 1u);
  EXPECT_EQ(sender.capacity(), 2u);
}

TEST(DynamicChannel, AllocationFailure) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  alloc.Exhaust();
  std::optional<DynamicChannel<int>> channel =
      CreateDynamicChannel<int>(alloc, 2);
  ASSERT_FALSE(channel.has_value());
}

}  // namespace
