// Copyright 2017 Google Inc.
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

#include "bigtable/client/chrono_literals.h"
#include "bigtable/client/data.h"

#include <absl/memory/memory.h>
#include <gmock/gmock.h>
#include <google/bigtable/v2/bigtable_mock.grpc.pb.h>

/// Define types and functions used in the tests.
// TODO(#67) - refactor Mock classes to a testing/ subdirectory.
namespace {
class MockClient : public bigtable::ClientInterface {
 public:
  MockClient() : mock_stub(new google::bigtable::v2::MockBigtableStub) {}

  MOCK_METHOD1(Open,
               std::unique_ptr<bigtable::Table>(std::string const &table_id));
  MOCK_CONST_METHOD0(Stub, google::bigtable::v2::Bigtable::StubInterface &());

  std::unique_ptr<google::bigtable::v2::MockBigtableStub> mock_stub;
};

class MockReader : public grpc::ClientReaderInterface<
                       ::google::bigtable::v2::MutateRowsResponse> {
 public:
  MOCK_METHOD0(WaitForInitialMetadata, void());
  MOCK_METHOD0(Finish, grpc::Status());
  MOCK_METHOD1(NextMessageSize, bool(std::uint32_t *));
  MOCK_METHOD1(Read, bool(::google::bigtable::v2::MutateRowsResponse *));
};

class TableBulkApplyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    using namespace ::testing;
    namespace btproto = ::google::bigtable::v2;

    EXPECT_CALL(client_, Stub())
        .WillRepeatedly(Invoke([this]() -> btproto::Bigtable::StubInterface & {
          return *client_.mock_stub;
        }));
  }
  MockClient client_;
  bigtable::Table table_ = bigtable::Table(&client_, "foo-table");
};
}  // anonymous namespace

/// @test Verify that Table::BulkApply() works in the easy case.
TEST_F(TableBulkApplyTest, Simple) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  auto reader = absl::make_unique<MockReader>();
  EXPECT_CALL(*reader, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        {
          auto &e = *r->add_entries();
          e.set_index(1);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*reader, Finish()).WillOnce(Return(grpc::Status::OK));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&reader](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return reader.release();
          }));

  EXPECT_NO_THROW(table_.BulkApply(bt::BulkMutation(
      bt::SingleRowMutation("foo", {bt::SetCell("fam", "col", 0, "baz")}),
      bt::SingleRowMutation("bar", {bt::SetCell("fam", "col", 0, "qux")}))));
}

/// @test Verify that Table::BulkApply() retries partial failures.
TEST_F(TableBulkApplyTest, RetryPartialFailure) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  auto r1 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r1, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        // Simulate a partial (recoverable) failure.
        auto &e0 = *r->add_entries();
        e0.set_index(0);
        e0.mutable_status()->set_code(grpc::UNAVAILABLE);
        auto &e1 = *r->add_entries();
        e1.set_index(1);
        e1.mutable_status()->set_code(grpc::OK);
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r1, Finish()).WillOnce(Return(grpc::Status::OK));

  auto r2 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r2, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r2, Finish()).WillOnce(Return(grpc::Status::OK));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&r1](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r1.release();
          }))
      .WillOnce(Invoke(
          [&r2](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r2.release();
          }));

  EXPECT_NO_THROW(table_.BulkApply(bt::BulkMutation(
      bt::SingleRowMutation("foo", {bigtable::SetCell("fam", "col", 0, "baz")}),
      bt::SingleRowMutation("bar",
                            {bigtable::SetCell("fam", "col", 0, "qux")}))));
}

/// @test Verify that Table::BulkApply() handles permanent failures.
TEST_F(TableBulkApplyTest, PermanentFailure) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  auto r1 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r1, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        {
          auto &e = *r->add_entries();
          e.set_index(1);
          e.mutable_status()->set_code(grpc::OUT_OF_RANGE);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r1, Finish()).WillOnce(Return(grpc::Status::OK));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&r1](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r1.release();
          }));

  EXPECT_THROW(
      table_.BulkApply(bt::BulkMutation(
          bt::SingleRowMutation("foo", {bt::SetCell("fam", "col", 0, "baz")}),
          bt::SingleRowMutation("bar", {bt::SetCell("fam", "col", 0, "qux")}))),
      std::exception);
}

/// @test Verify that Table::BulkApply() handles a terminated stream.
TEST_F(TableBulkApplyTest, CanceledStream) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  // Simulate a stream that returns one success and then terminates.  We expect
  // the BulkApply() operation to retry the request, because the mutation is in
  // an undetermined state.  Well, it should retry assuming it is idempotent,
  // which happens to be the case in this test.
  auto r1 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r1, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r1, Finish()).WillOnce(Return(grpc::Status::OK));

  // Create a second stream returned by the mocks when the client retries.
  auto r2 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r2, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r2, Finish()).WillOnce(Return(grpc::Status::OK));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&r1](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r1.release();
          }))
      .WillOnce(Invoke(
          [&r2](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r2.release();
          }));

  EXPECT_NO_THROW(table_.BulkApply(bt::BulkMutation(
      bt::SingleRowMutation("foo", {bt::SetCell("fam", "col", 0, "baz")}),
      bt::SingleRowMutation("bar", {bt::SetCell("fam", "col", 0, "qux")}))));
}

/// @test Verify that Table::BulkApply() reports correctly on too many errors.
TEST_F(TableBulkApplyTest, TooManyFailures) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  using namespace bigtable::chrono_literals;
  // Create a table with specific policies so we can test the behavior
  // without having to depend on timers expiring.  In this case tolerate only
  // 3 failures.
  bt::Table custom_table(
      &client_, "foo_table",
      // Configure the Table to stop at 3 failures.
      bt::LimitedErrorCountRetryPolicy(2),
      // Use much shorter backoff than the default to test faster.
      bt::ExponentialBackoffPolicy(10_us, 40_us),
      // TODO(#66) - it is annoying to set a policy we do not care about.
      bt::SafeIdempotentMutationPolicy());

  // Setup the mocks to fail more than 3 times.
  auto r1 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r1, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r1, Finish())
      .WillOnce(Return(grpc::Status(grpc::StatusCode::ABORTED, "")));

  auto create_cancelled_stream = [&](grpc::ClientContext *,
                                     btproto::MutateRowsRequest const &) {
    auto stream = absl::make_unique<MockReader>();
    EXPECT_CALL(*stream, Read(_)).WillOnce(Return(false));
    EXPECT_CALL(*stream, Finish())
        .WillOnce(Return(grpc::Status(grpc::StatusCode::ABORTED, "")));
    return stream.release();
  };

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&r1](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r1.release();
          }))
      .WillOnce(Invoke(create_cancelled_stream))
      .WillOnce(Invoke(create_cancelled_stream));

  EXPECT_THROW(
      custom_table.BulkApply(bt::BulkMutation(
          bt::SingleRowMutation("foo", {bt::SetCell("fam", "col", 0, "baz")}),
          bt::SingleRowMutation("bar", {bt::SetCell("fam", "col", 0, "qux")}))),
      std::exception);
}

/// @test Verify that Table::BulkApply() retries only idempotent mutations.
TEST_F(TableBulkApplyTest, RetryOnlyIdempotent) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  // We will send both idempotent and non-idempotent mutations.  We prepare the
  // mocks to return an empty stream in the first RPC request.  That will force
  // the client to only retry the idempotent mutations.
  auto r1 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r1, Read(_)).WillOnce(Return(false));
  EXPECT_CALL(*r1, Finish()).WillOnce(Return(grpc::Status::OK));

  auto r2 = absl::make_unique<MockReader>();
  EXPECT_CALL(*r2, Read(_))
      .WillOnce(Invoke([](btproto::MutateRowsResponse *r) {
        {
          auto &e = *r->add_entries();
          e.set_index(0);
          e.mutable_status()->set_code(grpc::OK);
        }
        return true;
      }))
      .WillOnce(Return(false));
  EXPECT_CALL(*r2, Finish()).WillOnce(Return(grpc::Status::OK));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&r1](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r1.release();
          }))
      .WillOnce(Invoke(
          [&r2](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return r2.release();
          }));

  try {
    table_.BulkApply(bt::BulkMutation(
        bt::SingleRowMutation("is-idempotent",
                              {bt::SetCell("fam", "col", 0, "qux")}),
        bt::SingleRowMutation("not-idempotent",
                              {bt::SetCell("fam", "col", "baz")})));
  } catch (bt::PermanentMutationFailure const &ex) {
    ASSERT_EQ(1UL, ex.failures().size());
    EXPECT_EQ(1, ex.failures()[0].original_index());
    EXPECT_EQ("not-idempotent", ex.failures()[0].mutation().row_key());
  } catch (std::exception const &ex) {
    FAIL() << "unexpected std::exception raised: " << ex.what();
  } catch (...) {
    FAIL() << "unexpected exception of unknown type raised";
  }
}

/// @test Verify that Table::BulkApply() works when the RPC fails.
TEST_F(TableBulkApplyTest, FailedRPC) {
  using namespace ::testing;
  namespace btproto = ::google::bigtable::v2;
  namespace bt = ::bigtable;

  auto reader = absl::make_unique<MockReader>();
  EXPECT_CALL(*reader, Read(_)).WillOnce(Return(false));
  EXPECT_CALL(*reader, Finish())
      .WillOnce(Return(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                    "no such table")));

  EXPECT_CALL(*client_.mock_stub, MutateRowsRaw(_, _))
      .WillOnce(Invoke(
          [&reader](grpc::ClientContext *, btproto::MutateRowsRequest const &) {
            return reader.release();
          }));

  try {
    table_.BulkApply(bt::BulkMutation(
        bt::SingleRowMutation("foo", {bt::SetCell("fam", "col", 0, "baz")}),
        bt::SingleRowMutation("bar", {bt::SetCell("fam", "col", 0, "qux")})));
  } catch (bt::PermanentMutationFailure const &ex) {
    EXPECT_EQ(2UL, ex.failures().size());
    EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, ex.status().error_code());
    EXPECT_EQ("no such table", ex.status().error_message());
  } catch (std::exception const &ex) {
    FAIL() << "unexpected std::exception raised: " << ex.what();
  } catch (...) {
    FAIL() << "unexpected exception of unknown type raised";
  }
}