#include <algorithm>
#include <type_traits>

#include "query/plan/operator.hpp"

#include "query/exceptions.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/interpret/eval.hpp"

// macro for the default implementation of LogicalOperator::Accept
// that accepts the visitor and visits it's input_ operator
#define ACCEPT_WITH_INPUT(class_name)                                    \
  bool class_name::Accept(HierarchicalLogicalOperatorVisitor &visitor) { \
    if (visitor.PreVisit(*this)) {                                       \
      input_->Accept(visitor);                                           \
    }                                                                    \
    return visitor.PostVisit(*this);                                     \
  }

namespace query::plan {

namespace {

// Sets a property on a record accessor from a TypedValue. In cases when the
// TypedValue cannot be converted to PropertyValue,
// QueryRuntimeException is raised.
template <class TRecordAccessor>
void PropsSetChecked(TRecordAccessor &record, GraphDbTypes::Property key,
                     TypedValue value) {
  try {
    record.PropsSet(key, value);
  } catch (const TypedValueException &) {
    throw QueryRuntimeException("'{}' cannot be used as a property value.",
                                value.type());
  }
}

// Checks if the given value of the symbol has the expected type. If not, raises
// QueryRuntimeException.
void ExpectType(Symbol symbol, TypedValue value, TypedValue::Type expected) {
  if (value.type() != expected)
    throw QueryRuntimeException("Expected a {} for '{}', but got {}.", expected,
                                symbol.name(), value.type());
}

}  // namespace

bool Once::OnceCursor::Pull(Frame &, const SymbolTable &) {
  if (!did_pull_) {
    did_pull_ = true;
    return true;
  }
  return false;
}

std::unique_ptr<Cursor> Once::MakeCursor(GraphDbAccessor &) {
  return std::make_unique<OnceCursor>();
}

void Once::OnceCursor::Reset() { did_pull_ = false; }

CreateNode::CreateNode(const NodeAtom *node_atom,
                       const std::shared_ptr<LogicalOperator> &input)
    : node_atom_(node_atom), input_(input ? input : std::make_shared<Once>()) {}

ACCEPT_WITH_INPUT(CreateNode)

std::unique_ptr<Cursor> CreateNode::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<CreateNodeCursor>(*this, db);
}

CreateNode::CreateNodeCursor::CreateNodeCursor(const CreateNode &self,
                                               GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool CreateNode::CreateNodeCursor::Pull(Frame &frame,
                                        const SymbolTable &symbol_table) {
  if (input_cursor_->Pull(frame, symbol_table)) {
    Create(frame, symbol_table);
    return true;
  }
  return false;
}

void CreateNode::CreateNodeCursor::Reset() { input_cursor_->Reset(); }

void CreateNode::CreateNodeCursor::Create(Frame &frame,
                                          const SymbolTable &symbol_table) {
  auto new_node = db_.insert_vertex();
  for (auto label : self_.node_atom_->labels_) new_node.add_label(label);

  // Evaluator should use the latest accessors, as modified in this query, when
  // setting properties on new nodes.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  for (auto &kv : self_.node_atom_->properties_)
    PropsSetChecked(new_node, kv.first, kv.second->Accept(evaluator));
  frame[symbol_table.at(*self_.node_atom_->identifier_)] = new_node;
}

CreateExpand::CreateExpand(const NodeAtom *node_atom, const EdgeAtom *edge_atom,
                           const std::shared_ptr<LogicalOperator> &input,
                           Symbol input_symbol, bool existing_node)
    : node_atom_(node_atom),
      edge_atom_(edge_atom),
      input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      existing_node_(existing_node) {}

ACCEPT_WITH_INPUT(CreateExpand)

std::unique_ptr<Cursor> CreateExpand::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<CreateExpandCursor>(*this, db);
}

CreateExpand::CreateExpandCursor::CreateExpandCursor(const CreateExpand &self,
                                                     GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool CreateExpand::CreateExpandCursor::Pull(Frame &frame,
                                            const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  // get the origin vertex
  TypedValue &vertex_value = frame[self_.input_symbol_];
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &v1 = vertex_value.Value<VertexAccessor>();

  // Similarly to CreateNode, newly created edges and nodes should use the
  // latest accesors.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  // E.g. we pickup new properties: `CREATE (n {p: 42}) -[:r {ep: n.p}]-> ()`
  v1.SwitchNew();

  // get the destination vertex (possibly an existing node)
  auto &v2 = OtherVertex(frame, symbol_table, evaluator);
  v2.SwitchNew();

  // create an edge between the two nodes
  switch (self_.edge_atom_->direction_) {
    case EdgeAtom::Direction::IN:
      CreateEdge(v2, v1, frame, symbol_table, evaluator);
      break;
    case EdgeAtom::Direction::OUT:
      CreateEdge(v1, v2, frame, symbol_table, evaluator);
      break;
    case EdgeAtom::Direction::BOTH:
      // in the case of an undirected CreateExpand we choose an arbitrary
      // direction. this is used in the MERGE clause
      // it is not allowed in the CREATE clause, and the semantic
      // checker needs to ensure it doesn't reach this point
      CreateEdge(v1, v2, frame, symbol_table, evaluator);
  }

  return true;
}

void CreateExpand::CreateExpandCursor::Reset() { input_cursor_->Reset(); }

VertexAccessor &CreateExpand::CreateExpandCursor::OtherVertex(
    Frame &frame, const SymbolTable &symbol_table,
    ExpressionEvaluator &evaluator) {
  if (self_.existing_node_) {
    const auto &dest_node_symbol =
        symbol_table.at(*self_.node_atom_->identifier_);
    TypedValue &dest_node_value = frame[dest_node_symbol];
    ExpectType(dest_node_symbol, dest_node_value, TypedValue::Type::Vertex);
    return dest_node_value.Value<VertexAccessor>();
  } else {
    // the node does not exist, it needs to be created
    auto node = db_.insert_vertex();
    for (auto label : self_.node_atom_->labels_) node.add_label(label);
    for (auto kv : self_.node_atom_->properties_)
      PropsSetChecked(node, kv.first, kv.second->Accept(evaluator));
    auto symbol = symbol_table.at(*self_.node_atom_->identifier_);
    frame[symbol] = node;
    return frame[symbol].Value<VertexAccessor>();
  }
}

void CreateExpand::CreateExpandCursor::CreateEdge(
    VertexAccessor &from, VertexAccessor &to, Frame &frame,
    const SymbolTable &symbol_table, ExpressionEvaluator &evaluator) {
  EdgeAccessor edge =
      db_.insert_edge(from, to, self_.edge_atom_->edge_types_[0]);
  for (auto kv : self_.edge_atom_->properties_)
    PropsSetChecked(edge, kv.first, kv.second->Accept(evaluator));
  frame[symbol_table.at(*self_.edge_atom_->identifier_)] = edge;
}

template <class TVerticesFun>
class ScanAllCursor : public Cursor {
 public:
  ScanAllCursor(Symbol output_symbol, std::unique_ptr<Cursor> input_cursor,
                TVerticesFun get_vertices, GraphDbAccessor &db)
      : output_symbol_(output_symbol),
        input_cursor_(std::move(input_cursor)),
        get_vertices_(std::move(get_vertices)),
        db_(db) {}

  bool Pull(Frame &frame, const SymbolTable &symbol_table) override {
    if (db_.should_abort()) throw HintedAbortError();
    if (!vertices_ || vertices_it_.value() == vertices_.value().end()) {
      if (!input_cursor_->Pull(frame, symbol_table)) return false;
      // We need a getter function, because in case of exhausting a lazy
      // iterable, we cannot simply reset it by calling begin().
      vertices_.emplace(get_vertices_(frame, symbol_table));
      vertices_it_.emplace(vertices_.value().begin());
    }

    // if vertices_ is empty then we are done even though we have just
    // reinitialized vertices_it_
    if (vertices_it_.value() == vertices_.value().end()) return false;

    frame[output_symbol_] = *vertices_it_.value()++;
    return true;
  }

  void Reset() override {
    input_cursor_->Reset();
    vertices_ = std::experimental::nullopt;
    vertices_it_ = std::experimental::nullopt;
  }

 private:
  const Symbol output_symbol_;
  const std::unique_ptr<Cursor> input_cursor_;
  TVerticesFun get_vertices_;
  std::experimental::optional<
      typename std::result_of<TVerticesFun(Frame &, const SymbolTable &)>::type>
      vertices_;
  std::experimental::optional<decltype(vertices_.value().begin())> vertices_it_;
  GraphDbAccessor &db_;
};

ScanAll::ScanAll(const std::shared_ptr<LogicalOperator> &input,
                 Symbol output_symbol, GraphView graph_view)
    : input_(input ? input : std::make_shared<Once>()),
      output_symbol_(output_symbol),
      graph_view_(graph_view) {
  permanent_assert(graph_view != GraphView::AS_IS,
                   "ScanAll must have explicitly defined GraphView")
}

ACCEPT_WITH_INPUT(ScanAll)

std::unique_ptr<Cursor> ScanAll::MakeCursor(GraphDbAccessor &db) {
  auto vertices = [this, &db](Frame &, const SymbolTable &) {
    return db.vertices(graph_view_ == GraphView::NEW);
  };
  return std::make_unique<ScanAllCursor<decltype(vertices)>>(
      output_symbol_, input_->MakeCursor(db), std::move(vertices), db);
}

ScanAllByLabel::ScanAllByLabel(const std::shared_ptr<LogicalOperator> &input,
                               Symbol output_symbol, GraphDbTypes::Label label,
                               GraphView graph_view)
    : ScanAll(input, output_symbol, graph_view), label_(label) {}

ACCEPT_WITH_INPUT(ScanAllByLabel)

std::unique_ptr<Cursor> ScanAllByLabel::MakeCursor(GraphDbAccessor &db) {
  auto vertices = [this, &db](Frame &, const SymbolTable &) {
    return db.vertices(label_, graph_view_ == GraphView::NEW);
  };
  return std::make_unique<ScanAllCursor<decltype(vertices)>>(
      output_symbol_, input_->MakeCursor(db), std::move(vertices), db);
}

ScanAllByLabelPropertyRange::ScanAllByLabelPropertyRange(
    const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
    GraphDbTypes::Label label, GraphDbTypes::Property property,
    std::experimental::optional<Bound> lower_bound,
    std::experimental::optional<Bound> upper_bound, GraphView graph_view)
    : ScanAll(input, output_symbol, graph_view),
      label_(label),
      property_(property),
      lower_bound_(lower_bound),
      upper_bound_(upper_bound) {
  debug_assert(lower_bound_ || upper_bound_, "Only one bound can be left out");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyRange)

std::unique_ptr<Cursor> ScanAllByLabelPropertyRange::MakeCursor(
    GraphDbAccessor &db) {
  auto is_less = [](const TypedValue &a, const TypedValue &b,
                    Bound::Type bound_type) {
    try {
      auto is_below = bound_type == Bound::Type::INCLUSIVE ? a < b : a <= b;
      if (is_below.IsNull() || is_below.Value<bool>()) return true;
    } catch (const TypedValueException &) {
      throw QueryRuntimeException(
          "Unable to compare values of type '{}' and '{}'", a.type(), b.type());
    }
    return false;
  };
  auto vertices = [this, &db, is_less](Frame &frame,
                                       const SymbolTable &symbol_table) {
    ExpressionEvaluator evaluator(frame, symbol_table, db, graph_view_);
    auto lower_val = lower_bound_ ? lower_bound_->value()->Accept(evaluator)
                                  : TypedValue::Null;
    auto upper_val = upper_bound_ ? upper_bound_->value()->Accept(evaluator)
                                  : TypedValue::Null;
    return iter::filter(
        [this, lower_val, upper_val, is_less](const VertexAccessor &vertex) {
          TypedValue value = vertex.PropsAt(property_);
          debug_assert(!value.IsNull(), "Unexpected property with Null value");
          if (lower_bound_ && is_less(value, lower_val, lower_bound_->type()))
            return false;
          if (upper_bound_ && is_less(upper_val, value, upper_bound_->type()))
            return false;
          return true;
        },
        db.vertices(label_, property_, graph_view_ == GraphView::NEW));
  };
  return std::make_unique<ScanAllCursor<decltype(vertices)>>(
      output_symbol_, input_->MakeCursor(db), std::move(vertices), db);
}

ScanAllByLabelPropertyValue::ScanAllByLabelPropertyValue(
    const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
    GraphDbTypes::Label label, GraphDbTypes::Property property,
    Expression *expression, GraphView graph_view)
    : ScanAll(input, output_symbol, graph_view),
      label_(label),
      property_(property),
      expression_(expression) {
  debug_assert(expression, "Expression is not optional.");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyValue)

class ScanAllByLabelPropertyValueCursor : public Cursor {
 public:
  ScanAllByLabelPropertyValueCursor(const ScanAllByLabelPropertyValue &self,
                                    GraphDbAccessor &db)
      : self_(self), db_(db), input_cursor_(self_.input()->MakeCursor(db_)) {}

  bool Pull(Frame &frame, const SymbolTable &symbol_table) override {
    if (db_.should_abort()) throw HintedAbortError();
    if (!vertices_ || vertices_it_.value() == vertices_.value().end()) {
      if (!input_cursor_->Pull(frame, symbol_table)) return false;
      ExpressionEvaluator evaluator(frame, symbol_table, db_,
                                    self_.graph_view());
      TypedValue value = self_.expression()->Accept(evaluator);
      if (value.IsNull()) return Pull(frame, symbol_table);
      try {
        vertices_.emplace(db_.vertices(self_.label(), self_.property(), value,
                                       self_.graph_view() == GraphView::NEW));
      } catch (const TypedValueException &) {
        throw QueryRuntimeException("'{}' cannot be used as a property value.",
                                    value.type());
      }
      vertices_it_.emplace(vertices_.value().begin());
    }

    // if vertices_ is empty then we are done even though we have just
    // reinitialized vertices_it_
    if (vertices_it_.value() == vertices_.value().end()) return false;

    frame[self_.output_symbol()] = *vertices_it_.value()++;
    return true;
  }

  void Reset() override {
    input_cursor_->Reset();
    vertices_ = std::experimental::nullopt;
    vertices_it_ = std::experimental::nullopt;
  }

 private:
  const ScanAllByLabelPropertyValue &self_;
  GraphDbAccessor &db_;
  const std::unique_ptr<Cursor> input_cursor_;
  std::experimental::optional<decltype(
      db_.vertices(self_.label(), self_.property(), TypedValue::Null, false))>
      vertices_;
  std::experimental::optional<decltype(vertices_.value().begin())> vertices_it_;
};

std::unique_ptr<Cursor> ScanAllByLabelPropertyValue::MakeCursor(
    GraphDbAccessor &db) {
  return std::make_unique<ScanAllByLabelPropertyValueCursor>(*this, db);
}

Expand::Expand(Symbol node_symbol, Symbol edge_symbol,
               EdgeAtom::Direction direction,
               const std::shared_ptr<LogicalOperator> &input,
               Symbol input_symbol, bool existing_node, bool existing_edge,
               GraphView graph_view)
    : node_symbol_(node_symbol),
      edge_symbol_(edge_symbol),
      direction_(direction),
      input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      existing_node_(existing_node),
      existing_edge_(existing_edge),
      graph_view_(graph_view) {}

ACCEPT_WITH_INPUT(Expand)

std::unique_ptr<Cursor> Expand::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<ExpandCursor>(*this, db);
}

Expand::ExpandCursor::ExpandCursor(const Expand &self, GraphDbAccessor &db)
    : self_(self), input_cursor_(self.input_->MakeCursor(db)), db_(db) {}

bool Expand::ExpandCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  while (true) {
    if (db_.should_abort()) throw HintedAbortError();
    // attempt to get a value from the incoming edges
    if (in_edges_ && *in_edges_it_ != in_edges_->end()) {
      EdgeAccessor edge = *(*in_edges_it_)++;
      if (HandleExistingEdge(edge, frame, symbol_table) &&
          PullNode(edge, EdgeAtom::Direction::IN, frame, symbol_table))
        return true;
      else
        continue;
    }

    // attempt to get a value from the outgoing edges
    if (out_edges_ && *out_edges_it_ != out_edges_->end()) {
      EdgeAccessor edge = *(*out_edges_it_)++;
      // when expanding in EdgeAtom::Direction::BOTH directions
      // we should do only one expansion for cycles, and it was
      // already done in the block above
      if (self_.direction_ == EdgeAtom::Direction::BOTH && edge.is_cycle())
        continue;
      if (HandleExistingEdge(edge, frame, symbol_table) &&
          PullNode(edge, EdgeAtom::Direction::OUT, frame, symbol_table))
        return true;
      else
        continue;
    }

    // if we are here, either the edges have not been initialized,
    // or they have been exhausted. attempt to initialize the edges,
    // if the input is exhausted
    if (!InitEdges(frame, symbol_table)) return false;

    // we have re-initialized the edges, continue with the loop
  }
}

void Expand::ExpandCursor::Reset() {
  input_cursor_->Reset();
  in_edges_.release();
  in_edges_it_.release();
  out_edges_.release();
  out_edges_it_.release();
}

bool Expand::ExpandCursor::InitEdges(Frame &frame,
                                     const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  TypedValue &vertex_value = frame[self_.input_symbol_];
  // Vertex could be null if it is created by a failed optional match, in such a
  // case we should stop expanding.
  if (vertex_value.IsNull()) return false;
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &vertex = vertex_value.Value<VertexAccessor>();
  // Switch the expansion origin vertex to the desired state.
  switch (self_.graph_view_) {
    case GraphView::NEW:
      vertex.SwitchNew();
      break;
    case GraphView::OLD:
      vertex.SwitchOld();
      break;
    case GraphView::AS_IS:
      break;
  }

  auto direction = self_.direction_;
  if (direction == EdgeAtom::Direction::IN ||
      direction == EdgeAtom::Direction::BOTH) {
    in_edges_ = std::make_unique<InEdgeT>(vertex.in());
    in_edges_it_ = std::make_unique<InEdgeIteratorT>(in_edges_->begin());
  }

  if (direction == EdgeAtom::Direction::OUT ||
      direction == EdgeAtom::Direction::BOTH) {
    out_edges_ = std::make_unique<InEdgeT>(vertex.out());
    out_edges_it_ = std::make_unique<InEdgeIteratorT>(out_edges_->begin());
  }

  // TODO add support for Front and Back expansion (when QueryPlanner
  // will need it). For now only Back expansion (left to right) is
  // supported
  // TODO add support for named paths

  return true;
}

bool Expand::ExpandCursor::HandleExistingEdge(const EdgeAccessor &new_edge,
                                              Frame &frame,
                                              const SymbolTable &symbol_table) {
  if (self_.existing_edge_) {
    TypedValue &old_edge_value = frame[self_.edge_symbol_];
    // old_edge_value may be Null when using optional matching
    if (old_edge_value.IsNull()) return false;
    ExpectType(self_.edge_symbol_, old_edge_value, TypedValue::Type::Edge);
    return old_edge_value.Value<EdgeAccessor>() == new_edge;
  } else {
    // not matching existing, so put the new_edge into the frame and return true
    frame[self_.edge_symbol_] = new_edge;
    return true;
  }
}

bool Expand::ExpandCursor::PullNode(const EdgeAccessor &new_edge,
                                    EdgeAtom::Direction direction, Frame &frame,
                                    const SymbolTable &symbol_table) {
  switch (direction) {
    case EdgeAtom::Direction::IN:
      return HandleExistingNode(new_edge.from(), frame, symbol_table);
    case EdgeAtom::Direction::OUT:
      return HandleExistingNode(new_edge.to(), frame, symbol_table);
    case EdgeAtom::Direction::BOTH:
      permanent_fail("Must indicate exact expansion direction here");
  }
}

bool Expand::ExpandCursor::HandleExistingNode(const VertexAccessor new_node,
                                              Frame &frame,
                                              const SymbolTable &symbol_table) {
  if (self_.existing_node_) {
    TypedValue &old_node_value = frame[self_.node_symbol_];
    // old_node_value may be Null when using optional matching
    if (old_node_value.IsNull()) return false;
    ExpectType(self_.node_symbol_, old_node_value, TypedValue::Type::Vertex);
    return old_node_value.Value<VertexAccessor>() == new_node;
  } else {
    // not matching existing, so put the new_node into the frame and return true
    frame[self_.node_symbol_] = new_node;
    return true;
  }
}

Filter::Filter(const std::shared_ptr<LogicalOperator> &input,
               Expression *expression)
    : input_(input ? input : std::make_shared<Once>()),
      expression_(expression) {}

ACCEPT_WITH_INPUT(Filter)

std::unique_ptr<Cursor> Filter::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<FilterCursor>(*this, db);
}

Filter::FilterCursor::FilterCursor(const Filter &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool Filter::FilterCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  // Like all filters, newly set values should not affect filtering of old nodes
  // and edges.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::OLD);
  while (input_cursor_->Pull(frame, symbol_table)) {
    TypedValue result = self_.expression_->Accept(evaluator);
    // Null is treated like false.
    if (result.IsNull()) continue;

    if (result.type() != TypedValue::Type::Bool)
      throw QueryRuntimeException(
          "Filter expression must be a bool or null, but got {}.",
          result.type());
    if (!result.Value<bool>()) continue;
    return true;
  }
  return false;
}

void Filter::FilterCursor::Reset() { input_cursor_->Reset(); }

Produce::Produce(const std::shared_ptr<LogicalOperator> &input,
                 const std::vector<NamedExpression *> named_expressions)
    : input_(input ? input : std::make_shared<Once>()),
      named_expressions_(named_expressions) {}

ACCEPT_WITH_INPUT(Produce)

std::unique_ptr<Cursor> Produce::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<ProduceCursor>(*this, db);
}

std::vector<Symbol> Produce::OutputSymbols(const SymbolTable &symbol_table) {
  std::vector<Symbol> symbols;
  for (const auto &named_expr : named_expressions_) {
    symbols.emplace_back(symbol_table.at(*named_expr));
  }
  return symbols;
}

const std::vector<NamedExpression *> &Produce::named_expressions() {
  return named_expressions_;
}

Produce::ProduceCursor::ProduceCursor(const Produce &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool Produce::ProduceCursor::Pull(Frame &frame,
                                  const SymbolTable &symbol_table) {
  if (input_cursor_->Pull(frame, symbol_table)) {
    // Produce should always yield the latest results.
    ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
    for (auto named_expr : self_.named_expressions_)
      named_expr->Accept(evaluator);
    return true;
  }
  return false;
}

void Produce::ProduceCursor::Reset() { input_cursor_->Reset(); }

Delete::Delete(const std::shared_ptr<LogicalOperator> &input_,
               const std::vector<Expression *> &expressions, bool detach_)
    : input_(input_), expressions_(expressions), detach_(detach_) {}

ACCEPT_WITH_INPUT(Delete)

std::unique_ptr<Cursor> Delete::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<DeleteCursor>(*this, db);
}

Delete::DeleteCursor::DeleteCursor(const Delete &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool Delete::DeleteCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  // Delete should get the latest information, this way it is also possible to
  // delete newly added nodes and edges.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  // collect expressions results so edges can get deleted before vertices
  // this is necessary because an edge that gets deleted could block vertex
  // deletion
  std::vector<TypedValue> expression_results;
  expression_results.reserve(self_.expressions_.size());
  for (Expression *expression : self_.expressions_) {
    expression_results.emplace_back(expression->Accept(evaluator));
  }

  // delete edges first
  for (TypedValue &expression_result : expression_results)
    if (expression_result.type() == TypedValue::Type::Edge)
      db_.remove_edge(expression_result.Value<EdgeAccessor>());

  // delete vertices
  for (TypedValue &expression_result : expression_results)
    switch (expression_result.type()) {
      case TypedValue::Type::Vertex: {
        VertexAccessor &va = expression_result.Value<VertexAccessor>();
        va.SwitchNew();  //  necessary because an edge deletion could have
                         //  updated
        if (self_.detach_)
          db_.detach_remove_vertex(va);
        else if (!db_.remove_vertex(va))
          throw QueryRuntimeException(
              "Failed to remove vertex because of it's existing "
              "connections. Consider using DETACH DELETE.");
        break;
      }

      // skip Edges (already deleted) and Nulls (can occur in optional match)
      case TypedValue::Type::Edge:
      case TypedValue::Type::Null:
        break;
      // check we're not trying to delete anything except vertices and edges
      default:
        throw QueryRuntimeException("Can only delete edges and vertices");
    }

  return true;
}

void Delete::DeleteCursor::Reset() { input_cursor_->Reset(); }

SetProperty::SetProperty(const std::shared_ptr<LogicalOperator> &input,
                         PropertyLookup *lhs, Expression *rhs)
    : input_(input), lhs_(lhs), rhs_(rhs) {}

ACCEPT_WITH_INPUT(SetProperty)

std::unique_ptr<Cursor> SetProperty::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<SetPropertyCursor>(*this, db);
}

SetProperty::SetPropertyCursor::SetPropertyCursor(const SetProperty &self,
                                                  GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool SetProperty::SetPropertyCursor::Pull(Frame &frame,
                                          const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  // Set, just like Create needs to see the latest changes.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  TypedValue lhs = self_.lhs_->expression_->Accept(evaluator);
  TypedValue rhs = self_.rhs_->Accept(evaluator);

  switch (lhs.type()) {
    case TypedValue::Type::Vertex:
      PropsSetChecked(lhs.Value<VertexAccessor>(), self_.lhs_->property_, rhs);
      break;
    case TypedValue::Type::Edge:
      PropsSetChecked(lhs.Value<EdgeAccessor>(), self_.lhs_->property_, rhs);
      break;
    case TypedValue::Type::Null:
      // Skip setting properties on Null (can occur in optional match).
      break;
    default:
      throw QueryRuntimeException(
          "Properties can only be set on Vertices and Edges");
  }
  return true;
}

void SetProperty::SetPropertyCursor::Reset() { input_cursor_->Reset(); }

SetProperties::SetProperties(const std::shared_ptr<LogicalOperator> &input,
                             Symbol input_symbol, Expression *rhs, Op op)
    : input_(input), input_symbol_(input_symbol), rhs_(rhs), op_(op) {}

ACCEPT_WITH_INPUT(SetProperties)

std::unique_ptr<Cursor> SetProperties::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<SetPropertiesCursor>(*this, db);
}

SetProperties::SetPropertiesCursor::SetPropertiesCursor(
    const SetProperties &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool SetProperties::SetPropertiesCursor::Pull(Frame &frame,
                                              const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  TypedValue &lhs = frame[self_.input_symbol_];

  // Set, just like Create needs to see the latest changes.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  TypedValue rhs = self_.rhs_->Accept(evaluator);

  switch (lhs.type()) {
    case TypedValue::Type::Vertex:
      Set(lhs.Value<VertexAccessor>(), rhs);
      break;
    case TypedValue::Type::Edge:
      Set(lhs.Value<EdgeAccessor>(), rhs);
      break;
    case TypedValue::Type::Null:
      // Skip setting properties on Null (can occur in optional match).
      break;
    default:
      throw QueryRuntimeException(
          "Properties can only be set on Vertices and Edges");
  }
  return true;
}

void SetProperties::SetPropertiesCursor::Reset() { input_cursor_->Reset(); }

template <typename TRecordAccessor>
void SetProperties::SetPropertiesCursor::Set(TRecordAccessor &record,
                                             const TypedValue &rhs) const {
  record.SwitchNew();
  if (self_.op_ == Op::REPLACE) record.PropsClear();

  auto set_props = [&record](const auto &properties) {
    for (const auto &kv : properties) record.PropsSet(kv.first, kv.second);
  };

  switch (rhs.type()) {
    case TypedValue::Type::Edge:
      set_props(rhs.Value<EdgeAccessor>().Properties());
      break;
    case TypedValue::Type::Vertex:
      set_props(rhs.Value<VertexAccessor>().Properties());
      break;
    case TypedValue::Type::Map: {
      for (const auto &kv : rhs.Value<std::map<std::string, TypedValue>>())
        PropsSetChecked(record, db_.property(kv.first), kv.second);
      break;
    }
    default:
      throw QueryRuntimeException(
          "Can only set Vertices, Edges and maps as properties");
  }
}

// instantiate the SetProperties function with concrete TRecordAccessor types
template void SetProperties::SetPropertiesCursor::Set(
    RecordAccessor<Vertex> &record, const TypedValue &rhs) const;
template void SetProperties::SetPropertiesCursor::Set(
    RecordAccessor<Edge> &record, const TypedValue &rhs) const;

SetLabels::SetLabels(const std::shared_ptr<LogicalOperator> &input,
                     Symbol input_symbol,
                     const std::vector<GraphDbTypes::Label> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(SetLabels)

std::unique_ptr<Cursor> SetLabels::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<SetLabelsCursor>(*this, db);
}

SetLabels::SetLabelsCursor::SetLabelsCursor(const SetLabels &self,
                                            GraphDbAccessor &db)
    : self_(self), input_cursor_(self.input_->MakeCursor(db)) {}

bool SetLabels::SetLabelsCursor::Pull(Frame &frame,
                                      const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  TypedValue &vertex_value = frame[self_.input_symbol_];
  // Skip setting labels on Null (can occur in optional match).
  if (vertex_value.IsNull()) return true;
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &vertex = vertex_value.Value<VertexAccessor>();
  vertex.SwitchNew();
  for (auto label : self_.labels_) vertex.add_label(label);

  return true;
}

void SetLabels::SetLabelsCursor::Reset() { input_cursor_->Reset(); }

RemoveProperty::RemoveProperty(const std::shared_ptr<LogicalOperator> &input,
                               PropertyLookup *lhs)
    : input_(input), lhs_(lhs) {}

ACCEPT_WITH_INPUT(RemoveProperty)

std::unique_ptr<Cursor> RemoveProperty::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<RemovePropertyCursor>(*this, db);
}

RemoveProperty::RemovePropertyCursor::RemovePropertyCursor(
    const RemoveProperty &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool RemoveProperty::RemovePropertyCursor::Pull(
    Frame &frame, const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  // Remove, just like Delete needs to see the latest changes.
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  TypedValue lhs = self_.lhs_->expression_->Accept(evaluator);

  switch (lhs.type()) {
    case TypedValue::Type::Vertex:
      lhs.Value<VertexAccessor>().PropsErase(self_.lhs_->property_);
      break;
    case TypedValue::Type::Edge:
      lhs.Value<EdgeAccessor>().PropsErase(self_.lhs_->property_);
      break;
    case TypedValue::Type::Null:
      // Skip removing properties on Null (can occur in optional match).
      break;
    default:
      throw QueryRuntimeException(
          "Properties can only be removed on Vertices and Edges");
  }
  return true;
}

void RemoveProperty::RemovePropertyCursor::Reset() { input_cursor_->Reset(); }

RemoveLabels::RemoveLabels(const std::shared_ptr<LogicalOperator> &input,
                           Symbol input_symbol,
                           const std::vector<GraphDbTypes::Label> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(RemoveLabels)

std::unique_ptr<Cursor> RemoveLabels::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<RemoveLabelsCursor>(*this, db);
}

RemoveLabels::RemoveLabelsCursor::RemoveLabelsCursor(const RemoveLabels &self,
                                                     GraphDbAccessor &db)
    : self_(self), input_cursor_(self.input_->MakeCursor(db)) {}

bool RemoveLabels::RemoveLabelsCursor::Pull(Frame &frame,
                                            const SymbolTable &symbol_table) {
  if (!input_cursor_->Pull(frame, symbol_table)) return false;

  TypedValue &vertex_value = frame[self_.input_symbol_];
  // Skip removing labels on Null (can occur in optional match).
  if (vertex_value.IsNull()) return true;
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &vertex = vertex_value.Value<VertexAccessor>();
  vertex.SwitchNew();
  for (auto label : self_.labels_) vertex.remove_label(label);

  return true;
}

void RemoveLabels::RemoveLabelsCursor::Reset() { input_cursor_->Reset(); }

template <typename TAccessor>
ExpandUniquenessFilter<TAccessor>::ExpandUniquenessFilter(
    const std::shared_ptr<LogicalOperator> &input, Symbol expand_symbol,
    const std::vector<Symbol> &previous_symbols)
    : input_(input),
      expand_symbol_(expand_symbol),
      previous_symbols_(previous_symbols) {}

template <typename TAccessor>
ACCEPT_WITH_INPUT(ExpandUniquenessFilter<TAccessor>)

template <typename TAccessor>
std::unique_ptr<Cursor> ExpandUniquenessFilter<TAccessor>::MakeCursor(
    GraphDbAccessor &db) {
  return std::make_unique<ExpandUniquenessFilterCursor>(*this, db);
}

template <typename TAccessor>
ExpandUniquenessFilter<TAccessor>::ExpandUniquenessFilterCursor::
    ExpandUniquenessFilterCursor(const ExpandUniquenessFilter &self,
                                 GraphDbAccessor &db)
    : self_(self), input_cursor_(self.input_->MakeCursor(db)) {}

template <typename TAccessor>
bool ExpandUniquenessFilter<TAccessor>::ExpandUniquenessFilterCursor::Pull(
    Frame &frame, const SymbolTable &symbol_table) {
  auto expansion_ok = [&]() {
    TypedValue &expand_value = frame[self_.expand_symbol_];
    TAccessor &expand_accessor = expand_value.Value<TAccessor>();
    for (const auto &previous_symbol : self_.previous_symbols_) {
      TypedValue &previous_value = frame[previous_symbol];
      // This shouldn't raise a TypedValueException, because the planner makes
      // sure these are all of the expected type. In case they are not, an error
      // should be raised long before this code is executed.
      TAccessor &previous_accessor = previous_value.Value<TAccessor>();
      if (expand_accessor == previous_accessor) return false;
    }
    return true;
  };

  while (input_cursor_->Pull(frame, symbol_table))
    if (expansion_ok()) return true;
  return false;
}

template <typename TAccessor>
void ExpandUniquenessFilter<TAccessor>::ExpandUniquenessFilterCursor::Reset() {
  input_cursor_->Reset();
}

// instantiations of the ExpandUniquenessFilter template class
// we only ever need these two
template class ExpandUniquenessFilter<VertexAccessor>;
template class ExpandUniquenessFilter<EdgeAccessor>;

namespace {

/**
 * Helper function for recursively reconstructing all the accessors in the
 * given TypedValue.
 */
void ReconstructTypedValue(TypedValue &value) {
  switch (value.type()) {
    case TypedValue::Type::Vertex:
      if (!value.Value<VertexAccessor>().Reconstruct())
        throw QueryRuntimeException(
            "Vertex invalid after WITH clause, (most likely deleted by a "
            "preceeding DELETE clause)");
      break;
    case TypedValue::Type::Edge:
      if (!value.Value<VertexAccessor>().Reconstruct())
        throw QueryRuntimeException(
            "Edge invalid after WITH clause, (most likely deleted by a "
            "preceeding DELETE clause)");
      break;
    case TypedValue::Type::List:
      for (TypedValue &inner_value : value.Value<std::vector<TypedValue>>())
        ReconstructTypedValue(inner_value);
      break;
    case TypedValue::Type::Map:
      for (auto &kv : value.Value<std::map<std::string, TypedValue>>())
        ReconstructTypedValue(kv.second);
      break;
    case TypedValue::Type::Path:
      // TODO implement path reconstruct?
      throw utils::NotYetImplemented("path reconstruction");
    default:
      break;
  }
}
}

Accumulate::Accumulate(const std::shared_ptr<LogicalOperator> &input,
                       const std::vector<Symbol> &symbols, bool advance_command)
    : input_(input), symbols_(symbols), advance_command_(advance_command) {}

ACCEPT_WITH_INPUT(Accumulate)

std::unique_ptr<Cursor> Accumulate::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<Accumulate::AccumulateCursor>(*this, db);
}

Accumulate::AccumulateCursor::AccumulateCursor(const Accumulate &self,
                                               GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool Accumulate::AccumulateCursor::Pull(Frame &frame,
                                        const SymbolTable &symbol_table) {
  // cache all the input
  if (!pulled_all_input_) {
    while (input_cursor_->Pull(frame, symbol_table)) {
      cache_.emplace_back();
      auto &row = cache_.back();
      for (const Symbol &symbol : self_.symbols_)
        row.emplace_back(frame[symbol]);
    }
    pulled_all_input_ = true;
    cache_it_ = cache_.begin();

    if (self_.advance_command_) {
      db_.advance_command();
      for (auto &row : cache_)
        for (auto &col : row) ReconstructTypedValue(col);
    }
  }

  if (cache_it_ == cache_.end()) return false;
  auto row_it = (cache_it_++)->begin();
  for (const Symbol &symbol : self_.symbols_) frame[symbol] = *row_it++;
  return true;
}

void Accumulate::AccumulateCursor::Reset() {
  input_cursor_->Reset();
  cache_.clear();
  cache_it_ = cache_.begin();
  pulled_all_input_ = false;
}

Aggregate::Aggregate(const std::shared_ptr<LogicalOperator> &input,
                     const std::vector<Aggregate::Element> &aggregations,
                     const std::vector<Expression *> &group_by,
                     const std::vector<Symbol> &remember)
    : input_(input ? input : std::make_shared<Once>()),
      aggregations_(aggregations),
      group_by_(group_by),
      remember_(remember) {}

ACCEPT_WITH_INPUT(Aggregate)

std::unique_ptr<Cursor> Aggregate::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<AggregateCursor>(*this, db);
}

Aggregate::AggregateCursor::AggregateCursor(Aggregate &self,
                                            GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

namespace {
/** Returns the default TypedValue for an Aggregation operation.
 * This value is valid both for returning when where are no inputs
 * to the aggregation op, and for initializing an aggregation result
 * when there are */
TypedValue DefaultAggregationOpValue(Aggregation::Op op) {
  switch (op) {
    case Aggregation::Op::COUNT:
      return TypedValue(0);
    case Aggregation::Op::SUM:
    case Aggregation::Op::MIN:
    case Aggregation::Op::MAX:
    case Aggregation::Op::AVG:
      return TypedValue::Null;
    case Aggregation::Op::COLLECT:
      return TypedValue(std::vector<TypedValue>());
  }
}
}

bool Aggregate::AggregateCursor::Pull(Frame &frame,
                                      const SymbolTable &symbol_table) {
  if (!pulled_all_input_) {
    ProcessAll(frame, symbol_table);
    pulled_all_input_ = true;
    aggregation_it_ = aggregation_.begin();

    // in case there is no input and no group_bys we need to return true just
    // this once
    if (aggregation_.empty() && self_.group_by_.empty()) {
      // place default aggregation values on the frame
      for (const auto &elem : self_.aggregations_)
        frame[std::get<2>(elem)] = DefaultAggregationOpValue(std::get<1>(elem));
      // place null as remember values on the frame
      for (const Symbol &remember_sym : self_.remember_)
        frame[remember_sym] = TypedValue::Null;
      return true;
    }
  }

  if (aggregation_it_ == aggregation_.end()) return false;

  // place aggregation values on the frame
  auto aggregation_values_it = aggregation_it_->second.values_.begin();
  for (const auto &aggregation_elem : self_.aggregations_)
    frame[std::get<2>(aggregation_elem)] = *aggregation_values_it++;

  // place remember values on the frame
  auto remember_values_it = aggregation_it_->second.remember_.begin();
  for (const Symbol &remember_sym : self_.remember_)
    frame[remember_sym] = *remember_values_it++;

  aggregation_it_++;
  return true;
}

void Aggregate::AggregateCursor::ProcessAll(Frame &frame,
                                            const SymbolTable &symbol_table) {
  ExpressionEvaluator evaluator(frame, symbol_table, db_, GraphView::NEW);
  while (input_cursor_->Pull(frame, symbol_table))
    ProcessOne(frame, symbol_table, evaluator);

  // calculate AVG aggregations (so far they have only been summed)
  for (int pos = 0; pos < static_cast<int>(self_.aggregations_.size()); ++pos) {
    if (std::get<1>(self_.aggregations_[pos]) != Aggregation::Op::AVG) continue;
    for (auto &kv : aggregation_) {
      AggregationValue &agg_value = kv.second;
      int count = agg_value.counts_[pos];
      if (count > 0)
        agg_value.values_[pos] = agg_value.values_[pos] / (double)count;
    }
  }
}

void Aggregate::AggregateCursor::ProcessOne(Frame &frame,
                                            const SymbolTable &symbol_table,
                                            ExpressionEvaluator &evaluator) {
  // create the group-by list of values
  std::list<TypedValue> group_by;
  for (Expression *expression : self_.group_by_) {
    group_by.emplace_back(expression->Accept(evaluator));
  }

  AggregationValue &agg_value = aggregation_[group_by];
  EnsureInitialized(frame, agg_value);
  Update(frame, symbol_table, evaluator, agg_value);
}

void Aggregate::AggregateCursor::EnsureInitialized(
    Frame &frame,
    Aggregate::AggregateCursor::AggregationValue &agg_value) const {
  if (agg_value.values_.size() > 0) return;

  for (const auto &agg_elem : self_.aggregations_)
    agg_value.values_.emplace_back(
        DefaultAggregationOpValue(std::get<1>(agg_elem)));
  agg_value.counts_.resize(self_.aggregations_.size(), 0);

  for (const Symbol &remember_sym : self_.remember_)
    agg_value.remember_.push_back(frame[remember_sym]);
}

void Aggregate::AggregateCursor::Update(
    Frame &, const SymbolTable &, ExpressionEvaluator &evaluator,
    Aggregate::AggregateCursor::AggregationValue &agg_value) {
  debug_assert(
      self_.aggregations_.size() == agg_value.values_.size(),
      "Expected as much AggregationValue.values_ as there are aggregations.");
  debug_assert(
      self_.aggregations_.size() == agg_value.counts_.size(),
      "Expected as much AggregationValue.counts_ as there are aggregations.");

  // we iterate over counts, values and aggregation info at the same time
  auto count_it = agg_value.counts_.begin();
  auto value_it = agg_value.values_.begin();
  auto agg_elem_it = self_.aggregations_.begin();
  for (; count_it < agg_value.counts_.end();
       count_it++, value_it++, agg_elem_it++) {
    // COUNT(*) is the only case where input expression is optional
    // handle it here
    auto input_expr_ptr = std::get<0>(*agg_elem_it);
    if (!input_expr_ptr) {
      *count_it += 1;
      *value_it = *count_it;
      continue;
    }

    TypedValue input_value = input_expr_ptr->Accept(evaluator);

    // Aggregations skip Null input values.
    if (input_value.IsNull()) continue;

    const auto &agg_op = std::get<1>(*agg_elem_it);
    *count_it += 1;
    if (*count_it == 1) {
      // first value, nothing to aggregate. check type, set and continue.
      switch (agg_op) {
        case Aggregation::Op::MIN:
        case Aggregation::Op::MAX:
          *value_it = input_value;
          EnsureOkForMinMax(input_value);
          break;
        case Aggregation::Op::SUM:
        case Aggregation::Op::AVG:
          *value_it = input_value;
          EnsureOkForAvgSum(input_value);
          break;
        case Aggregation::Op::COUNT:
          *value_it = 1;
          break;
        case Aggregation::Op::COLLECT:
          value_it->Value<std::vector<TypedValue>>().push_back(input_value);
          break;
      }
      continue;
    }

    // aggregation of existing values
    switch (agg_op) {
      case Aggregation::Op::COUNT:
        *value_it = *count_it;
        break;
      case Aggregation::Op::MIN: {
        EnsureOkForMinMax(input_value);
        try {
          TypedValue comparison_result = input_value < *value_it;
          // since we skip nulls we either have a valid comparison, or
          // an exception was just thrown above
          // safe to assume a bool TypedValue
          if (comparison_result.Value<bool>()) *value_it = input_value;
        } catch (const TypedValueException &) {
          throw QueryRuntimeException("Unable to get MIN of '{}' and '{}'",
                                      input_value.type(), value_it->type());
        }
        break;
      }
      case Aggregation::Op::MAX: {
        //  all comments as for Op::Min
        EnsureOkForMinMax(input_value);
        try {
          TypedValue comparison_result = input_value > *value_it;
          if (comparison_result.Value<bool>()) *value_it = input_value;
        } catch (const TypedValueException &) {
          throw QueryRuntimeException("Unable to get MAX of '{}' and '{}'",
                                      input_value.type(), value_it->type());
        }
        break;
      }
      case Aggregation::Op::AVG:
      // for averaging we sum first and divide by count once all
      // the input has been processed
      case Aggregation::Op::SUM:
        EnsureOkForAvgSum(input_value);
        *value_it = *value_it + input_value;
        break;
      case Aggregation::Op::COLLECT:
        value_it->Value<std::vector<TypedValue>>().push_back(input_value);
        break;
    }  // end switch over Aggregation::Op enum
  }    // end loop over all aggregations
}

void Aggregate::AggregateCursor::Reset() {
  input_cursor_->Reset();
  aggregation_.clear();
  aggregation_it_ = aggregation_.begin();
  pulled_all_input_ = false;
}

void Aggregate::AggregateCursor::EnsureOkForMinMax(
    const TypedValue &value) const {
  switch (value.type()) {
    case TypedValue::Type::Bool:
    case TypedValue::Type::Int:
    case TypedValue::Type::Double:
    case TypedValue::Type::String:
      return;
    default:
      throw QueryRuntimeException(
          "Only Bool, Int, Double and String values are allowed in "
          "MIN and MAX aggregations");
  }
}
void Aggregate::AggregateCursor::EnsureOkForAvgSum(
    const TypedValue &value) const {
  switch (value.type()) {
    case TypedValue::Type::Int:
    case TypedValue::Type::Double:
      return;
    default:
      throw QueryRuntimeException(
          "Only numeric values allowed in SUM and AVG aggregations");
  }
}

bool TypedValueListEqual::operator()(const std::list<TypedValue> &left,
                                     const std::list<TypedValue> &right) const {
  return std::equal(left.begin(), left.end(), right.begin(),
                    TypedValue::BoolEqual{});
}

Skip::Skip(const std::shared_ptr<LogicalOperator> &input,
           Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Skip)

std::unique_ptr<Cursor> Skip::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<SkipCursor>(*this, db);
}

std::vector<Symbol> Skip::OutputSymbols(const SymbolTable &symbol_table) {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

Skip::SkipCursor::SkipCursor(Skip &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool Skip::SkipCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  while (input_cursor_->Pull(frame, symbol_table)) {
    if (to_skip_ == -1) {
      // first successful pull from the input
      // evaluate the skip expression
      ExpressionEvaluator evaluator(frame, symbol_table, db_);
      TypedValue to_skip = self_.expression_->Accept(evaluator);
      if (to_skip.type() != TypedValue::Type::Int)
        throw QueryRuntimeException("Result of SKIP expression must be an int");

      to_skip_ = to_skip.Value<int64_t>();
      if (to_skip_ < 0)
        throw QueryRuntimeException(
            "Result of SKIP expression must be greater or equal to zero");
    }

    if (skipped_++ < to_skip_) continue;
    return true;
  }
  return false;
}

void Skip::SkipCursor::Reset() {
  input_cursor_->Reset();
  to_skip_ = -1;
  skipped_ = 0;
}

Limit::Limit(const std::shared_ptr<LogicalOperator> &input,
             Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Limit)

std::unique_ptr<Cursor> Limit::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<LimitCursor>(*this, db);
}

std::vector<Symbol> Limit::OutputSymbols(const SymbolTable &symbol_table) {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

Limit::LimitCursor::LimitCursor(Limit &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool Limit::LimitCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  // we need to evaluate the limit expression before the first input Pull
  // because it might be 0 and thereby we shouldn't Pull from input at all
  // we can do this before Pulling from the input because the limit expression
  // is not allowed to contain any identifiers
  if (limit_ == -1) {
    ExpressionEvaluator evaluator(frame, symbol_table, db_);
    TypedValue limit = self_.expression_->Accept(evaluator);
    if (limit.type() != TypedValue::Type::Int)
      throw QueryRuntimeException("Result of LIMIT expression must be an int");

    limit_ = limit.Value<int64_t>();
    if (limit_ < 0)
      throw QueryRuntimeException(
          "Result of LIMIT expression must be greater or equal to zero");
  }

  // check we have not exceeded the limit before pulling
  if (pulled_++ >= limit_) return false;

  return input_cursor_->Pull(frame, symbol_table);
}

void Limit::LimitCursor::Reset() {
  input_cursor_->Reset();
  limit_ = -1;
  pulled_ = 0;
}

OrderBy::OrderBy(const std::shared_ptr<LogicalOperator> &input,
                 const std::vector<std::pair<Ordering, Expression *>> &order_by,
                 const std::vector<Symbol> &output_symbols)
    : input_(input), output_symbols_(output_symbols) {
  // split the order_by vector into two vectors of orderings and expressions
  std::vector<Ordering> ordering;
  ordering.reserve(order_by.size());
  order_by_.reserve(order_by.size());
  for (const auto &ordering_expression_pair : order_by) {
    ordering.emplace_back(ordering_expression_pair.first);
    order_by_.emplace_back(ordering_expression_pair.second);
  }
  compare_ = TypedValueListCompare(ordering);
}

ACCEPT_WITH_INPUT(OrderBy)

std::unique_ptr<Cursor> OrderBy::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<OrderByCursor>(*this, db);
}

std::vector<Symbol> OrderBy::OutputSymbols(const SymbolTable &symbol_table) {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

OrderBy::OrderByCursor::OrderByCursor(OrderBy &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self_.input_->MakeCursor(db)) {}

bool OrderBy::OrderByCursor::Pull(Frame &frame,
                                  const SymbolTable &symbol_table) {
  if (!did_pull_all_) {
    ExpressionEvaluator evaluator(frame, symbol_table, db_);
    while (input_cursor_->Pull(frame, symbol_table)) {
      // collect the order_by elements
      std::list<TypedValue> order_by;
      for (auto expression_ptr : self_.order_by_) {
        order_by.emplace_back(expression_ptr->Accept(evaluator));
      }

      // collect the output elements
      std::list<TypedValue> output;
      for (const Symbol &output_sym : self_.output_symbols_)
        output.emplace_back(frame[output_sym]);

      cache_.emplace_back(order_by, output);
    }

    std::sort(cache_.begin(), cache_.end(),
              [this](const auto &pair1, const auto &pair2) {
                return self_.compare_(pair1.first, pair2.first);
              });

    did_pull_all_ = true;
    cache_it_ = cache_.begin();
  }

  if (cache_it_ == cache_.end()) return false;

  // place the output values on the frame
  debug_assert(self_.output_symbols_.size() == cache_it_->second.size(),
               "Number of values does not match the number of output symbols "
               "in OrderBy");
  auto output_sym_it = self_.output_symbols_.begin();
  for (const TypedValue &output : cache_it_->second)
    frame[*output_sym_it++] = output;

  cache_it_++;
  return true;
}

void OrderBy::OrderByCursor::Reset() {
  input_cursor_->Reset();
  did_pull_all_ = false;
  cache_.clear();
  cache_it_ = cache_.begin();
}

bool OrderBy::TypedValueCompare(const TypedValue &a, const TypedValue &b) {
  // in ordering null comes after everything else
  // at the same time Null is not less that null
  // first deal with Null < Whatever case
  if (a.IsNull()) return false;
  // now deal with NotNull < Null case
  if (b.IsNull()) return true;

  // comparisons are from this point legal only between values of
  // the  same type, or int+float combinations
  if ((a.type() != b.type() && !(a.IsNumeric() && b.IsNumeric())))
    throw QueryRuntimeException(
        "Can't compare value of type {} to value of type {}", a.type(),
        b.type());

  switch (a.type()) {
    case TypedValue::Type::Bool:
      return !a.Value<bool>() && b.Value<bool>();
    case TypedValue::Type::Int:
      if (b.type() == TypedValue::Type::Double)
        return a.Value<int64_t>() < b.Value<double>();
      else
        return a.Value<int64_t>() < b.Value<int64_t>();
    case TypedValue::Type::Double:
      if (b.type() == TypedValue::Type::Int)
        return a.Value<double>() < b.Value<int64_t>();
      else
        return a.Value<double>() < b.Value<double>();
    case TypedValue::Type::String:
      return a.Value<std::string>() < b.Value<std::string>();
    case TypedValue::Type::List:
    case TypedValue::Type::Map:
    case TypedValue::Type::Vertex:
    case TypedValue::Type::Edge:
    case TypedValue::Type::Path:
      throw QueryRuntimeException(
          "Comparison is not defined for values of type {}", a.type());
    default:
      permanent_fail("Unhandled comparison for types");
  }
}

bool OrderBy::TypedValueListCompare::operator()(
    const std::list<TypedValue> &c1, const std::list<TypedValue> &c2) const {
  auto c1_it = c1.begin();
  auto c2_it = c2.begin();
  // ordering is invalid if there are more elements in the collections
  // then there are in the ordering_ vector
  debug_assert(std::distance(c1_it, c1.end()) <= ordering_.size() &&
                   std::distance(c2_it, c2.end()) <= ordering_.size(),
               "Collections contain more elements then there are orderings");

  auto ordering_it = ordering_.begin();
  for (; c1_it != c1.end() && c2_it != c2.end();
       c1_it++, c2_it++, ordering_it++) {
    if (OrderBy::TypedValueCompare(*c1_it, *c2_it))
      return *ordering_it == Ordering::ASC;
    if (OrderBy::TypedValueCompare(*c2_it, *c1_it))
      return *ordering_it == Ordering::DESC;
  }

  // at least one collection is exhausted
  // c1 is less then c2 iff c1 reached the end but c2 didn't
  return (c1_it == c1.end()) && (c2_it != c2.end());
}

Merge::Merge(const std::shared_ptr<LogicalOperator> input,
             const std::shared_ptr<LogicalOperator> merge_match,
             const std::shared_ptr<LogicalOperator> merge_create)
    : input_(input ? input : std::make_shared<Once>()),
      merge_match_(merge_match),
      merge_create_(merge_create) {}

bool Merge::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && merge_match_->Accept(visitor) &&
        merge_create_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

std::unique_ptr<Cursor> Merge::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<MergeCursor>(*this, db);
}

Merge::MergeCursor::MergeCursor(Merge &self, GraphDbAccessor &db)
    : input_cursor_(self.input_->MakeCursor(db)),
      merge_match_cursor_(self.merge_match_->MakeCursor(db)),
      merge_create_cursor_(self.merge_create_->MakeCursor(db)) {}

bool Merge::MergeCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  if (pull_input_) {
    if (input_cursor_->Pull(frame, symbol_table)) {
      // after a successful input from the input
      // reset merge_match (it's expand iterators maintain state)
      // and merge_create (could have a Once at the beginning)
      merge_match_cursor_->Reset();
      merge_create_cursor_->Reset();
    } else
      // input is exhausted, we're done
      return false;
  }

  // pull from the merge_match cursor
  if (merge_match_cursor_->Pull(frame, symbol_table)) {
    // if successful, next Pull from this should not pull_input_
    pull_input_ = false;
    return true;
  } else {
    // failed to Pull from the merge_match cursor
    if (pull_input_) {
      // if we have just now pulled from the input
      // and failed to pull from merge_match, we should create
      __attribute__((unused)) bool merge_create_pull_result =
          merge_create_cursor_->Pull(frame, symbol_table);
      debug_assert(merge_create_pull_result, "MergeCreate must never fail");
      return true;
    }
    // we have exhausted merge_match_cursor_ after 1 or more successful Pulls
    // attempt next input_cursor_ pull
    pull_input_ = true;
    return Pull(frame, symbol_table);
  }
}

void Merge::MergeCursor::Reset() {
  input_cursor_->Reset();
  merge_match_cursor_->Reset();
  merge_create_cursor_->Reset();
  pull_input_ = true;
}

Optional::Optional(const std::shared_ptr<LogicalOperator> &input,
                   const std::shared_ptr<LogicalOperator> &optional,
                   const std::vector<Symbol> &optional_symbols)
    : input_(input ? input : std::make_shared<Once>()),
      optional_(optional),
      optional_symbols_(optional_symbols) {}

bool Optional::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && optional_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

std::unique_ptr<Cursor> Optional::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<OptionalCursor>(*this, db);
}

Optional::OptionalCursor::OptionalCursor(Optional &self, GraphDbAccessor &db)
    : self_(self),
      input_cursor_(self.input_->MakeCursor(db)),
      optional_cursor_(self.optional_->MakeCursor(db)) {}

bool Optional::OptionalCursor::Pull(Frame &frame,
                                    const SymbolTable &symbol_table) {
  if (pull_input_) {
    if (input_cursor_->Pull(frame, symbol_table)) {
      // after a successful input from the input
      // reset optional_ (it's expand iterators maintain state)
      optional_cursor_->Reset();
    } else
      // input is exhausted, we're done
      return false;
  }

  // pull from the optional_ cursor
  if (optional_cursor_->Pull(frame, symbol_table)) {
    // if successful, next Pull from this should not pull_input_
    pull_input_ = false;
    return true;
  } else {
    // failed to Pull from the merge_match cursor
    if (pull_input_) {
      // if we have just now pulled from the input
      // and failed to pull from optional_ so set the
      // optional symbols to Null, ensure next time the
      // input gets pulled and return true
      for (const Symbol &sym : self_.optional_symbols_)
        frame[sym] = TypedValue::Null;
      pull_input_ = true;
      return true;
    }
    // we have exhausted optional_cursor_ after 1 or more successful Pulls
    // attempt next input_cursor_ pull
    pull_input_ = true;
    return Pull(frame, symbol_table);
  }
}

void Optional::OptionalCursor::Reset() {
  input_cursor_->Reset();
  optional_cursor_->Reset();
  pull_input_ = true;
}

Unwind::Unwind(const std::shared_ptr<LogicalOperator> &input,
               Expression *input_expression, Symbol output_symbol)
    : input_(input ? input : std::make_shared<Once>()),
      input_expression_(input_expression),
      output_symbol_(output_symbol) {}

ACCEPT_WITH_INPUT(Unwind)

std::unique_ptr<Cursor> Unwind::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<UnwindCursor>(*this, db);
}

Unwind::UnwindCursor::UnwindCursor(Unwind &self, GraphDbAccessor &db)
    : self_(self), db_(db), input_cursor_(self.input_->MakeCursor(db)) {}

bool Unwind::UnwindCursor::Pull(Frame &frame, const SymbolTable &symbol_table) {
  if (db_.should_abort()) throw HintedAbortError();
  // if we reached the end of our list of values
  // pull from the input
  if (input_value_it_ == input_value_.end()) {
    if (!input_cursor_->Pull(frame, symbol_table)) return false;

    // successful pull from input, initialize value and iterator
    ExpressionEvaluator evaluator(frame, symbol_table, db_);
    TypedValue input_value = self_.input_expression_->Accept(evaluator);
    if (input_value.type() != TypedValue::Type::List)
      throw QueryRuntimeException("UNWIND only accepts list values, got '{}'",
                                  input_value.type());
    input_value_ = input_value.Value<std::vector<TypedValue>>();
    input_value_it_ = input_value_.begin();
  }

  // if we reached the end of our list of values goto back to top
  if (input_value_it_ == input_value_.end()) return Pull(frame, symbol_table);

  frame[self_.output_symbol_] = *input_value_it_++;
  return true;
}

void Unwind::UnwindCursor::Reset() {
  input_cursor_->Reset();
  input_value_.clear();
  input_value_it_ = input_value_.end();
}

Distinct::Distinct(const std::shared_ptr<LogicalOperator> &input,
                   const std::vector<Symbol> &value_symbols)
    : input_(input ? input : std::make_shared<Once>()),
      value_symbols_(value_symbols) {}

ACCEPT_WITH_INPUT(Distinct)

std::unique_ptr<Cursor> Distinct::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<DistinctCursor>(*this, db);
}

std::vector<Symbol> Distinct::OutputSymbols(const SymbolTable &symbol_table) {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

Distinct::DistinctCursor::DistinctCursor(Distinct &self, GraphDbAccessor &db)
    : self_(self), input_cursor_(self.input_->MakeCursor(db)) {}

bool Distinct::DistinctCursor::Pull(Frame &frame,
                                    const SymbolTable &symbol_table) {
  while (true) {
    if (!input_cursor_->Pull(frame, symbol_table)) return false;

    std::list<TypedValue> row;
    for (const auto &symbol : self_.value_symbols_)
      row.emplace_back(frame[symbol]);
    if (seen_rows_.insert(std::move(row)).second) return true;
  }
}

void Distinct::DistinctCursor::Reset() {
  input_cursor_->Reset();
  seen_rows_.clear();
}

CreateIndex::CreateIndex(GraphDbTypes::Label label,
                         GraphDbTypes::Property property)
    : label_(label), property_(property) {}

bool CreateIndex::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  return visitor.Visit(*this);
}

class CreateIndexCursor : public Cursor {
 public:
  CreateIndexCursor(CreateIndex &self, GraphDbAccessor &db)
      : self_(self), db_(db) {}

  bool Pull(Frame &, const SymbolTable &) override {
    if (did_create_) return false;
    try {
      db_.BuildIndex(self_.label(), self_.property());
    } catch (const IndexExistsException &) {
      // Ignore creating an existing index.
    }
    did_create_ = true;
    return true;
  }

  void Reset() override { did_create_ = false; }

 private:
  const CreateIndex &self_;
  GraphDbAccessor &db_;
  bool did_create_ = false;
};

std::unique_ptr<Cursor> CreateIndex::MakeCursor(GraphDbAccessor &db) {
  return std::make_unique<CreateIndexCursor>(*this, db);
}

}  // namespace query::plan
