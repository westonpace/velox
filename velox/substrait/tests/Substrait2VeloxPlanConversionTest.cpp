/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/substrait/tests/JsonToProtoConverter.h"

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/connectors/Connector.h"
#include "velox/dwio/common/tests/utils/DataFiles.h"
#include "velox/exec/Exchange.h"
#include "velox/exec/PartitionedOutputBufferManager.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/substrait/SubstraitToVeloxPlan.h"
#include "velox/type/Type.h"

using namespace facebook::velox;
using namespace facebook::velox::test;
using namespace facebook::velox::connector::hive;
using namespace facebook::velox::exec;

class MockExchangeSource : public facebook::velox::exec::ExchangeSource {
 public:
  MockExchangeSource(
      const std::string& taskId,
      int destination,
      std::shared_ptr<ExchangeQueue> queue,
      memory::MemoryPool* FOLLY_NONNULL pool,
      RowVectorPtr table)
      : facebook::velox::exec::ExchangeSource(taskId, destination, queue, pool), table_(table) {}

  bool shouldRequestLocked() override {
    if (atEnd_) {
      return false;
    }
    return true;
  }

  std::unique_ptr<SerializedPage> toSerializedPage(VectorPtr vector) {
    auto data = std::make_unique<VectorStreamGroup>(facebook::velox::memory::MemoryAllocator::getInstance());
    auto size = vector->size();
    auto range = IndexRange{0, size};
    data->createStreamTree(
        std::dynamic_pointer_cast<const RowType>(vector->type()), size);
    data->append(
        std::dynamic_pointer_cast<RowVector>(vector), folly::Range(&range, 1));
    auto weakBufferManager = facebook::velox::exec::PartitionedOutputBufferManager::getInstance();
    if (auto bufferManager = weakBufferManager.lock()) {
        auto listener = bufferManager->newListener();
        IOBufOutputStream stream(*facebook::velox::memory::MemoryAllocator::getInstance(), listener.get(), data->size());
        data->flush(&stream);
        return std::make_unique<SerializedPage>(stream.getIOBuf());
    }
    VELOX_FAIL("No PartitionedOutputBufferManager instance");
  }

  void request() override {
    std::lock_guard<std::mutex> l(queue_->mutex());
    atEnd_ = true;
    requestPending_ = false;
    sequence_++;
    queue_->enqueue(toSerializedPage(table_));
    queue_->enqueue(nullptr);
  }

  void close() override {
    auto buffers = PartitionedOutputBufferManager::getInstance().lock();
    buffers->deleteResults(taskId_, destination_);
  }

 private:
  RowVectorPtr table_;
};

std::unordered_map<std::string, RowVectorPtr>& getMockTablesMap() {
    static std::unordered_map<std::string, RowVectorPtr> mockTablesMap;
    return mockTablesMap;
}

std::unique_ptr<facebook::velox::exec::ExchangeSource> createMockExchangeSource(
    const std::string& taskId,
    int destination,
    std::shared_ptr<facebook::velox::exec::ExchangeQueue> queue,
    facebook::velox::memory::MemoryPool* FOLLY_NONNULL pool) {
  if (strncmp(taskId.c_str(), "mock://", 7) == 0) {
    auto table = getMockTablesMap()[taskId];
    return std::make_unique<MockExchangeSource>(
        taskId, destination, std::move(queue), pool, table);
  }
  return nullptr;
}

class Substrait2VeloxPlanConversionTest
    : public exec::test::HiveConnectorTestBase {

 public:

   void TearDown() override {
     getMockTablesMap().clear();
     exec::test::HiveConnectorTestBase::TearDown();
   }

 protected:
  std::vector<std::shared_ptr<facebook::velox::connector::ConnectorSplit>>
  makeSplits(
      const facebook::velox::substrait::SubstraitVeloxPlanConverter& converter,
      std::shared_ptr<const core::PlanNode> planNode) {
    const auto& splitInfos = converter.splitInfos();
    auto leafPlanNodeIds = planNode->leafPlanNodeIds();
    // Only one leaf node is expected here.
    EXPECT_EQ(1, leafPlanNodeIds.size());
    const auto& splitInfo = splitInfos.at(*leafPlanNodeIds.begin());

    const auto& paths = splitInfo->paths;
    const auto& starts = splitInfo->starts;
    const auto& lengths = splitInfo->lengths;
    const auto fileFormat = splitInfo->format;

    std::vector<std::shared_ptr<facebook::velox::connector::ConnectorSplit>>
        splits;
    splits.reserve(paths.size());

    for (int i = 0; i < paths.size(); i++) {
      auto path = fmt::format("{}{}", tmpDir_->path, paths[i]);
      auto start = starts[i];
      auto length = lengths[i];
      auto split = facebook::velox::exec::test::HiveConnectorSplitBuilder(path)
                       .fileFormat(fileFormat)
                       .start(start)
                       .length(length)
                       .build();
      splits.emplace_back(split);
    }
    return splits;
  }

  std::vector<std::shared_ptr<facebook::velox::connector::ConnectorSplit>>
  makeNamedTableSplits(
      const facebook::velox::substrait::SubstraitVeloxPlanConverter& converter,
      std::shared_ptr<const core::PlanNode> planNode) {
        return {};
  }

  std::shared_ptr<exec::test::TempDirectoryPath> tmpDir_{
      exec::test::TempDirectoryPath::create()};
};

// This test will firstly generate mock TPC-H lineitem ORC file. Then, Velox's
// computing will be tested based on the generated ORC file.
// Input: Json file of the Substrait plan for the below modified TPC-H Q6 query:
//
//  SELECT sum(l_extendedprice * l_discount) AS revenue
//  FROM lineitem
//  WHERE
//    l_shipdate_new >= 8766 AND l_shipdate_new < 9131 AND
//    l_discount BETWEEN .06 - 0.01 AND .06 + 0.01 AND
//    l_quantity < 24
//
//  Tested Velox operators: TableScan (Filter Pushdown), Project, Aggregate.
TEST_F(Substrait2VeloxPlanConversionTest, q6) {
  // Generate the used ORC file.
  auto type =
      ROW({"l_orderkey",
           "l_partkey",
           "l_suppkey",
           "l_linenumber",
           "l_quantity",
           "l_extendedprice",
           "l_discount",
           "l_tax",
           "l_returnflag",
           "l_linestatus",
           "l_shipdate",
           "l_commitdate",
           "l_receiptdate",
           "l_shipinstruct",
           "l_shipmode",
           "l_comment"},
          {BIGINT(),
           BIGINT(),
           BIGINT(),
           INTEGER(),
           DOUBLE(),
           DOUBLE(),
           DOUBLE(),
           DOUBLE(),
           VARCHAR(),
           VARCHAR(),
           DOUBLE(),
           DOUBLE(),
           DOUBLE(),
           VARCHAR(),
           VARCHAR(),
           VARCHAR()});
  std::shared_ptr<memory::MemoryPool> pool{memory::getDefaultMemoryPool()};
  std::vector<VectorPtr> vectors;
  // TPC-H lineitem table has 16 columns.
  int colNum = 16;
  vectors.reserve(colNum);
  std::vector<int64_t> lOrderkeyData = {
      4636438147,
      2012485446,
      1635327427,
      8374290148,
      2972204230,
      8001568994,
      989963396,
      2142695974,
      6354246853,
      4141748419};
  vectors.emplace_back(makeFlatVector<int64_t>(lOrderkeyData));
  std::vector<int64_t> lPartkeyData = {
      263222018,
      255918298,
      143549509,
      96877642,
      201976875,
      196938305,
      100260625,
      273511608,
      112999357,
      299103530};
  vectors.emplace_back(makeFlatVector<int64_t>(lPartkeyData));
  std::vector<int64_t> lSuppkeyData = {
      2102019,
      13998315,
      12989528,
      4717643,
      9976902,
      12618306,
      11940632,
      871626,
      1639379,
      3423588};
  vectors.emplace_back(makeFlatVector<int64_t>(lSuppkeyData));
  std::vector<int32_t> lLinenumberData = {4, 6, 1, 5, 1, 2, 1, 5, 2, 6};
  vectors.emplace_back(makeFlatVector<int32_t>(lLinenumberData));
  std::vector<double> lQuantityData = {
      6.0, 1.0, 19.0, 4.0, 6.0, 12.0, 23.0, 11.0, 16.0, 19.0};
  vectors.emplace_back(makeFlatVector<double>(lQuantityData));
  std::vector<double> lExtendedpriceData = {
      30586.05,
      7821.0,
      1551.33,
      30681.2,
      1941.78,
      66673.0,
      6322.44,
      41754.18,
      8704.26,
      63780.36};
  vectors.emplace_back(makeFlatVector<double>(lExtendedpriceData));
  std::vector<double> lDiscountData = {
      0.05, 0.06, 0.01, 0.07, 0.05, 0.06, 0.07, 0.05, 0.06, 0.07};
  vectors.emplace_back(makeFlatVector<double>(lDiscountData));
  std::vector<double> lTaxData = {
      0.02, 0.03, 0.01, 0.0, 0.01, 0.01, 0.03, 0.07, 0.01, 0.04};
  vectors.emplace_back(makeFlatVector<double>(lTaxData));
  std::vector<std::string> lReturnflagData = {
      "N", "A", "A", "R", "A", "N", "A", "A", "N", "R"};
  vectors.emplace_back(makeFlatVector<std::string>(lReturnflagData));
  std::vector<std::string> lLinestatusData = {
      "O", "F", "F", "F", "F", "O", "F", "F", "O", "F"};
  vectors.emplace_back(makeFlatVector<std::string>(lLinestatusData));
  std::vector<double> lShipdateNewData = {
      8953.666666666666,
      8773.666666666666,
      9034.666666666666,
      8558.666666666666,
      9072.666666666666,
      8864.666666666666,
      9004.666666666666,
      8778.666666666666,
      9013.666666666666,
      8832.666666666666};
  vectors.emplace_back(makeFlatVector<double>(lShipdateNewData));
  std::vector<double> lCommitdateNewData = {
      10447.666666666666,
      8953.666666666666,
      8325.666666666666,
      8527.666666666666,
      8438.666666666666,
      10049.666666666666,
      9036.666666666666,
      8666.666666666666,
      9519.666666666666,
      9138.666666666666};
  vectors.emplace_back(makeFlatVector<double>(lCommitdateNewData));
  std::vector<double> lReceiptdateNewData = {
      10456.666666666666,
      8979.666666666666,
      8299.666666666666,
      8474.666666666666,
      8525.666666666666,
      9996.666666666666,
      9103.666666666666,
      8726.666666666666,
      9593.666666666666,
      9178.666666666666};
  vectors.emplace_back(makeFlatVector<double>(lReceiptdateNewData));
  std::vector<std::string> lShipinstructData = {
      "COLLECT COD",
      "NONE",
      "TAKE BACK RETURN",
      "NONE",
      "TAKE BACK RETURN",
      "NONE",
      "DELIVER IN PERSON",
      "DELIVER IN PERSON",
      "TAKE BACK RETURN",
      "NONE"};
  vectors.emplace_back(makeFlatVector<std::string>(lShipinstructData));
  std::vector<std::string> lShipmodeData = {
      "FOB",
      "REG AIR",
      "MAIL",
      "FOB",
      "RAIL",
      "SHIP",
      "REG AIR",
      "REG AIR",
      "TRUCK",
      "AIR"};
  vectors.emplace_back(makeFlatVector<std::string>(lShipmodeData));
  std::vector<std::string> lCommentData = {
      " the furiously final foxes. quickly final p",
      "thely ironic",
      "ate furiously. even, pending pinto bean",
      "ackages af",
      "odolites. slyl",
      "ng the regular requests sleep above",
      "lets above the slyly ironic theodolites sl",
      "lyly regular excuses affi",
      "lly unusual theodolites grow slyly above",
      " the quickly ironic pains lose car"};
  vectors.emplace_back(makeFlatVector<std::string>(lCommentData));

  // Write data into an ORC file.
  writeToFile(
      tmpDir_->path + "/mock_lineitem.orc",
      {makeRowVector(type->names(), vectors)});

  // Find and deserialize Substrait plan json file.
  std::string planPath =
      getDataFilePath("velox/substrait/tests", "data/q6_first_stage.json");

  // Read q6_first_stage.json and resume the Substrait plan.
  ::substrait::Plan substraitPlan;
  JsonToProtoConverter::readFromFile(planPath, substraitPlan);

  // Convert to Velox PlanNode.
  facebook::velox::substrait::SubstraitVeloxPlanConverter planConverter(
      pool_.get());
  auto planNode = planConverter.toVeloxPlan(substraitPlan);

  auto expectedResult = makeRowVector({
      makeFlatVector<double>(1, [](auto /*row*/) { return 13613.1921; }),
  });

  exec::test::AssertQueryBuilder(planNode)
      .splits(makeSplits(planConverter, planNode))
      .assertResults(expectedResult);
}

// This test runs a simple query against in-memory data using a named table
// The Substrait plan is a hard-coded JSON file and contains two schemas with
// the expected named tables:
//
// "0": { "l_id": INTEGER, "left": DOUBLE }
// "1": { "r_id": INTEGER, "right": DOUBLE }
//
// The query is roughly:
//
//  SELECT t1.l_id, t1.left, t2.right FROM "0" AS t1
//  INNER JOIN "1" AS t2 ON t1.l_id = t2.r_id
//  ORDER BY t1.l_id
//  LIMIT 2
//
//  Tested Velox operators: ValuesNode (Named Table), Join, OrderBy, Limit.
TEST_F(Substrait2VeloxPlanConversionTest, namedTable) {
  facebook::velox::exec::ExchangeSource::registerFactory(createMockExchangeSource);

  auto leftTypes = ROW({"l_id", "left"}, {INTEGER(), DOUBLE()}); // table 0
  auto rightTypes = ROW({"r_id", "right"}, {INTEGER(), DOUBLE()}); // table 1
  auto outTypes = ROW({"l_id", "left", "right"}, {INTEGER(), DOUBLE(), DOUBLE()}); // expected output

  std::shared_ptr<memory::MemoryPool> pool{memory::getDefaultMemoryPool()};
  std::vector<VectorPtr> leftVectors(2);
  std::vector<VectorPtr> rightVectors(2);
  std::vector<VectorPtr> outVectors(3);
  leftVectors[0] = makeFlatVector<int32_t>({0, 1, 2, 3});
  leftVectors[1] = makeFlatVector<double>({10.0, 10.1, 10.2, 10.3});
  rightVectors[0] = makeFlatVector<int32_t>({3, 2, 1, 0});
  rightVectors[1] = makeFlatVector<double>({20.3, 20.2, 20.1, 20.0});
  // Only contains the first two due to the LIMIT 2
  outVectors[0] = makeFlatVector<int32_t>({0, 1});
  outVectors[1] = makeFlatVector<double>({10.0, 10.1});
  outVectors[2] = makeFlatVector<double>({20.0, 20.1});

  RowVectorPtr left = makeRowVector(leftTypes->names(), leftVectors);
  RowVectorPtr right = makeRowVector(rightTypes->names(), rightVectors);
  RowVectorPtr expected = makeRowVector(outTypes->names(), outVectors);

  getMockTablesMap().clear();
  getMockTablesMap()["mock://0"] = left;
  getMockTablesMap()["mock://1"] = right;

  // Find and deserialize Substrait plan json file.
  std::string planPath =
      getDataFilePath("velox/substrait/tests", "data/named_table.json");
  ::substrait::Plan substraitPlan;
  JsonToProtoConverter::readFromFile(planPath, substraitPlan);

  std::unordered_map<facebook::velox::core::PlanNodeId, std::string> splitInfos;

  facebook::velox::substrait::SubstraitVeloxPlanConverter::NamedTableHandler namedTableHandler = 
  [&] (const std::vector<std::string>& names,
       const facebook::velox::core::PlanNodeId& planNodeId,
       RowTypePtr rowType) -> facebook::velox::core::PlanNodePtr {
    if (names.size() != 1) {
        VELOX_FAIL("Expected to receive one name for each table in this test"); 
    }
    core::PlanNodePtr node = std::make_shared<core::ExchangeNode>(planNodeId, std::move(rowType));
    splitInfos[node->id()] = "mock://" + names[0];
    return node;
  };

//   facebook::velox::substrait::SubstraitVeloxPlanConverter::NamedTableHandler namedTableHandler = 
//   [&] (const std::vector<std::string>& names,
//        const facebook::velox::core::PlanNodeId& planNodeId,
//        RowTypePtr rowType) -> facebook::velox::core::PlanNodePtr {
//     if (names.size() != 1) {
//         VELOX_FAIL("Expected to receive one name for each table in this test"); 
//     }
//     RowVectorPtr table;
//     if (names[0] == "0") {
//         table = left;
//     } else if (names[0] == "1") {
//         table = right;
//     } else {
//         VELOX_FAIL("Expected to receive a table named '0' or '1'");
//     }
//     std::vector<RowVectorPtr> tables{std::move(table)};
//     return std::make_shared<core::ValuesNode>(planNodeId, std::move(tables));
//   };

  // Convert to Velox PlanNode.
  facebook::velox::substrait::SubstraitVeloxPlanConverter planConverter(
      pool_.get(), std::move(namedTableHandler));
  auto planNode = planConverter.toVeloxPlan(substraitPlan);

  exec::test::AssertQueryBuilder assertBuilder(planNode);
  for (const auto& splitInfo : splitInfos) {
    assertBuilder.split(splitInfo.first, std::make_shared<facebook::velox::exec::RemoteConnectorSplit>(splitInfo.second));
  }
  assertBuilder.assertResults(expected);

  getMockTablesMap().clear();
}
