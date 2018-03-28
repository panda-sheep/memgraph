#include <memory>
#include <random>
#include <shared_mutex>
#include <vector>

#include "gflags/gflags.h"

#include "long_running_common.hpp"
#include "stats/stats.hpp"
#include "stats/stats_rpc_messages.hpp"
#include "threading/sync/rwlock.hpp"

class Graph500BfsClient : public TestClient {
 public:
  Graph500BfsClient(int id) : TestClient(), rg_(id) {
    auto result = Execute("MATCH (n:Node) RETURN count(1)", {}, "NumNodes");
    CHECK(result) << "Read-only query should not fail";
    num_nodes_ = result->records[0][0].ValueInt();
  }

 private:
  std::mt19937 rg_;
  int num_nodes_;

  void Step() override {
    std::uniform_int_distribution<int64_t> dist(0, num_nodes_ - 1);

    int start = -1;
    do {
      start = dist(rg_);
      auto result = Execute(
          "MATCH (n:Node {id: $id})-->(m) WHERE m != n "
          "RETURN count(m) AS degree",
          {{"id", start}}, "GetDegree");
      CHECK(result) << "Read-only query should not fail";
      if (result->records[0][0].ValueInt() > 0) {
        break;
      }
    } while (true);

    auto result =
        Execute("MATCH path = (n:Node {id: $id})-[*bfs]->() RETURN count(1)",
                {{"id", start}}, "Bfs");
    CHECK(result) << "Read-only query should not fail!";
  }
};

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < FLAGS_num_workers; ++i) {
    clients.emplace_back(std::make_unique<Graph500BfsClient>(i));
  }

  RunMultithreadedTest(clients);

  stats::StopStatsLogging();

  return 0;
}
