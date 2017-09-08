Feature: Bfs

  Scenario: Test match BFS depth blocked
      Given an empty graph
      And having executed:
          """
          CREATE (n {a:'0'})-[:r]->({a:'1.1'})-[:r]->({a:'2.1'}), (n)-[:r]->({a:'1.2'})
          """
      When executing query:
          """
          MATCH (n {a:'0'})-bfs(e, m| true, 1)->(m) RETURN n.a, m.a
          """
      Then the result should be:
          | n.a | m.a   |
          | '0' | '1.1' |
          | '0' | '1.2' |

  Scenario: Test match BFS filtered
      Given an empty graph
      And having executed:
          """
          CREATE (n {a:'0'})-[:r]->({a:'1.1'})-[:r]->({a:'2.1'}), (n)-[:r]->({a:'1.2'})
          """
      When executing query:
          """
          MATCH (n {a:'0'})-bfs(e, m| m.a = '1.1' OR m.a = '0', 10)->(m) RETURN n.a, m.a
          """
      Then the result should be:
          | n.a | m.a   |
          | '0' | '1.1' |

  Scenario: Test match BFS resulting edge list
      Given an empty graph
      And having executed:
          """
          CREATE (:Start)-[:r {id: 0}]->()-[:r {id: 1}]->()-[:r {id: 2}]->()-[:r {id: 3}]->()
          """
      When executing query:
          """
          MATCH (:Start)-bfs[r](e, m| true, 10)->(m) WHERE size(r) > 3
          RETURN size(r) AS s, (r[0]).id AS r0, (r[2]).id AS r2
          """
      Then the result should be:
          | s | r0 | r2 |
          | 4 | 0  | 2  |
