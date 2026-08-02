// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/elasticsearch/es_log_record_exporter.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/exporters/elasticsearch/es_log_recordable.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/logs/severity.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/utility.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/logs/recordable.h"
#include "opentelemetry/sdk/resource/resource.h"

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "nlohmann/json.hpp"

namespace sdklogs       = opentelemetry::sdk::logs;
namespace logs_api      = opentelemetry::logs;
namespace nostd         = opentelemetry::nostd;
namespace logs_exporter = opentelemetry::exporter::logs;

TEST(ElasticsearchLogsExporterTests, CustomClientConstructionSucceeds)
{
  logs_exporter::ElasticsearchExporterOptions opts;
  auto exporter = std::unique_ptr<sdklogs::LogRecordExporter>(
      new logs_exporter::ElasticsearchLogRecordExporter(opts));
  ASSERT_NE(exporter, nullptr);
}

// Attempt to write a log to an invalid host/port, test that the Export() returns failure
TEST(DISABLED_ElasticsearchLogsExporterTests, InvalidEndpoint)
{
  // Create invalid connection options for the elasticsearch exporter
  logs_exporter::ElasticsearchExporterOptions options("localhost", -1);

  // Create an elasticsearch exporter
  auto exporter = std::unique_ptr<sdklogs::LogRecordExporter>(
      new logs_exporter::ElasticsearchLogRecordExporter(options));

  // Create a log record and send to the exporter
  auto record = exporter->MakeRecordable();
  auto result = exporter->Export(nostd::span<std::unique_ptr<sdklogs::Recordable>>(&record, 1));

  // Ensure the return value is failure
  ASSERT_EQ(result, opentelemetry::sdk::common::ExportResult::kFailure);
}

// Test that when the exporter is shutdown, any call to Export should return failure
TEST(DISABLED_ElasticsearchLogsExporterTests, Shutdown)
{
  // Create an elasticsearch exporter and immediately shut it down
  auto exporter = std::unique_ptr<sdklogs::LogRecordExporter>(
      new logs_exporter::ElasticsearchLogRecordExporter);
  bool shutdownResult = exporter->Shutdown();
  ASSERT_TRUE(shutdownResult);

  // Write a log to the shutdown exporter
  auto record = exporter->MakeRecordable();
  auto result = exporter->Export(nostd::span<std::unique_ptr<sdklogs::Recordable>>(&record, 1));

  // Ensure the return value is failure
  ASSERT_EQ(result, opentelemetry::sdk::common::ExportResult::kFailure);
}

// Test the elasticsearch recordable object
TEST(DISABLED_ElasticsearchLogsExporterTests, RecordableCreation)
{
  // Create an elasticsearch exporter
  auto exporter = std::unique_ptr<sdklogs::LogRecordExporter>(
      new logs_exporter::ElasticsearchLogRecordExporter);

  // Create a recordable
  auto record = exporter->MakeRecordable();
  record->SetSeverity(logs_api::Severity::kFatal);
  record->SetTimestamp(std::chrono::system_clock::now());
  record->SetBody("Body of the log message");

  // Attributes and resource support different types
  record->SetAttribute("key0", false);
  record->SetAttribute("key1", "1");

  auto resource = opentelemetry::sdk::resource::Resource::Create({{"key2", 2}, {"key3", 3142}});
  record->SetResource(resource);

  exporter->Export(nostd::span<std::unique_ptr<sdklogs::Recordable>>(&record, 1));
}

TEST(ElasticsearchLogRecordableTests, BasicTests)
{
  const auto severity = logs_api::Severity::kFatal;
  const std::array<nostd::string_view, 2> stringlist{
      {nostd::string_view("string1"), nostd::string_view("string2")}};

  const std::int64_t expected_observed_ts = 1732063944999647774LL;
  const std::string expected_timestamp("2024-11-20T00:52:24.999647Z");
  const std::string expected_severity(
      opentelemetry::logs::SeverityNumToText[static_cast<std::size_t>(severity)]);
  const std::string expected_body("Body of the log message");
  const std::string expected_scope_name("scope_name");
  const bool expected_boolean  = false;
  const int expected_int       = 1;
  const double expected_double = 2.0;

  const nlohmann::json expected{
      {"@timestamp", expected_timestamp},
      {"boolean", expected_boolean},
      {"double", expected_double},
      {"ecs", {{"version", "8.11.0"}}},
      {"int", expected_int},
      {"log", {{"level", expected_severity}, {"logger", expected_scope_name}}},
      {"message", expected_body},
      {"observedtimestamp", expected_observed_ts},
      {"stringlist", {stringlist[0], stringlist[1]}}};

  const opentelemetry::common::SystemTimestamp now{std::chrono::nanoseconds(expected_observed_ts)};

  const auto scope =
      opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(expected_scope_name);

  opentelemetry::exporter::logs::ElasticSearchRecordable recordable;
  recordable.SetTimestamp(now);
  recordable.SetObservedTimestamp(now);
  recordable.SetSeverity(severity);
  recordable.SetBody(expected_body);
  recordable.SetInstrumentationScope(*scope);

  recordable.SetAttribute("boolean", expected_boolean);
  recordable.SetAttribute("int", expected_int);
  recordable.SetAttribute("double", expected_double);
  recordable.SetAttribute("stringlist", stringlist);

  const auto actual = recordable.GetJSON();

  EXPECT_EQ(actual, expected);
}

// ---------------------------------------------------------------------------
// ForceFlush deadline.
// ---------------------------------------------------------------------------
namespace
{
namespace http_client = opentelemetry::ext::http::client;

// Accepted by both the substring check and a top level "errors": false parse, so
// these cases keep meaning the same thing whichever success check is in place.
constexpr const char *kAcceptedBody =
    R"({"took":30,"errors":false,"items":[{"index":{"_shards":{"failed" : 0}}}]})";

class FakeResponse : public http_client::Response
{
public:
  FakeResponse(http_client::StatusCode status, const std::string &body)
      : status_(status), body_(body.begin(), body.end())
  {}
  const http_client::Body &GetBody() const noexcept override { return body_; }
  bool ForEachHeader(
      nostd::function_ref<bool(nostd::string_view, nostd::string_view)>) const noexcept override
  {
    return true;
  }
  bool ForEachHeader(
      const nostd::string_view &,
      nostd::function_ref<bool(nostd::string_view, nostd::string_view)>) const noexcept override
  {
    return true;
  }
  http_client::StatusCode GetStatusCode() const noexcept override { return status_; }

private:
  http_client::StatusCode status_;
  http_client::Body body_;
};

class FakeRequest : public http_client::Request
{
public:
  void SetMethod(http_client::Method) noexcept override {}
  void SetUri(nostd::string_view) noexcept override {}
  void SetSslOptions(const http_client::HttpSslOptions &) noexcept override {}
  void SetBody(http_client::Body &) noexcept override {}
  void AddHeader(nostd::string_view, nostd::string_view) noexcept override {}
  void ReplaceHeader(nostd::string_view, nostd::string_view) noexcept override {}
  void SetTimeoutMs(std::chrono::milliseconds) noexcept override {}
  void SetCompression(const http_client::Compression &) noexcept override {}
  void EnableLogging(bool) noexcept override {}
  void SetRetryPolicy(const http_client::RetryPolicy &) noexcept override {}
};

using EventScript = std::function<void(const std::shared_ptr<http_client::EventHandler> &)>;

class FakeSession : public http_client::Session
{
public:
  explicit FakeSession(EventScript script) : script_(std::move(script)) {}
  std::shared_ptr<http_client::Request> CreateRequest() noexcept override
  {
    return std::make_shared<FakeRequest>();
  }
  void SendRequest(std::shared_ptr<http_client::EventHandler> handler) noexcept override
  {
    script_(handler);
  }
  bool IsSessionActive() noexcept override { return false; }
  bool CancelSession() noexcept override { return true; }
  bool FinishSession() noexcept override { return true; }

private:
  EventScript script_;
};

class FakeHttpClient : public http_client::HttpClient
{
public:
  explicit FakeHttpClient(EventScript script) : script_(std::move(script)) {}
  std::shared_ptr<http_client::Session> CreateSession(nostd::string_view) noexcept override
  {
    return std::make_shared<FakeSession>(script_);
  }
  bool CancelAllSessions() noexcept override { return true; }
  bool FinishAllSessions() noexcept override { return true; }
  void SetMaxSessionsPerConnection(std::size_t) noexcept override {}

private:
  EventScript script_;
};

}  // namespace

// ---------------------------------------------------------------------------
// ForceFlush deadline. Only built with async export, which is the only
// configuration where the wait exists.
// ---------------------------------------------------------------------------
namespace
{
// A response timeout short enough that a wait bounded by it instead of by the caller's deadline
// is visible in the elapsed time.
constexpr int kShortResponseTimeoutSeconds = 2;

struct FlushFixture
{
  std::shared_ptr<FakeHttpClient> client;
  std::unique_ptr<logs_exporter::ElasticsearchLogRecordExporter> exporter;
};

FlushFixture MakeExporter(EventScript script)
{
  FlushFixture fixture;
  fixture.client = std::make_shared<FakeHttpClient>(std::move(script));
  logs_exporter::ElasticsearchExporterOptions options;
  options.response_timeout_ = kShortResponseTimeoutSeconds;
  fixture.exporter          = std::unique_ptr<logs_exporter::ElasticsearchLogRecordExporter>(
      new logs_exporter::ElasticsearchLogRecordExporter(options, fixture.client));
  return fixture;
}

void ExportOnce(logs_exporter::ElasticsearchLogRecordExporter &exporter)
{
  auto record = exporter.MakeRecordable();
  exporter.Export(nostd::span<std::unique_ptr<sdklogs::Recordable>>(&record, 1));
}
}  // namespace

// The wait these cases describe exists only in an async build, so they skip elsewhere rather than
// compile out: gtest_add_tests reads the source, and a case missing from the binary is still
// registered with CTest, where it then reports a pass without having run. The skip goes in SetUp
// because GTEST_SKIP returns, and a skip at the top of each body would leave the rest of that body
// unreachable, which MSVC reports as C4702.
namespace
{
class ElasticsearchForceFlushTests : public ::testing::Test
{
protected:
  void SetUp() override
  {
#ifndef ENABLE_ASYNC_EXPORT
    GTEST_SKIP() << "ForceFlush has nothing to wait for when async export is disabled";
#endif
  }
};
}  // namespace

// The session never calls back, so the flush cannot complete. It has to say so, and it has to say
// so when the caller's deadline runs out rather than when the response timeout does.
TEST_F(ElasticsearchForceFlushTests, ReportsFailureWhenTheFlushDoesNotComplete)
{
  // The handler is kept because a dropped one now reports a failure from its destructor, which
  // would finish the session and leave nothing for the flush to wait on. The real curl operation
  // owns the handler until the request ends, so this is also the truer shape.
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  auto fixture = MakeExporter([&kept](const std::shared_ptr<http_client::EventHandler> &handler) {
    kept.push_back(handler);
  });
  ExportOnce(*fixture.exporter);

  const auto start   = std::chrono::steady_clock::now();
  const bool flushed = fixture.exporter->ForceFlush(std::chrono::milliseconds{20});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(flushed);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            kShortResponseTimeoutSeconds * 1000);
}

// Nothing outstanding, so there is nothing to wait for.
TEST_F(ElasticsearchForceFlushTests, ReturnsImmediatelyWithNothingInFlight)
{
  auto fixture = MakeExporter([](const std::shared_ptr<http_client::EventHandler> &) {});

  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  start)
                .count(),
            kShortResponseTimeoutSeconds * 1000);
}

// The session completes inside SendRequest(), before the flush is even asked for, so the
// completion is already published when the predicate is first evaluated.
TEST_F(ElasticsearchForceFlushTests, SucceedsWhenTheSessionFinishedBeforeTheWait)
{
  auto fixture = MakeExporter([](const std::shared_ptr<http_client::EventHandler> &handler) {
    FakeResponse response(200, kAcceptedBody);
    handler->OnResponse(response);
  });
  ExportOnce(*fixture.exporter);

  EXPECT_TRUE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}

// Two exports, one of which never finishes. Reporting success here would tell the caller data was
// flushed that is still in flight.
TEST_F(ElasticsearchForceFlushTests, PartialCompletionIsNotSuccess)
{
  bool respond = true;
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  auto fixture =
      MakeExporter([&respond, &kept](const std::shared_ptr<http_client::EventHandler> &handler) {
        kept.push_back(handler);  // the second session has to stay unfinished
        if (respond)
        {
          FakeResponse response(200, kAcceptedBody);
          handler->OnResponse(response);
        }
      });
  ExportOnce(*fixture.exporter);
  respond = false;
  ExportOnce(*fixture.exporter);

  EXPECT_FALSE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}

// A completion delivered from another thread after the waiter has parked has to wake it. This is
// the half the inline scripts above cannot reach.
TEST_F(ElasticsearchForceFlushTests, ACompletionAfterTheWaiterParksWakesIt)
{
  // Held by the test, not by the fakes: a handler owns its session, so a session that also owned
  // its handler would be a reference cycle and leak.
  std::shared_ptr<http_client::EventHandler> captured;
  auto fixture =
      MakeExporter([&captured](const std::shared_ptr<http_client::EventHandler> &handler) {
        captured = handler;
      });
  ExportOnce(*fixture.exporter);
  ASSERT_NE(captured, nullptr);

  std::thread responder([&captured] {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    FakeResponse response(200, kAcceptedBody);
    captured->OnResponse(response);
  });

  const bool flushed = fixture.exporter->ForceFlush(std::chrono::seconds{5});
  responder.join();
  EXPECT_TRUE(flushed);
}

// A second caller gets its own deadline. Serialising the calls is fine, making the second one
// wait out the first one's is not, since its timeout would mean nothing.
TEST_F(ElasticsearchForceFlushTests, AConcurrentFlushKeepsItsOwnDeadline)
{
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  auto fixture = MakeExporter([&kept](const std::shared_ptr<http_client::EventHandler> &handler) {
    kept.push_back(handler);
  });
  ExportOnce(*fixture.exporter);

  // The first caller waits well past the bound asserted below, so a second caller that queued
  // behind it could not come in under that bound by accident.
  std::thread slow([&fixture] { fixture.exporter->ForceFlush(std::chrono::milliseconds{1500}); });
  std::this_thread::sleep_for(std::chrono::milliseconds{100});

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  slow.join();

  EXPECT_LT(ms, 700) << "waited behind the first caller instead of its own deadline";
}

// The default argument is microseconds::max(), which AdjustWaitForTimeout maps to the sentinel for
// no deadline. That branch takes the lock outright and waits on the predicate, so it needs a case
// where the predicate already holds or the test would never return.
TEST_F(ElasticsearchForceFlushTests, AnIndefiniteFlushReturnsOnceEverythingIsFinished)
{
  auto fixture = MakeExporter([](const std::shared_ptr<http_client::EventHandler> &handler) {
    FakeResponse response(200, kAcceptedBody);
    handler->OnResponse(response);
  });
  ExportOnce(*fixture.exporter);

  EXPECT_TRUE(fixture.exporter->ForceFlush());
}

// A failed export still finishes its session, so the flush completes and reports success even
// though the export itself did not.
TEST_F(ElasticsearchForceFlushTests, AFailedExportStillFinishesItsSession)
{
  auto fixture = MakeExporter([](const std::shared_ptr<http_client::EventHandler> &handler) {
    FakeResponse response(200, R"({"took":1,"errors":true,"items":[]})");
    handler->OnResponse(response);
  });
  ExportOnce(*fixture.exporter);

  EXPECT_TRUE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}
// ---------------------------------------------------------------------------
// Exactly-once accounting for the async handler, which exists only in an async build, so
// these cases skip there rather than compile out.
// ---------------------------------------------------------------------------

class ElasticsearchAsyncCompletionTests : public ::testing::Test
{
protected:
  void SetUp() override
  {
#ifndef ENABLE_ASYNC_EXPORT
    GTEST_SKIP() << "the async handler does not exist when async export is disabled";
#endif
  }
};
// The three states the handler used to drop. Each has to finish the session, or ForceFlush waits
// on a request that can never complete.
TEST_F(ElasticsearchAsyncCompletionTests, PreviouslyIgnoredStatesFinishTheSession)
{
  for (const auto state :
       {http_client::SessionState::ReadError, http_client::SessionState::WriteError,
        http_client::SessionState::Destroyed})
  {
    SCOPED_TRACE(static_cast<int>(state));
    std::vector<std::shared_ptr<http_client::EventHandler>> kept;
    auto fixture =
        MakeExporter([&kept, state](const std::shared_ptr<http_client::EventHandler> &handler) {
          kept.push_back(handler);  // so the event, not the destructor, is what finishes it
          handler->OnEvent(state, "");
        });
    ExportOnce(*fixture.exporter);
    EXPECT_TRUE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
  }
}

// What Session::SendRequest does when HttpOperation::SendAsync fails to set up: the operation
// dispatches ConnectFailed and returns non-OK, then SendRequest dispatches CreateFailed for the
// same handler. One export, so one finished session, not two.
TEST_F(ElasticsearchAsyncCompletionTests, TwoTerminalEventsCountAsOneSession)
{
  // The real curl operation owns the handler until the request finishes, so a fake that lets it
  // go would make every request complete the moment SendRequest returns. The test owns them
  // instead of the fakes, since a handler owns its session and the reverse would be a cycle.
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  int session = 0;
  auto fixture =
      MakeExporter([&kept, &session](const std::shared_ptr<http_client::EventHandler> &handler) {
        kept.push_back(handler);
        if (++session == 1)
        {
          handler->OnEvent(http_client::SessionState::ConnectFailed, "");
          handler->OnEvent(http_client::SessionState::CreateFailed, "");
        }
      });

  ExportOnce(*fixture.exporter);  // reports twice before the fix
  ExportOnce(*fixture.exporter);  // never calls back

  EXPECT_FALSE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}))
      << "the second session is still in flight";
}

// The other in-tree double fire: the completion lambda uses two independent ifs, so an aborted
// operation that also has a response reports Cancelled and then delivers the response.
TEST_F(ElasticsearchAsyncCompletionTests, CancelledThenAResponseCountsOnce)
{
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  int session = 0;
  auto fixture =
      MakeExporter([&kept, &session](const std::shared_ptr<http_client::EventHandler> &handler) {
        kept.push_back(handler);
        if (++session == 1)
        {
          handler->OnEvent(http_client::SessionState::Cancelled, "");
          FakeResponse response(200, kAcceptedBody);
          handler->OnResponse(response);
        }
      });

  ExportOnce(*fixture.exporter);
  ExportOnce(*fixture.exporter);

  EXPECT_FALSE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}

// A response followed by the session being torn down is the ordinary successful shape.
TEST_F(ElasticsearchAsyncCompletionTests, ResponseThenDestroyedCountsOnce)
{
  std::vector<std::shared_ptr<http_client::EventHandler>> kept;
  int session = 0;
  auto fixture =
      MakeExporter([&kept, &session](const std::shared_ptr<http_client::EventHandler> &handler) {
        kept.push_back(handler);
        if (++session == 1)
        {
          FakeResponse response(200, kAcceptedBody);
          handler->OnResponse(response);
          handler->OnEvent(http_client::SessionState::Destroyed, "");
        }
      });

  ExportOnce(*fixture.exporter);
  ExportOnce(*fixture.exporter);

  EXPECT_FALSE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}

// A handler destroyed without ever reporting still has to finish its session.
TEST_F(ElasticsearchAsyncCompletionTests, AHandlerDestroyedWithoutAnOutcomeStillFinishes)
{
  auto fixture = MakeExporter([](const std::shared_ptr<http_client::EventHandler> &) {});
  ExportOnce(*fixture.exporter);
  EXPECT_TRUE(fixture.exporter->ForceFlush(std::chrono::milliseconds{20}));
}
