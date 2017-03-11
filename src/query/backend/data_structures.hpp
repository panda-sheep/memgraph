//
// Copyright 2017 Memgraph
// Created by Florijan Stamenkovic on 08.03.17.
//

#pragma once

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/assert.hpp"

namespace query {

using std::vector;
using std::make_pair;

/**
 * Data structures used by the compiler. Used as support
 * (or full replacement) for the AST in the later phases
 * of compilation (semantic analysis and code generation).
 */
class DataStructures {

public:
  /**
   *  Following are accessors for various query elements
   *  that are identified with a string.
   *
   *  The Get<X>Index(std::string name)
   *  functions look for element of type <X> that has
   *  the given name.
   */

  auto GetVariableIndex(const std::string &name) {
    return GetNamedElement(variables_, name);
  }

  const auto &variables() const { return variables_; }

  auto GetPropertyIndex(const std::string &name) {
    return GetNamedElement(properties_, name);
  }

  const auto &properties() const { return properties_; }

  auto GetLabelIndex(const std::string &name) {
    return GetNamedElement(labels_, name);
  }

  const auto &labels() const { return labels_; }

  auto GetEdgeTypeIndex(const std::string &name) {
    return GetNamedElement(edge_types_, name);
  }

  const auto &edge_types() const { return edge_types_; }

  auto GetParamIndex(const std::string &name) {
    return GetNamedElement(params_, name);
  }

  const auto &params() const { return params_; }

  /**
   * Following are the expression data structures.
   */

  enum class ExpressionOperand {
    VARIABLE,
    EXPRESSION,
    PROPERTY,
    LABEL,
    EDGE_TYPE,
    PARAMETER
    // TODO add all possible expression operands
  };

  enum class ExpressionOp {
    LOGICAL_OR,
    LOGICAL_XOR,
    LOGICAL_AND,
    LOGICAL_NOT,
    EQ,
    NE,
    LT,
    GT,
    LE,
    GE,
    ADDITION,
    SUBTRACTION,
    MULTIPLICATION,
    DIVISION,
    MODULO,
    UNARY_MINUS,
    UNARY_PLUS,
    VARIABLE, // turns a variable into an expression
    PROPERTY_GETTER,
    LITERAL,
    PARAMETER
    // TODO add all expression ops
  };

  /**
   * An expression parsed from the query. Contains
   * an enum that defines which operation the expression
   * should perform, and a vector of operands. Each
   * operand is defined by it's type and it's index.
   */
  struct Expression {
    const ExpressionOp op_;
    vector<std::pair<ExpressionOperand, int>> operands_;
  };

  /**
   * @param op Expression operation.
   * @return  pair<int, Expression &> that is (index, expression)
   */
  auto AddExpression(ExpressionOp op,
                     std::vector<std::pair<ExpressionOperand, int>> ops = {}) {
    expressions_.emplace_back(Expression{op, ops});
    return make_pair((int)expressions_.size() - 1,
                     std::ref(expressions_.back()));
  }

  const auto &expressions() const { return expressions_; }

  auto &expressions() { return expressions_; }

  /**
   * The following functions are for pattern matching.
   */
  class Node {
  public:
    // node name, -1 if the node is not named
    int variable_{-1};
    vector<int> labels_;
    // pairs of (property_index, expression_index)
    vector<std::pair<int, int>> properties_;

    Node(int variable = -1) : variable_(variable) {}
  };

  class Relationship {
  public:
    enum class Direction { LEFT, RIGHT, BOTH };
    Direction direction_;
    // relationship name, -1 if not named
    int variable_{-1};
    vector<int> types_;
    // pairs of (property_index, expression_index)
    vector<std::pair<int, int>> properties_;

    bool has_range_ = false;
    long long lower_bound = 1LL;
    long long upper_bound = LLONG_MAX;

    Relationship(Direction direction, int variable = -1)
        : direction_(direction), variable_(variable) {}
  };

  struct Pattern {
    int variable_{-1};
    vector<Node> nodes_;
    vector<Relationship> relationships_;
  };

  /**
   * @param start_node Starting node of the pattern.
   * @return pair<int, Pattern &> that is (index, new pattern)
   */
  auto AddPattern(const Node &start_node) {
    patterns_.emplace_back();
    auto &added_pattern = patterns_.back();
    added_pattern.nodes_.emplace_back(start_node);
    return make_pair((int)patterns_.size() - 1, std::ref(added_pattern));
  }

  auto &patterns() { return patterns_; }

  const auto &patterns() const { return patterns_; }

  /**
   * Following is a type hierarchy for clauses. All
   * inherit clause and define their own members.
   */
  class Clause {
  public:
    enum class Type {
      MATCH,
      UNWIND,
      MERGE,
      CREATE,
      SET,
      DELETE,
      REMOVE,
      WITH,
      RETURN
      // TODO add all other ones
    };

    const Type type_;

    Clause(Type type) : type_(type) {}

    virtual ~Clause() {}

    /**
     * Returns a reference to this Clause, cast to the desired
     * derived type. Throws std::bad_cast if the conversion is
     * not allowed.
     *
     * @tparam TDerived The type to return.
     * @return A reference to this, cast to TDerived
     */
    template <typename TDerived> TDerived &As() {
      auto *r_val = dynamic_cast<TDerived *>(this);
      if (r_val == nullptr)
        // TODO consider a specialized exception here
        throw std::bad_cast();

      return *r_val;
    }
  };

  /**
   * Returns all the clauses that can then be dynamically
   * cast to their exact type (which is known from clause.type_
   */
  auto &clauses() { return clauses_; }

  const auto &clauses() const { return clauses_; }

  class Match : public Clause {
  public:
    int where_expression_{-1};
    // indices of patterns in this match
    vector<int> patterns_;

    Match() : Clause(Type::MATCH) {}
  };

  /**
   * Creates and adds a new Match clause and returns it's
   * index and a ref to it
   */
  auto AddMatch() {
    clauses_.emplace_back(std::make_unique<Match>(Match()));
    return make_pair((int)clauses_.size() - 1,
                     std::ref(clauses_.back()->As<Match>()));
  }

  class Return : public Clause {
  public:
    // if the return clause contains '*' at it's start
    bool return_all_;
    // vector of tuples (expression, expr_text, variable)
    // where variable is -1 if there is no AS
    vector<std::tuple<int, std::string, int>> expressions_;

    Return(bool return_all) : Clause(Type::RETURN), return_all_(return_all) {}
  };

  /**
   * Creates and adds a new Return clause and returns it's
   * index and a ref to it
   */
  auto AddReturn(bool return_all) {
    clauses_.emplace_back(std::make_unique<Return>(Return(return_all)));
    return make_pair((int)clauses_.size() - 1,
                     std::ref(clauses_.back()->As<Return>()));
  }

  /** Convenience functions for getting clauses of a specific type */
  template <typename TDerived> auto ClausesOfType(Clause::Type type) const {
    vector<std::reference_wrapper<TDerived>> r_val;
    for (auto &clause_ptr : clauses())
      if (clause_ptr->type_ == type)
        r_val.push_back(std::ref(clause_ptr->As<TDerived>()));

    return r_val;
  }

  auto Matches() const { return ClausesOfType<Match>(Clause::Type::MATCH); }
  auto Returns() const { return ClausesOfType<Return>(Clause::Type::RETURN); }

private:
  vector<std::string> variables_;
  vector<std::string> properties_;
  vector<std::string> labels_;
  vector<std::string> edge_types_;
  vector<std::string> params_;
  vector<Expression> expressions_;
  vector<Pattern> patterns_;
  vector<std::unique_ptr<Clause>> clauses_;

  /**
   * Helper function for getting a named element.
   *
   * @param collection The collection in which to search.
   * @param name Name to look for / add.
   * @return  Index of the NamedElement with the given name
   *    (found existing or added new).
   */
  int GetNamedElement(vector<std::string> &collection,
                      const std::string &name) {
    for (int i = 0; i < (int)collection.size(); ++i)
      if (collection[i] == name)
        return i;

    collection.emplace_back(name);
    return (int)collection.size() - 1;
  }
};
};
