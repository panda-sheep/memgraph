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
#include <optional>
#include <string>

namespace memgraph::storage::replication {
struct ReplicationClientConfig {
  std::optional<double> timeout;
  // The default delay between main checking/pinging replicas is 1s because
  // that seems like a reasonable timeframe in which main should notice a
  // replica is down.
  std::chrono::seconds replica_check_frequency{1};

  struct SSL {
    std::string key_file = "";
    std::string cert_file = "";
  };

  std::optional<SSL> ssl;
};

struct ReplicationServerConfig {
  struct SSL {
    std::string key_file;
    std::string cert_file;
    std::string ca_file;
    bool verify_peer;
  };

  std::optional<SSL> ssl;
};
}  // namespace memgraph::storage::replication
