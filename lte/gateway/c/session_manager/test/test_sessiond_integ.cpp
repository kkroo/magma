/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <iostream>
#include <string.h>
#include <chrono>
#include <thread>
#include <future>

#include <grpc++/grpc++.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "CloudReporter.h"
#include "MagmaService.h"
#include "ProtobufCreators.h"
#include "ServiceRegistrySingleton.h"
#include "SessionManagerServer.h"
#include "SessiondMocks.h"
#include "LocalEnforcer.h"

using grpc::Status;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::Test;

namespace magma {

class SessiondTest : public ::testing::Test {
 protected:
  virtual void SetUp()
  {
    auto test_channel = ServiceRegistrySingleton::Instance()->GetGrpcChannel(
      "test_service", ServiceRegistrySingleton::LOCAL);
    folly::EventBase *evb = folly::EventBaseManager::get()->getEventBase();

    controller_mock = std::make_shared<MockCentralController>();
    pipelined_mock = std::make_shared<MockPipelined>();

    pipelined_client = std::make_shared<AsyncPipelinedClient>(test_channel);
    auto rule_store = std::make_shared<StaticRuleStore>();
    insert_static_rule(rule_store, 1, "rule1");
    insert_static_rule(rule_store, 1, "rule2");
    insert_static_rule(rule_store, 2, "rule3");

    monitor = std::make_shared<LocalEnforcer>(rule_store, pipelined_client);
    reporter = std::make_shared<SessionCloudReporter>(evb, test_channel);

    local_service =
      std::make_shared<service303::MagmaService>("sessiond", "1.0");
    session_manager = std::make_shared<LocalSessionManagerAsyncService>(
      local_service->GetNewCompletionQueue(),
      std::make_unique<LocalSessionManagerHandlerImpl>(
        monitor.get(), reporter.get()));

    proxy_responder = std::make_shared<SessionProxyResponderAsyncService>(
      local_service->GetNewCompletionQueue(),
      std::make_unique<SessionProxyResponderHandlerImpl>(monitor.get()));

    local_service->AddServiceToServer(session_manager.get());
    local_service->AddServiceToServer(proxy_responder.get());

    test_service =
      std::make_shared<service303::MagmaService>("test_service", "1.0");
    test_service->AddServiceToServer(controller_mock.get());
    test_service->AddServiceToServer(pipelined_mock.get());

    local_service->Start();

    std::thread([&]() {
      std::cout << "Started cloud thread\n";
      test_service->Start();
      test_service->WaitForShutdown();
    }).detach();
    std::thread([&]() { pipelined_client->rpc_response_loop(); }).detach();
    std::thread([&]() {
      std::cout << "Started monitor thread\n";
      monitor->attachEventBase(evb);
      monitor->start();
    }).detach();
    std::thread([&]() {
      std::cout << "Started reporter thread\n";
      reporter->rpc_response_loop();
    }).detach();
    std::thread([&]() {
      std::cout << "Started local grpc thread\n";
      session_manager->wait_for_requests();
    }).detach();
    std::thread([&]() {
      std::cout << "Started local grpc thread\n";
      proxy_responder->wait_for_requests();
    }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  virtual void TearDown()
  {
    local_service->Stop();
    monitor->stop();
    reporter->stop();
    test_service->Stop();
    pipelined_client->stop();
  }

  void insert_static_rule(
    std::shared_ptr<StaticRuleStore> rule_store,
    uint32_t charging_key,
    const std::string &rule_id)
  {
    PolicyRule rule;
    rule.set_id(rule_id);
    rule.set_rating_group(charging_key);
    rule.set_tracking_type(PolicyRule::ONLY_OCS);
    rule_store->insert_rule(rule);
  }

  // Timeout to not block test
  void set_timeout(uint32_t ms, std::promise<void> *end_promise)
  {
    std::thread([&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      EXPECT_TRUE(false);
      end_promise->set_value();
    }).detach();
  }

 protected:
  std::shared_ptr<MockCentralController> controller_mock;
  std::shared_ptr<MockPipelined> pipelined_mock;
  std::shared_ptr<LocalEnforcer> monitor;
  std::shared_ptr<SessionCloudReporter> reporter;
  std::shared_ptr<LocalSessionManagerAsyncService> session_manager;
  std::shared_ptr<SessionProxyResponderAsyncService> proxy_responder;
  std::shared_ptr<service303::MagmaService> local_service;
  std::shared_ptr<service303::MagmaService> test_service;
  std::shared_ptr<AsyncPipelinedClient> pipelined_client;
};

MATCHER_P(CheckCreateSession, imsi, "")
{
  auto sid = static_cast<const CreateSessionRequest *>(arg);
  return sid->subscriber().id() == imsi;
}

MATCHER_P(CheckSingleUpdate, expected_update, "")
{
  auto request = static_cast<const UpdateSessionRequest *>(arg);
  if (request->updates_size() != 1) {
    return false;
  }

  auto &update = request->updates(0);
  bool val =
    update.usage().type() == expected_update.usage().type() &&
    update.usage().bytes_tx() == expected_update.usage().bytes_tx() &&
    update.usage().bytes_rx() == expected_update.usage().bytes_rx() &&
    update.sid() == expected_update.sid() &&
    update.usage().charging_key() == expected_update.usage().charging_key();
  return val;
}

MATCHER_P(CheckTerminate, imsi, "")
{
  auto request = static_cast<const SessionTerminateRequest *>(arg);
  return request->sid() == imsi;
}

MATCHER_P2(CheckActivateFlows, imsi, rule_count, "")
{
  auto request = static_cast<const ActivateFlowsRequest *>(arg);
  return request->sid().id() == imsi && request->rule_ids_size() == rule_count;
}

MATCHER_P(CheckDeactivateFlows, imsi, "")
{
  auto request = static_cast<const DeactivateFlowsRequest *>(arg);
  return request->sid().id() == imsi;
}

ACTION_P2(SetEndPromise, promise_p, status)
{
  promise_p->set_value();
  return status;
}

/**
 * End to end test.
 * 1) Create session, respond with 2 charging keys
 * 2) Report rule stats, charging key 1 goes over
 *    Expect update with charging key 1
 * 3) End Session for IMSI1
 * 4) Report rule stats without stats for IMSI1 (terminated)
 *    Expect update with terminated charging keys 1 and 2
 */
TEST_F(SessiondTest, end_to_end_success)
{
  std::promise<void> end_promise;
  {
    InSequence dummy;

    CreateSessionResponse create_response;
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule1");
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule2");
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule3");
    create_update_response(
      "IMSI1", 1, 1024, create_response.mutable_credits()->Add());
    create_update_response(
      "IMSI1", 2, 1024, create_response.mutable_credits()->Add());
    // Expect create session with IMSI1
    EXPECT_CALL(
      *controller_mock,
      CreateSession(testing::_, CheckCreateSession("IMSI1"), testing::_))
      .Times(1)
      .WillOnce(testing::DoAll(
        testing::SetArgPointee<2>(create_response),
        testing::Return(grpc::Status::OK)));

    EXPECT_CALL(
      *pipelined_mock,
      ActivateFlows(testing::_, CheckActivateFlows("IMSI1", 3), testing::_))
      .Times(1);

    CreditUsageUpdate expected_update;
    create_usage_update(
      "IMSI1", 1, 1024, 512, CreditUsage::QUOTA_EXHAUSTED, &expected_update);
    UpdateSessionResponse update_response;
    create_update_response(
      "IMSI1", 1, 1024, update_response.mutable_responses()->Add());
    // Expect update with IMSI1, charging key 1
    EXPECT_CALL(
      *controller_mock,
      UpdateSession(testing::_, CheckSingleUpdate(expected_update), testing::_))
      .Times(1)
      .WillOnce(testing::DoAll(
        testing::SetArgPointee<2>(update_response),
        testing::Return(grpc::Status::OK)));

    // Expect flows to be deactivated before final update is sent out
    EXPECT_CALL(
      *pipelined_mock,
      DeactivateFlows(testing::_, CheckDeactivateFlows("IMSI1"), testing::_))
      .Times(1);

    SessionTerminateResponse terminate_response;
    terminate_response.set_sid("IMSI1");

    EXPECT_CALL(
      *controller_mock,
      TerminateSession(testing::_, CheckTerminate("IMSI1"), testing::_))
      .Times(1)
      .WillOnce(testing::DoAll(
        testing::SetArgPointee<2>(terminate_response),
        SetEndPromise(&end_promise, Status::OK)));
  }

  auto channel = ServiceRegistrySingleton::Instance()->GetGrpcChannel(
    "sessiond", ServiceRegistrySingleton::LOCAL);
  auto stub = LocalSessionManager::NewStub(channel);

  grpc::ClientContext create_context;
  LocalCreateSessionResponse create_resp;
  LocalCreateSessionRequest request;
  request.mutable_sid()->set_id("IMSI1");
  stub->CreateSession(&create_context, request, &create_resp);

  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 512, 512, record_list->Add());
  create_rule_record("IMSI1", "rule2", 512, 0, record_list->Add());
  create_rule_record("IMSI1", "rule3", 32, 32, record_list->Add());
  grpc::ClientContext update_context;
  Void void_resp;
  stub->ReportRuleStats(&update_context, table, &void_resp);

  LocalEndSessionResponse update_resp;
  grpc::ClientContext end_context;
  SubscriberID end_request;
  end_request.set_id("IMSI1");
  stub->EndSession(&end_context, end_request, &update_resp);

  set_timeout(5000, &end_promise);
  end_promise.get_future().get();
}

/**
 * End to end test with cloud service intermittent.
 * 1) Create session, respond with 2 charging keys
 * 2) Report rule stats, charging key 1 goes over
 *    Expect update with charging key 1
 * 3) Cloud will respond with a timeout
 * 4) In next rule stats report, expect same update again, since last failed
 */
TEST_F(SessiondTest, end_to_end_cloud_down)
{
  std::promise<void> end_promise;
  {
    InSequence dummy;

    CreateSessionResponse create_response;
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule1");
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule2");
    create_response.mutable_static_rules()->Add()->mutable_rule_id()->assign(
      "rule3");
    create_update_response(
      "IMSI1", 1, 1024, create_response.mutable_credits()->Add());
    create_update_response(
      "IMSI1", 2, 1024, create_response.mutable_credits()->Add());
    // Expect create session with IMSI1
    EXPECT_CALL(
      *controller_mock,
      CreateSession(testing::_, CheckCreateSession("IMSI1"), testing::_))
      .Times(1)
      .WillOnce(testing::DoAll(
        testing::SetArgPointee<2>(create_response),
        testing::Return(grpc::Status::OK)));

    CreditUsageUpdate expected_update;
    create_usage_update(
      "IMSI1", 1, 1024, 512, CreditUsage::QUOTA_EXHAUSTED, &expected_update);
    // Expect update with IMSI1, charging key 1, return timeout from cloud
    EXPECT_CALL(
      *controller_mock,
      UpdateSession(testing::_, CheckSingleUpdate(expected_update), testing::_))
      .Times(1)
      .WillOnce(
        testing::Return(grpc::Status(grpc::DEADLINE_EXCEEDED, "timeout")));

    auto second_update = expected_update;
    second_update.mutable_usage()->set_bytes_rx(1048);
    // expect second update that's exactly the same but with an increased rx
    EXPECT_CALL(
      *controller_mock,
      UpdateSession(testing::_, CheckSingleUpdate(second_update), testing::_))
      .Times(1)
      .WillOnce(SetEndPromise(&end_promise, Status::OK));
  }

  auto channel = ServiceRegistrySingleton::Instance()->GetGrpcChannel(
    "sessiond", ServiceRegistrySingleton::LOCAL);
  auto stub = LocalSessionManager::NewStub(channel);

  grpc::ClientContext create_context;
  LocalCreateSessionResponse create_resp;
  LocalCreateSessionRequest request;
  request.mutable_sid()->set_id("IMSI1");
  stub->CreateSession(&create_context, request, &create_resp);

  RuleRecordTable table1;
  auto record_list = table1.mutable_records();
  create_rule_record("IMSI1", "rule1", 512, 512, record_list->Add());
  create_rule_record("IMSI1", "rule2", 512, 0, record_list->Add());
  grpc::ClientContext update_context1;
  Void void_resp;
  stub->ReportRuleStats(&update_context1, table1, &void_resp);

  // Need to wait for cloud response to come back and usage monitor to reset.
  // Unfortunately, there is no simple way to wait for response to come back
  // then callback to be called in event base
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  RuleRecordTable table2;
  record_list = table2.mutable_records();
  create_rule_record("IMSI1", "rule1", 24, 0, record_list->Add());
  create_rule_record("IMSI1", "rule2", 0, 0, record_list->Add());
  grpc::ClientContext update_context2;
  stub->ReportRuleStats(&update_context2, table2, &void_resp);

  set_timeout(5000, &end_promise);
  end_promise.get_future().get();
}

int main(int argc, char **argv)
{
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;
  FLAGS_v = 10;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

} // namespace magma
