//
// Copyright 2017 Memgraph
// Created by Florijan Stamenkovic on 14.03.17.
//

#include <iterator>
#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "communication/result_stream_faker.hpp"
#include "dbms/dbms.hpp"
#include "query/context.hpp"
#include "query/exceptions.hpp"
#include "query/interpret/frame.hpp"
#include "query/plan/operator.hpp"

#include "query_plan_common.hpp"

using namespace query;
using namespace query::plan;

TEST(QueryPlan, CreateNodeWithAttributes) {
  Dbms dbms;
  auto dba = dbms.active();

  GraphDbTypes::Label label = dba->label("Person");
  GraphDbTypes::Property property = dba->label("age");

  AstTreeStorage storage;
  SymbolTable symbol_table;

  auto node = NODE("n");
  symbol_table[*node->identifier_] = symbol_table.CreateSymbol("n");
  node->labels_.emplace_back(label);
  node->properties_[property] = LITERAL(42);

  auto create = std::make_shared<CreateNode>(node, nullptr);
  PullAll(create, *dba, symbol_table);
  dba->advance_command();

  // count the number of vertices
  int vertex_count = 0;
  for (VertexAccessor vertex : dba->vertices()) {
    vertex_count++;
    EXPECT_EQ(vertex.labels().size(), 1);
    EXPECT_EQ(*vertex.labels().begin(), label);
    EXPECT_EQ(vertex.Properties().size(), 1);
    auto prop_eq = vertex.PropsAt(property) == TypedValue(42);
    ASSERT_EQ(prop_eq.type(), TypedValue::Type::Bool);
    EXPECT_TRUE(prop_eq.Value<bool>());
  }
  EXPECT_EQ(vertex_count, 1);
}

TEST(QueryPlan, CreateReturn) {
  // test CREATE (n:Person {age: 42}) RETURN n, n.age
  Dbms dbms;
  auto dba = dbms.active();

  GraphDbTypes::Label label = dba->label("Person");
  GraphDbTypes::Property property = dba->label("age");

  AstTreeStorage storage;
  SymbolTable symbol_table;

  auto node = NODE("n");
  auto sym_n = symbol_table.CreateSymbol("n");
  symbol_table[*node->identifier_] = sym_n;
  node->labels_.emplace_back(label);
  node->properties_[property] = LITERAL(42);

  auto create = std::make_shared<CreateNode>(node, nullptr);
  auto named_expr_n = NEXPR("n", IDENT("n"));
  symbol_table[*named_expr_n] = symbol_table.CreateSymbol("named_expr_n");
  symbol_table[*named_expr_n->expression_] = sym_n;
  auto prop_lookup = PROPERTY_LOOKUP("n", property);
  symbol_table[*prop_lookup->expression_] = sym_n;
  auto named_expr_n_p = NEXPR("n", prop_lookup);
  symbol_table[*named_expr_n_p] = symbol_table.CreateSymbol("named_expr_n_p");
  symbol_table[*named_expr_n->expression_] = sym_n;

  auto produce = MakeProduce(create, named_expr_n, named_expr_n_p);
  auto result = CollectProduce(produce, symbol_table, *dba);
  EXPECT_EQ(1, result.GetResults().size());
  EXPECT_EQ(2, result.GetResults()[0].size());
  EXPECT_EQ(TypedValue::Type::Vertex, result.GetResults()[0][0].type());
  EXPECT_EQ(1,
            result.GetResults()[0][0].Value<VertexAccessor>().labels().size());
  EXPECT_EQ(label,
            result.GetResults()[0][0].Value<VertexAccessor>().labels()[0]);
  EXPECT_EQ(TypedValue::Type::Int, result.GetResults()[0][1].type());
  EXPECT_EQ(42, result.GetResults()[0][1].Value<int64_t>());

  dba->advance_command();
  EXPECT_EQ(1, CountIterable(dba->vertices()));
}

TEST(QueryPlan, CreateExpand) {
  Dbms dbms;
  auto dba = dbms.active();

  GraphDbTypes::Label label_node_1 = dba->label("Node1");
  GraphDbTypes::Label label_node_2 = dba->label("Node2");
  GraphDbTypes::Property property = dba->label("prop");
  GraphDbTypes::EdgeType edge_type = dba->label("edge_type");

  SymbolTable symbol_table;
  AstTreeStorage storage;

  auto test_create_path = [&](bool cycle, int expected_nodes_created,
                              int expected_edges_created) {
    int before_v = CountIterable(dba->vertices());
    int before_e = CountIterable(dba->edges());

    // data for the first node
    auto n = NODE("n");
    n->labels_.emplace_back(label_node_1);
    n->properties_[property] = LITERAL(1);
    auto n_sym = symbol_table.CreateSymbol("n");
    symbol_table[*n->identifier_] = n_sym;

    // data for the second node
    auto m = NODE("m");
    m->labels_.emplace_back(label_node_2);
    m->properties_[property] = LITERAL(2);
    if (cycle)
      symbol_table[*m->identifier_] = n_sym;
    else
      symbol_table[*m->identifier_] = symbol_table.CreateSymbol("m");

    auto r = EDGE("r", EdgeAtom::Direction::RIGHT);
    symbol_table[*r->identifier_] = symbol_table.CreateSymbol("r");
    r->edge_types_.emplace_back(edge_type);
    r->properties_[property] = LITERAL(3);

    auto create_op = std::make_shared<CreateNode>(n, nullptr);
    auto create_expand =
        std::make_shared<CreateExpand>(m, r, create_op, n_sym, cycle);
    PullAll(create_expand, *dba, symbol_table);
    dba->advance_command();

    EXPECT_EQ(CountIterable(dba->vertices()) - before_v,
              expected_nodes_created);
    EXPECT_EQ(CountIterable(dba->edges()) - before_e, expected_edges_created);
  };

  test_create_path(false, 2, 1);
  test_create_path(true, 1, 1);

  for (VertexAccessor vertex : dba->vertices()) {
    EXPECT_EQ(vertex.labels().size(), 1);
    GraphDbTypes::Label label = vertex.labels()[0];
    if (label == label_node_1) {
      // node created by first op
      EXPECT_EQ(vertex.PropsAt(property).Value<int64_t>(), 1);
    } else if (label == label_node_2) {
      // node create by expansion
      EXPECT_EQ(vertex.PropsAt(property).Value<int64_t>(), 2);
    } else {
      // should not happen
      FAIL();
    }

    for (EdgeAccessor edge : dba->edges()) {
      EXPECT_EQ(edge.edge_type(), edge_type);
      EXPECT_EQ(edge.PropsAt(property).Value<int64_t>(), 3);
    }
  }
}

TEST(QueryPlan, MatchCreateNode) {
  Dbms dbms;
  auto dba = dbms.active();

  // add three nodes we'll match and expand-create from
  dba->insert_vertex();
  dba->insert_vertex();
  dba->insert_vertex();
  dba->advance_command();

  SymbolTable symbol_table;
  AstTreeStorage storage;

  // first node
  auto n_scan_all = MakeScanAll(storage, symbol_table, "n");
  // second node
  auto m = NODE("m");
  symbol_table[*m->identifier_] = symbol_table.CreateSymbol("m");
  // creation op
  auto create_node = std::make_shared<CreateNode>(m, n_scan_all.op_);

  EXPECT_EQ(CountIterable(dba->vertices()), 3);
  PullAll(create_node, *dba, symbol_table);
  dba->advance_command();
  EXPECT_EQ(CountIterable(dba->vertices()), 6);
}

TEST(QueryPlan, MatchCreateExpand) {
  Dbms dbms;
  auto dba = dbms.active();

  // add three nodes we'll match and expand-create from
  dba->insert_vertex();
  dba->insert_vertex();
  dba->insert_vertex();
  dba->advance_command();

  //  GraphDbTypes::Label label_node_1 = dba->label("Node1");
  //  GraphDbTypes::Label label_node_2 = dba->label("Node2");
  //  GraphDbTypes::Property property = dba->label("prop");
  GraphDbTypes::EdgeType edge_type = dba->label("edge_type");

  SymbolTable symbol_table;
  AstTreeStorage storage;

  auto test_create_path = [&](bool cycle, int expected_nodes_created,
                              int expected_edges_created) {
    int before_v = CountIterable(dba->vertices());
    int before_e = CountIterable(dba->edges());

    // data for the first node
    auto n_scan_all = MakeScanAll(storage, symbol_table, "n");

    // data for the second node
    auto m = NODE("m");
    if (cycle)
      symbol_table[*m->identifier_] = n_scan_all.sym_;
    else
      symbol_table[*m->identifier_] = symbol_table.CreateSymbol("m");

    auto r = EDGE("r", EdgeAtom::Direction::RIGHT);
    symbol_table[*r->identifier_] = symbol_table.CreateSymbol("r");
    r->edge_types_.emplace_back(edge_type);

    auto create_expand = std::make_shared<CreateExpand>(m, r, n_scan_all.op_,
                                                        n_scan_all.sym_, cycle);
    PullAll(create_expand, *dba, symbol_table);
    dba->advance_command();

    EXPECT_EQ(CountIterable(dba->vertices()) - before_v,
              expected_nodes_created);
    EXPECT_EQ(CountIterable(dba->edges()) - before_e, expected_edges_created);
  };

  test_create_path(false, 3, 3);
  test_create_path(true, 0, 6);
}

TEST(QueryPlan, Delete) {
  Dbms dbms;
  auto dba = dbms.active();

  // make a fully-connected (one-direction, no cycles) with 4 nodes
  std::vector<VertexAccessor> vertices;
  for (int i = 0; i < 4; ++i) vertices.push_back(dba->insert_vertex());
  auto type = dba->edge_type("type");
  for (int j = 0; j < 4; ++j)
    for (int k = j + 1; k < 4; ++k)
      dba->insert_edge(vertices[j], vertices[k], type);

  dba->advance_command();
  EXPECT_EQ(4, CountIterable(dba->vertices()));
  EXPECT_EQ(6, CountIterable(dba->edges()));

  AstTreeStorage storage;
  SymbolTable symbol_table;

  // attempt to delete a vertex, and fail
  {
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_get = storage.Create<Identifier>("n");
    symbol_table[*n_get] = n.sym_;
    auto delete_op = std::make_shared<plan::Delete>(
        n.op_, std::vector<Expression *>{n_get}, false);
    EXPECT_THROW(PullAll(delete_op, *dba, symbol_table), QueryRuntimeException);
    dba->advance_command();
    EXPECT_EQ(4, CountIterable(dba->vertices()));
    EXPECT_EQ(6, CountIterable(dba->edges()));
  }

  // detach delete a single vertex
  {
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_get = storage.Create<Identifier>("n");
    symbol_table[*n_get] = n.sym_;
    auto delete_op = std::make_shared<plan::Delete>(
        n.op_, std::vector<Expression *>{n_get}, true);
    Frame frame(symbol_table.max_position());
    delete_op->MakeCursor(*dba)->Pull(frame, symbol_table);
    dba->advance_command();
    EXPECT_EQ(3, CountIterable(dba->vertices()));
    EXPECT_EQ(3, CountIterable(dba->edges()));
  }

  // delete all remaining edges
  {
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto r_m = MakeExpand(storage, symbol_table, n.op_, n.sym_, "r",
                          EdgeAtom::Direction::RIGHT, false, "m", false);
    auto r_get = storage.Create<Identifier>("r");
    symbol_table[*r_get] = r_m.edge_sym_;
    auto delete_op = std::make_shared<plan::Delete>(
        r_m.op_, std::vector<Expression *>{r_get}, false);
    PullAll(delete_op, *dba, symbol_table);
    dba->advance_command();
    EXPECT_EQ(3, CountIterable(dba->vertices()));
    EXPECT_EQ(0, CountIterable(dba->edges()));
  }

  // delete all remaining vertices
  {
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_get = storage.Create<Identifier>("n");
    symbol_table[*n_get] = n.sym_;
    auto delete_op = std::make_shared<plan::Delete>(
        n.op_, std::vector<Expression *>{n_get}, false);
    PullAll(delete_op, *dba, symbol_table);
    dba->advance_command();
    EXPECT_EQ(0, CountIterable(dba->vertices()));
    EXPECT_EQ(0, CountIterable(dba->edges()));
  }
}

TEST(QueryPlan, DeleteReturn) {
  Dbms dbms;
  auto dba = dbms.active();

  // make a fully-connected (one-direction, no cycles) with 4 nodes
  auto prop = dba->property("prop");
  for (int i = 0; i < 4; ++i) {
    auto va = dba->insert_vertex();
    va.PropsSet(prop, 42);
  }

  dba->advance_command();
  EXPECT_EQ(4, CountIterable(dba->vertices()));
  EXPECT_EQ(0, CountIterable(dba->edges()));

  AstTreeStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");

  auto n_get = storage.Create<Identifier>("n");
  symbol_table[*n_get] = n.sym_;
  auto delete_op = std::make_shared<plan::Delete>(
      n.op_, std::vector<Expression *>{n_get}, true);

  auto prop_lookup =
      storage.Create<PropertyLookup>(storage.Create<Identifier>("n"), prop);
  symbol_table[*prop_lookup->expression_] = n.sym_;
  auto n_p = storage.Create<NamedExpression>("n", prop_lookup);
  symbol_table[*n_p] = symbol_table.CreateSymbol("bla");
  auto produce = MakeProduce(delete_op, n_p);

  auto result = CollectProduce(produce, symbol_table, *dba);
  EXPECT_EQ(4, result.GetResults().size());
  dba->advance_command();
  EXPECT_EQ(0, CountIterable(dba->vertices()));
}

TEST(QueryPlan, SetProperty) {
  Dbms dbms;
  auto dba = dbms.active();

  // graph with 4 vertices in connected pairs
  // the origin vertex in each par and both edges
  // have a property set
  auto v1 = dba->insert_vertex();
  auto v2 = dba->insert_vertex();
  auto v3 = dba->insert_vertex();
  auto v4 = dba->insert_vertex();
  auto edge_type = dba->edge_type("edge_type");
  dba->insert_edge(v1, v3, edge_type);
  dba->insert_edge(v2, v4, edge_type);
  dba->advance_command();

  AstTreeStorage storage;
  SymbolTable symbol_table;

  // scan (n)-[r]->(m)
  auto n = MakeScanAll(storage, symbol_table, "n");
  auto r_m = MakeExpand(storage, symbol_table, n.op_, n.sym_, "r",
                        EdgeAtom::Direction::RIGHT, false, "m", false);

  // set prop1 to 42 on n and r
  auto prop1 = dba->property("prop1");
  auto literal = LITERAL(42);

  auto n_p = PROPERTY_LOOKUP("n", prop1);
  symbol_table[*n_p->expression_] = n.sym_;
  auto set_n_p = std::make_shared<plan::SetProperty>(r_m.op_, n_p, literal);

  auto r_p = PROPERTY_LOOKUP("r", prop1);
  symbol_table[*r_p->expression_] = r_m.edge_sym_;
  auto set_r_p = std::make_shared<plan::SetProperty>(set_n_p, r_p, literal);
  EXPECT_EQ(2, PullAll(set_r_p, *dba, symbol_table));
  dba->advance_command();

  EXPECT_EQ(CountIterable(dba->edges()), 2);
  for (EdgeAccessor edge : dba->edges()) {
    ASSERT_EQ(edge.PropsAt(prop1).type(), PropertyValue::Type::Int);
    EXPECT_EQ(edge.PropsAt(prop1).Value<int64_t>(), 42);
    VertexAccessor from = edge.from();
    VertexAccessor to = edge.to();
    ASSERT_EQ(from.PropsAt(prop1).type(), PropertyValue::Type::Int);
    EXPECT_EQ(from.PropsAt(prop1).Value<int64_t>(), 42);
    ASSERT_EQ(to.PropsAt(prop1).type(), PropertyValue::Type::Null);
  }
}

TEST(QueryPlan, SetProperties) {
  auto test_set_properties = [](bool update) {
    Dbms dbms;
    auto dba = dbms.active();

    // graph: ({a: 0})-[:R {b:1}]->({c:2})
    auto prop_a = dba->property("a");
    auto prop_b = dba->property("b");
    auto prop_c = dba->property("c");
    auto v1 = dba->insert_vertex();
    auto v2 = dba->insert_vertex();
    auto e = dba->insert_edge(v1, v2, dba->edge_type("R"));
    v1.PropsSet(prop_a, 0);
    e.PropsSet(prop_b, 1);
    v2.PropsSet(prop_c, 2);
    dba->advance_command();

    AstTreeStorage storage;
    SymbolTable symbol_table;

    // scan (n)-[r]->(m)
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto r_m = MakeExpand(storage, symbol_table, n.op_, n.sym_, "r",
                          EdgeAtom::Direction::RIGHT, false, "m", false);

    auto op = update ? plan::SetProperties::Op::UPDATE
                     : plan::SetProperties::Op::REPLACE;

    // set properties on r to n, and on r to m
    auto r_ident = IDENT("r");
    symbol_table[*r_ident] = r_m.edge_sym_;
    auto m_ident = IDENT("m");
    symbol_table[*m_ident] = r_m.node_sym_;
    auto set_r_to_n =
        std::make_shared<plan::SetProperties>(r_m.op_, n.sym_, r_ident, op);
    auto set_m_to_r = std::make_shared<plan::SetProperties>(
        set_r_to_n, r_m.edge_sym_, m_ident, op);
    EXPECT_EQ(1, PullAll(set_m_to_r, *dba, symbol_table));
    dba->advance_command();

    EXPECT_EQ(CountIterable(dba->edges()), 1);
    for (EdgeAccessor edge : dba->edges()) {
      VertexAccessor from = edge.from();
      EXPECT_EQ(from.Properties().size(), update ? 2 : 1);
      if (update) {
        ASSERT_EQ(from.PropsAt(prop_a).type(), PropertyValue::Type::Int);
        EXPECT_EQ(from.PropsAt(prop_a).Value<int64_t>(), 0);
      }
      ASSERT_EQ(from.PropsAt(prop_b).type(), PropertyValue::Type::Int);
      EXPECT_EQ(from.PropsAt(prop_b).Value<int64_t>(), 1);

      EXPECT_EQ(edge.Properties().size(), update ? 2 : 1);
      if (update) {
        ASSERT_EQ(edge.PropsAt(prop_b).type(), PropertyValue::Type::Int);
        EXPECT_EQ(edge.PropsAt(prop_b).Value<int64_t>(), 1);
      }
      ASSERT_EQ(edge.PropsAt(prop_c).type(), PropertyValue::Type::Int);
      EXPECT_EQ(edge.PropsAt(prop_c).Value<int64_t>(), 2);

      VertexAccessor to = edge.to();
      EXPECT_EQ(to.Properties().size(), 1);
      ASSERT_EQ(to.PropsAt(prop_c).type(), PropertyValue::Type::Int);
      EXPECT_EQ(to.PropsAt(prop_c).Value<int64_t>(), 2);
    }
  };

  test_set_properties(true);
  test_set_properties(false);
}

TEST(QueryPlan, SetLabels) {
  Dbms dbms;
  auto dba = dbms.active();

  auto label1 = dba->label("label1");
  auto label2 = dba->label("label2");
  auto label3 = dba->label("label3");
  dba->insert_vertex().add_label(label1);
  dba->insert_vertex().add_label(label1);
  dba->advance_command();

  AstTreeStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");
  auto label_set = std::make_shared<plan::SetLabels>(
      n.op_, n.sym_, std::vector<GraphDbTypes::Label>{label2, label3});
  EXPECT_EQ(2, PullAll(label_set, *dba, symbol_table));

  for (VertexAccessor vertex : dba->vertices()) {
    vertex.SwitchNew();
    EXPECT_EQ(3, vertex.labels().size());
    EXPECT_TRUE(vertex.has_label(label2));
    EXPECT_TRUE(vertex.has_label(label3));
  }
}

TEST(QueryPlan, RemoveProperty) {
  Dbms dbms;
  auto dba = dbms.active();

  // graph with 4 vertices in connected pairs
  // the origin vertex in each par and both edges
  // have a property set
  auto prop1 = dba->property("prop1");
  auto v1 = dba->insert_vertex();
  auto v2 = dba->insert_vertex();
  auto v3 = dba->insert_vertex();
  auto v4 = dba->insert_vertex();
  auto edge_type = dba->edge_type("edge_type");
  dba->insert_edge(v1, v3, edge_type).PropsSet(prop1, 42);
  dba->insert_edge(v2, v4, edge_type);
  v2.PropsSet(prop1, 42);
  v3.PropsSet(prop1, 42);
  v4.PropsSet(prop1, 42);
  auto prop2 = dba->property("prop2");
  v1.PropsSet(prop2, 0);
  v2.PropsSet(prop2, 0);
  dba->advance_command();

  AstTreeStorage storage;
  SymbolTable symbol_table;

  // scan (n)-[r]->(m)
  auto n = MakeScanAll(storage, symbol_table, "n");
  auto r_m = MakeExpand(storage, symbol_table, n.op_, n.sym_, "r",
                        EdgeAtom::Direction::RIGHT, false, "m", false);

  auto n_p = PROPERTY_LOOKUP("n", prop1);
  symbol_table[*n_p->expression_] = n.sym_;
  auto set_n_p = std::make_shared<plan::RemoveProperty>(r_m.op_, n_p);

  auto r_p = PROPERTY_LOOKUP("r", prop1);
  symbol_table[*r_p->expression_] = r_m.edge_sym_;
  auto set_r_p = std::make_shared<plan::RemoveProperty>(set_n_p, r_p);
  EXPECT_EQ(2, PullAll(set_r_p, *dba, symbol_table));
  dba->advance_command();

  EXPECT_EQ(CountIterable(dba->edges()), 2);
  for (EdgeAccessor edge : dba->edges()) {
    EXPECT_EQ(edge.PropsAt(prop1).type(), PropertyValue::Type::Null);
    VertexAccessor from = edge.from();
    VertexAccessor to = edge.to();
    EXPECT_EQ(from.PropsAt(prop1).type(), PropertyValue::Type::Null);
    EXPECT_EQ(from.PropsAt(prop2).type(), PropertyValue::Type::Int);
    EXPECT_EQ(to.PropsAt(prop1).type(), PropertyValue::Type::Int);
  }
}

TEST(QueryPlan, RemoveLabels) {
  Dbms dbms;
  auto dba = dbms.active();

  auto label1 = dba->label("label1");
  auto label2 = dba->label("label2");
  auto label3 = dba->label("label3");
  auto v1 = dba->insert_vertex();
  v1.add_label(label1);
  v1.add_label(label2);
  v1.add_label(label3);
  auto v2 = dba->insert_vertex();
  v2.add_label(label1);
  v2.add_label(label3);
  dba->advance_command();

  AstTreeStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");
  auto label_remove = std::make_shared<plan::RemoveLabels>(
      n.op_, n.sym_, std::vector<GraphDbTypes::Label>{label1, label2});
  EXPECT_EQ(2, PullAll(label_remove, *dba, symbol_table));

  for (VertexAccessor vertex : dba->vertices()) {
    vertex.SwitchNew();
    EXPECT_EQ(1, vertex.labels().size());
    EXPECT_FALSE(vertex.has_label(label1));
    EXPECT_FALSE(vertex.has_label(label2));
  }
}

TEST(QueryPlan, NodeFilterSet) {
  Dbms dbms;
  auto dba = dbms.active();
  // Create a graph such that (v1 {prop: 42}) is connected to v2 and v3.
  auto v1 = dba->insert_vertex();
  auto prop = dba->property("prop");
  v1.PropsSet(prop, 42);
  auto v2 = dba->insert_vertex();
  auto v3 = dba->insert_vertex();
  auto edge_type = dba->edge_type("Edge");
  dba->insert_edge(v1, v2, edge_type);
  dba->insert_edge(v1, v3, edge_type);
  dba->advance_command();
  // Create operations which match (v1 {prop: 42}) -- (v) and increment the
  // v1.prop. The expected result is two incremenentations, since v1 is matched
  // twice for 2 edges it has.
  AstTreeStorage storage;
  SymbolTable symbol_table;
  // MATCH (n {prop: 42}) -[r]- (m)
  auto scan_all = MakeScanAll(storage, symbol_table, "n");
  scan_all.node_->properties_[prop] = LITERAL(42);
  auto expand = MakeExpand(storage, symbol_table, scan_all.op_, scan_all.sym_,
                           "r", EdgeAtom::Direction::BOTH, false, "m", false);
  auto node_filter =
      std::make_shared<NodeFilter>(expand.op_, scan_all.sym_, scan_all.node_);
  // SET n.prop = n.prop + 1
  auto set_prop = PROPERTY_LOOKUP("n", prop);
  symbol_table[*set_prop->expression_] = scan_all.sym_;
  auto add = ADD(set_prop, LITERAL(1));
  auto set = std::make_shared<plan::SetProperty>(node_filter, set_prop, add);
  EXPECT_EQ(2, PullAll(set, *dba, symbol_table));
  dba->advance_command();
  v1.Reconstruct();
  auto prop_eq = v1.PropsAt(prop) == TypedValue(42 + 2);
  ASSERT_EQ(prop_eq.type(), TypedValue::Type::Bool);
  EXPECT_TRUE(prop_eq.Value<bool>());
}

TEST(QueryPlan, FilterRemove) {
  Dbms dbms;
  auto dba = dbms.active();
  // Create a graph such that (v1 {prop: 42}) is connected to v2 and v3.
  auto v1 = dba->insert_vertex();
  auto prop = dba->property("prop");
  v1.PropsSet(prop, 42);
  auto v2 = dba->insert_vertex();
  auto v3 = dba->insert_vertex();
  auto edge_type = dba->edge_type("Edge");
  dba->insert_edge(v1, v2, edge_type);
  dba->insert_edge(v1, v3, edge_type);
  dba->advance_command();
  // Create operations which match (v1 {prop: 42}) -- (v) and remove v1.prop.
  // The expected result is two matches, for each edge of v1.
  AstTreeStorage storage;
  SymbolTable symbol_table;
  // MATCH (n) -[r]- (m) WHERE n.prop < 43
  auto scan_all = MakeScanAll(storage, symbol_table, "n");
  scan_all.node_->properties_[prop] = LITERAL(42);
  auto expand = MakeExpand(storage, symbol_table, scan_all.op_, scan_all.sym_,
                           "r", EdgeAtom::Direction::BOTH, false, "m", false);
  auto filter_prop = PROPERTY_LOOKUP("n", prop);
  symbol_table[*filter_prop->expression_] = scan_all.sym_;
  auto filter =
      std::make_shared<Filter>(expand.op_, LESS(filter_prop, LITERAL(43)));
  // REMOVE n.prop
  auto rem_prop = PROPERTY_LOOKUP("n", prop);
  symbol_table[*rem_prop->expression_] = scan_all.sym_;
  auto rem = std::make_shared<plan::RemoveProperty>(filter, rem_prop);
  EXPECT_EQ(2, PullAll(rem, *dba, symbol_table));
  dba->advance_command();
  v1.Reconstruct();
  EXPECT_EQ(v1.PropsAt(prop).type(), PropertyValue::Type::Null);
}

TEST(QueryPlan, SetRemove) {
  Dbms dbms;
  auto dba = dbms.active();
  auto v = dba->insert_vertex();
  auto label1 = dba->label("label1");
  auto label2 = dba->label("label2");
  dba->advance_command();
  // Create operations which match (v) and set and remove v :label.
  // The expected result is single (v) as it was at the start.
  AstTreeStorage storage;
  SymbolTable symbol_table;
  // MATCH (n) SET n :label1 :label2 REMOVE n :label1 :label2
  auto scan_all = MakeScanAll(storage, symbol_table, "n");
  auto set = std::make_shared<plan::SetLabels>(
      scan_all.op_, scan_all.sym_,
      std::vector<GraphDbTypes::Label>{label1, label2});
  auto rem = std::make_shared<plan::RemoveLabels>(
      set, scan_all.sym_, std::vector<GraphDbTypes::Label>{label1, label2});
  EXPECT_EQ(1, PullAll(rem, *dba, symbol_table));
  dba->advance_command();
  v.Reconstruct();
  EXPECT_FALSE(v.has_label(label1));
  EXPECT_FALSE(v.has_label(label2));
}
