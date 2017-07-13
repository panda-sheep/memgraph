#include <experimental/optional>
#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "database/graph_db_accessor.hpp"
#include "database/dbms.hpp"
#include "utils/bound.hpp"

using testing::UnorderedElementsAreArray;

template <typename TIterable>
auto Count(TIterable iterable) {
  return std::distance(iterable.begin(), iterable.end());
}

/**
 * A test fixture that contains a database, accessor,
 * label, property and an edge_type.
 */
class GraphDbAccessorIndex : public testing::Test {
 protected:
  Dbms dbms;
  std::unique_ptr<GraphDbAccessor> dba = dbms.active();
  GraphDbTypes::Property property = dba->property("property");
  GraphDbTypes::Label label = dba->label("label");
  GraphDbTypes::EdgeType edge_type = dba->edge_type("edge_type");

  auto AddVertex() {
    auto vertex = dba->insert_vertex();
    vertex.add_label(label);
    return vertex;
  }

  auto AddVertex(int property_value) {
    auto vertex = dba->insert_vertex();
    vertex.add_label(label);
    vertex.PropsSet(property, property_value);
    return vertex;
  }

  // commits the current dba, and replaces it with a new one
  void Commit() {
    dba->commit();
    auto dba2 = dbms.active();
    dba.swap(dba2);
  }
};

TEST_F(GraphDbAccessorIndex, LabelIndexCount) {
  auto label2 = dba->label("label2");
  EXPECT_EQ(dba->vertices_count(label), 0);
  EXPECT_EQ(dba->vertices_count(label2), 0);
  EXPECT_EQ(dba->vertices_count(), 0);
  for (int i = 0; i < 11; ++i) dba->insert_vertex().add_label(label);
  for (int i = 0; i < 17; ++i) dba->insert_vertex().add_label(label2);
  // even though xxx_count functions in GraphDbAccessor can over-estaimate
  // in this situation they should be exact (nothing was ever deleted)
  EXPECT_EQ(dba->vertices_count(label), 11);
  EXPECT_EQ(dba->vertices_count(label2), 17);
  EXPECT_EQ(dba->vertices_count(), 28);
}

TEST_F(GraphDbAccessorIndex, LabelIndexIteration) {
  // add 10 vertices, check visibility
  for (int i = 0; i < 10; i++) AddVertex();
  EXPECT_EQ(Count(dba->vertices(label, false)), 0);
  EXPECT_EQ(Count(dba->vertices(label, true)), 10);
  Commit();
  EXPECT_EQ(Count(dba->vertices(label, false)), 10);
  EXPECT_EQ(Count(dba->vertices(label, true)), 10);

  // remove 3 vertices, check visibility
  int deleted = 0;
  for (auto vertex : dba->vertices(false)) {
    dba->remove_vertex(vertex);
    if (++deleted >= 3) break;
  }
  EXPECT_EQ(Count(dba->vertices(label, false)), 10);
  EXPECT_EQ(Count(dba->vertices(label, true)), 7);
  Commit();
  EXPECT_EQ(Count(dba->vertices(label, false)), 7);
  EXPECT_EQ(Count(dba->vertices(label, true)), 7);
}

TEST_F(GraphDbAccessorIndex, EdgeTypeCount) {
  auto edge_type2 = dba->edge_type("edge_type2");
  EXPECT_EQ(dba->edges_count(edge_type), 0);
  EXPECT_EQ(dba->edges_count(edge_type2), 0);
  EXPECT_EQ(dba->edges_count(), 0);

  auto v1 = AddVertex();
  auto v2 = AddVertex();
  for (int i = 0; i < 11; ++i) dba->insert_edge(v1, v2, edge_type);
  for (int i = 0; i < 17; ++i) dba->insert_edge(v1, v2, edge_type2);
  // even though xxx_count functions in GraphDbAccessor can over-estaimate
  // in this situation they should be exact (nothing was ever deleted)
  EXPECT_EQ(dba->edges_count(edge_type), 11);
  EXPECT_EQ(dba->edges_count(edge_type2), 17);
  EXPECT_EQ(dba->edges_count(), 28);
}

TEST_F(GraphDbAccessorIndex, LabelPropertyIndexBuild) {
  AddVertex(0);
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(dba->vertices_count(label, property), "Index doesn't exist.");

  Commit();
  dba->BuildIndex(label, property);
  Commit();

  EXPECT_EQ(dba->vertices_count(label, property), 1);

  // confirm there is a differentiation of indexes based on (label, property)
  auto label2 = dba->label("label2");
  auto property2 = dba->property("property2");
  dba->BuildIndex(label2, property);
  dba->BuildIndex(label, property2);
  Commit();

  EXPECT_EQ(dba->vertices_count(label, property), 1);
  EXPECT_EQ(dba->vertices_count(label2, property), 0);
  EXPECT_EQ(dba->vertices_count(label, property2), 0);
}

TEST_F(GraphDbAccessorIndex, LabelPropertyIndexBuildTwice) {
  dba->BuildIndex(label, property);
  EXPECT_THROW(dba->BuildIndex(label, property), utils::BasicException);
}

TEST_F(GraphDbAccessorIndex, LabelPropertyIndexCount) {
  dba->BuildIndex(label, property);
  EXPECT_EQ(dba->vertices_count(label, property), 0);
  EXPECT_EQ(Count(dba->vertices(label, property)), 0);
  for (int i = 0; i < 14; ++i) AddVertex(0);
  EXPECT_EQ(dba->vertices_count(label, property), 14);
  EXPECT_EQ(Count(dba->vertices(label, property)), 14);
}

#define EXPECT_WITH_MARGIN(x, center) \
  EXPECT_THAT(                        \
      x, testing::AllOf(testing::Ge(center - 2), testing::Le(center + 2)));

TEST_F(GraphDbAccessorIndex, LabelPropertyValueCount) {
  dba->BuildIndex(label, property);

  // add some vertices without the property
  for (int i = 0; i < 20; i++) AddVertex();

  // add vertices with prop values [0, 29), ten vertices for each value
  for (int i = 0; i < 300; i++) AddVertex(i / 10);
  // add verties in t he [30, 40) range, 100 vertices for each value
  for (int i = 0; i < 1000; i++) AddVertex(30 + i / 100);

  // test estimates for exact value count
  EXPECT_WITH_MARGIN(dba->vertices_count(label, property, 10), 10);
  EXPECT_WITH_MARGIN(dba->vertices_count(label, property, 14), 10);
  EXPECT_WITH_MARGIN(dba->vertices_count(label, property, 30), 100);
  EXPECT_WITH_MARGIN(dba->vertices_count(label, property, 39), 100);
  EXPECT_EQ(dba->vertices_count(label, property, 40), 0);

  // helper functions
  auto Inclusive = [](int64_t value) {
    return std::experimental::make_optional(
        utils::MakeBoundInclusive(PropertyValue(value)));
  };
  auto Exclusive = [](int64_t value) {
    return std::experimental::make_optional(
        utils::MakeBoundExclusive(PropertyValue(value)));
  };
  auto vertices_count = [this](auto lower, auto upper) {
    return dba->vertices_count(label, property, lower, upper);
  };

  using std::experimental::nullopt;
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(vertices_count(nullopt, nullopt), "bound must be provided");
  EXPECT_WITH_MARGIN(vertices_count(nullopt, Exclusive(4)), 40);
  EXPECT_WITH_MARGIN(vertices_count(nullopt, Inclusive(4)), 50);
  EXPECT_WITH_MARGIN(vertices_count(Exclusive(13), nullopt), 160 + 1000);
  EXPECT_WITH_MARGIN(vertices_count(Inclusive(13), nullopt), 170 + 1000);
  EXPECT_WITH_MARGIN(vertices_count(Inclusive(13), Exclusive(14)), 10);
  EXPECT_WITH_MARGIN(vertices_count(Exclusive(13), Inclusive(14)), 10);
  EXPECT_WITH_MARGIN(vertices_count(Exclusive(13), Exclusive(13)), 0);
  EXPECT_WITH_MARGIN(vertices_count(Inclusive(20), Exclusive(13)), 0);
}

#undef EXPECT_WITH_MARGIN

TEST_F(GraphDbAccessorIndex, LabelPropertyValueIteration) {
  dba->BuildIndex(label, property);
  Commit();

  // insert 10 verties and and check visibility
  for (int i = 0; i < 10; i++) AddVertex(12);
  EXPECT_EQ(Count(dba->vertices(label, property, 12, false)), 0);
  EXPECT_EQ(Count(dba->vertices(label, property, 12, true)), 10);
  Commit();
  EXPECT_EQ(Count(dba->vertices(label, property, 12, false)), 10);
  EXPECT_EQ(Count(dba->vertices(label, property, 12, true)), 10);
}

TEST_F(GraphDbAccessorIndex, LabelPropertyValueSorting) {
  dba->BuildIndex(label, property);
  Commit();

  std::vector<PropertyValue> expected_property_value(50, 0);

  // strings
  for (int i = 0; i < 10; ++i) {
    auto vertex_accessor = dba->insert_vertex();
    vertex_accessor.add_label(label);
    vertex_accessor.PropsSet(property,
                             static_cast<std::string>(std::to_string(i)));
    expected_property_value[i] = vertex_accessor.PropsAt(property);
  }
  // bools - insert in reverse to check for comparison between values.
  for (int i = 9; i >= 0; --i) {
    auto vertex_accessor = dba->insert_vertex();
    vertex_accessor.add_label(label);
    vertex_accessor.PropsSet(property, static_cast<bool>(i / 5));
    expected_property_value[10 + i] = vertex_accessor.PropsAt(property);
  }

  // integers
  for (int i = 0; i < 10; ++i) {
    auto vertex_accessor = dba->insert_vertex();
    vertex_accessor.add_label(label);
    vertex_accessor.PropsSet(property, i);
    expected_property_value[20 + 2 * i] = vertex_accessor.PropsAt(property);
  }
  // doubles
  for (int i = 0; i < 10; ++i) {
    auto vertex_accessor = dba->insert_vertex();
    vertex_accessor.add_label(label);
    vertex_accessor.PropsSet(property, static_cast<double>(i + 0.5));
    expected_property_value[20 + 2 * i + 1] = vertex_accessor.PropsAt(property);
  }

  // lists of ints - insert in reverse to check for comparision between
  // lists.
  for (int i = 9; i >= 0; --i) {
    auto vertex_accessor = dba->insert_vertex();
    vertex_accessor.add_label(label);
    std::vector<PropertyValue> value;
    value.push_back(PropertyValue(i));
    vertex_accessor.PropsSet(property, value);
    expected_property_value[40 + i] = vertex_accessor.PropsAt(property);
  }

  EXPECT_EQ(Count(dba->vertices(label, property, false)), 0);
  EXPECT_EQ(Count(dba->vertices(label, property, true)), 50);

  int cnt = 0;
  for (auto vertex : dba->vertices(label, property, true)) {
    const PropertyValue &property_value = vertex.PropsAt(property);
    EXPECT_EQ(property_value.type(), expected_property_value[cnt].type());
    switch (property_value.type()) {
      case PropertyValue::Type::Bool:
        EXPECT_EQ(property_value.Value<bool>(),
                  expected_property_value[cnt].Value<bool>());
        break;
      case PropertyValue::Type::Double:
        EXPECT_EQ(property_value.Value<double>(),
                  expected_property_value[cnt].Value<double>());
        break;
      case PropertyValue::Type::Int:
        EXPECT_EQ(property_value.Value<int64_t>(),
                  expected_property_value[cnt].Value<int64_t>());
        break;
      case PropertyValue::Type::String:
        EXPECT_EQ(property_value.Value<std::string>(),
                  expected_property_value[cnt].Value<std::string>());
        break;
      case PropertyValue::Type::List: {
        auto received_value =
            property_value.Value<std::vector<PropertyValue>>();
        auto expected_value =
            expected_property_value[cnt].Value<std::vector<PropertyValue>>();
        EXPECT_EQ(received_value.size(), expected_value.size());
        EXPECT_EQ(received_value.size(), 1);
        EXPECT_EQ(received_value[0].Value<int64_t>(),
                  expected_value[0].Value<int64_t>());
        break;
      }
      case PropertyValue::Type::Null:
        ASSERT_FALSE("Invalid value type.");
    }
    ++cnt;
  }
}

/**
 * A test fixture that contains a database, accessor,
 * (label, property) index and 100 vertices, 10 for
 * each of [0, 10) property values.
 */
class GraphDbAccesssorIndexRange : public GraphDbAccessorIndex {
 protected:
  void SetUp() override {
    dba->BuildIndex(label, property);
    for (int i = 0; i < 100; i++) AddVertex(i / 10);

    ASSERT_EQ(Count(dba->vertices(false)), 0);
    ASSERT_EQ(Count(dba->vertices(true)), 100);
    Commit();
    ASSERT_EQ(Count(dba->vertices(false)), 100);
  }

  auto Vertices(std::experimental::optional<utils::Bound<PropertyValue>> lower,
                std::experimental::optional<utils::Bound<PropertyValue>> upper,
                bool current_state = false) {
    return dba->vertices(label, property, lower, upper, current_state);
  }

  auto Inclusive(PropertyValue value) {
    return std::experimental::make_optional(
        utils::MakeBoundInclusive(PropertyValue(value)));
  }

  auto Exclusive(int value) {
    return std::experimental::make_optional(
        utils::MakeBoundExclusive(PropertyValue(value)));
  }
};

TEST_F(GraphDbAccesssorIndexRange, RangeIteration) {
  using std::experimental::nullopt;
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(7))), 80);
  EXPECT_EQ(Count(Vertices(nullopt, Exclusive(7))), 70);
  EXPECT_EQ(Count(Vertices(Inclusive(7), nullopt)), 30);
  EXPECT_EQ(Count(Vertices(Exclusive(7), nullopt)), 20);
  EXPECT_EQ(Count(Vertices(Exclusive(3), Exclusive(6))), 20);
  EXPECT_EQ(Count(Vertices(Inclusive(3), Inclusive(6))), 40);
  EXPECT_EQ(Count(Vertices(Inclusive(6), Inclusive(3))), 0);
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(Vertices(nullopt, nullopt), "bound must be provided");
}

TEST_F(GraphDbAccesssorIndexRange, RangeIterationCurrentState) {
  using std::experimental::nullopt;
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(7))), 80);
  for (int i = 0; i < 20; i++) AddVertex(2);
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(7))), 80);
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(7), true)), 100);
  Commit();
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(7))), 100);
}

TEST_F(GraphDbAccesssorIndexRange, RangeInterationIncompatibleTypes) {
  using std::experimental::nullopt;

  // using PropertyValue::Null as a bound fails with an assertion
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(Vertices(nullopt, Inclusive(PropertyValue::Null)),
               "not a valid index bound");
  EXPECT_DEATH(Vertices(Inclusive(PropertyValue::Null), nullopt),
               "not a valid index bound");
  std::vector<PropertyValue> incompatible_with_int{
      "string", true, std::vector<PropertyValue>{1}};

  // using incompatible upper and lower bounds yields no results
  EXPECT_EQ(Count(Vertices(Inclusive(2), Inclusive("string"))), 0);

  // for incomparable bound and stored data,
  // expect that no results are returned
  ASSERT_EQ(Count(Vertices(Inclusive(0), nullopt)), 100);
  for (PropertyValue value : incompatible_with_int) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EQ(Count(Vertices(nullopt, Inclusive(value))), 0)
        << "Found vertices of type int for predicate value type: "
        << value.type();
    EXPECT_EQ(Count(Vertices(Inclusive(value), nullopt)), 0)
        << "Found vertices of type int for predicate value type: "
        << value.type();
  }

  // we can compare int to double
  EXPECT_EQ(Count(Vertices(nullopt, Inclusive(1000.0))), 100);
  EXPECT_EQ(Count(Vertices(Inclusive(0.0), nullopt)), 100);
}
