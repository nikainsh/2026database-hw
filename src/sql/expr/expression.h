/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "sql/expr/bind_context.h"
#include "sql/expr/tuple.h"
#include "sql/parser/parse_defs.h"

/// 表达式类型，对应 miniob 的 ExprType
enum class ExprType
{
  COLUMN,
  CONST,
  COMPARISON,
  CONJUNCTION,
  AGGREGATE,
};

/// 支持的聚合函数
enum class AggFunc
{
  NONE = 0,
  COUNT,
  SUM,
  AVG,
  MIN,
  MAX,
};

const char *agg_func_to_string(AggFunc func);
AggFunc     agg_func_from_string(const std::string &name);

/**
 * @brief 表达式基类
 * @details 解析阶段先生成未绑定的表达式树，Stmt 阶段调用 bind() 把列名解析成
 *          (table_index, column_index)，执行阶段按元组求值。与 miniob 的
 *          Expression + ExpressionBinder 分工一致。
 */
class Expression
{
public:
  explicit Expression(ExprType type) : expr_type_(type) {}
  virtual ~Expression() = default;

  ExprType expr_type() const { return expr_type_; }

  void              set_name(const std::string &name) { name_ = name; }
  const std::string &name() const { return name_; }

  virtual RC bind(const BindContext &context);
  /// 在一行数据上求值
  virtual RC evaluate(const Tuple &tuple, Value &value) const = 0;
  virtual AttrType result_type() const { return AttrType::UNDEFINED; }
  virtual bool     has_aggregate() const { return false; }
  /// EXPLAIN 里展示的样子
  virtual std::string describe() const = 0;

  /// 收集表达式引用到的列，谓词下推与索引选择需要判断“这个条件涉及哪些表”
  virtual void collect_column_refs(std::vector<ColumnRef> &refs) const {}

protected:
  ExprType    expr_type_;
  std::string name_;
};

/// 列引用
class ColumnExpression : public Expression
{
public:
  ColumnExpression(const std::string &table_name, const std::string &column_name);

  RC         bind(const BindContext &context) override;
  RC         evaluate(const Tuple &tuple, Value &value) const override;
  AttrType   result_type() const override { return type_; }
  std::string describe() const override;

  const std::string &table_name() const { return table_name_; }
  const std::string &column_name() const { return column_name_; }
  int                table_index() const { return table_index_; }
  int                column_index() const { return column_index_; }
  ColumnRef          ref() const { return ColumnRef(table_index_, column_index_); }

  void collect_column_refs(std::vector<ColumnRef> &refs) const override { refs.push_back(ref()); }

private:
  std::string table_name_;
  std::string column_name_;
  int         table_index_  = -1;
  int         column_index_ = -1;
  AttrType    type_         = AttrType::UNDEFINED;
};

/// 常量
class ConstExpression : public Expression
{
public:
  explicit ConstExpression(const Value &value);

  RC evaluate(const Tuple &tuple, Value &value) const override;
  RC bind(const BindContext &context) override { return RC::SUCCESS; }
  AttrType result_type() const override { return value_.type(); }
  std::string describe() const override;

  const Value &value() const { return value_; }

private:
  Value value_;
};

/// 比较表达式：left comp right
class ComparisonExpression : public Expression
{
public:
  ComparisonExpression(std::unique_ptr<Expression> left, CompOp comp, std::unique_ptr<Expression> right);

  RC bind(const BindContext &context) override;
  RC evaluate(const Tuple &tuple, Value &value) const override;
  AttrType result_type() const override { return AttrType::BOOLEAN; }
  std::string describe() const override;

  CompOp              comp() const { return comp_; }
  const Expression   *left() const { return left_.get(); }
  const Expression   *right() const { return right_.get(); }
  /// 把常量搬到右边、列搬到左边（索引选择时需要这个规范形式）
  void                normalize_operands();
  bool                is_constant_compare() const;

  void collect_column_refs(std::vector<ColumnRef> &refs) const override
  {
    left_->collect_column_refs(refs);
    right_->collect_column_refs(refs);
  }

private:
  std::unique_ptr<Expression> left_;
  std::unique_ptr<Expression> right_;
  CompOp                      comp_;
};

/// AND / OR
class ConjunctionExpression : public Expression
{
public:
  ConjunctionExpression(std::vector<std::unique_ptr<Expression>> children, bool is_and);

  RC bind(const BindContext &context) override;
  RC evaluate(const Tuple &tuple, Value &value) const override;
  AttrType result_type() const override { return AttrType::BOOLEAN; }
  std::string describe() const override;

  bool                        is_and() const { return is_and_; }
  const std::vector<std::unique_ptr<Expression>> &children() const { return children_; }

  void collect_column_refs(std::vector<ColumnRef> &refs) const override
  {
    for (const auto &child : children_) {
      child->collect_column_refs(refs);
    }
  }

private:
  std::vector<std::unique_ptr<Expression>> children_;
  bool                                     is_and_;
};

/// 聚合函数调用；求值由聚合算子完成，这里只描述“算什么”
class AggregateExpression : public Expression
{
public:
  AggregateExpression(AggFunc func, std::unique_ptr<Expression> arg);

  RC bind(const BindContext &context) override;
  RC evaluate(const Tuple &tuple, Value &value) const override;
  AttrType result_type() const override { return result_type_; }
  bool     has_aggregate() const override { return true; }
  std::string describe() const override;

  AggFunc            func() const { return func_; }
  const Expression  *arg() const { return arg_.get(); }

  void collect_column_refs(std::vector<ColumnRef> &refs) const override
  {
    if (arg_ != nullptr) {
      arg_->collect_column_refs(refs);
    }
  }

private:
  AggFunc                   func_;
  std::unique_ptr<Expression> arg_;  /// nullptr 表示 count(*)
  AttrType                  result_type_ = AttrType::UNDEFINED;
};

/**
 * @brief 聚合累加器：一个聚合函数在一段数据上的中间状态
 */
class AggregateState
{
public:
  RC init(AggFunc func, const Expression *arg, const std::string &display_name);

  /// 对一行数据累加
  RC accumulate(const Tuple &tuple);
  /// 得到最终结果
  RC result(Value &value) const;

  AggFunc          func() const { return func_; }
  const Expression *arg() const { return arg_; }
  const std::string &name() const { return name_; }

private:
  AggFunc          func_  = AggFunc::NONE;
  const Expression *arg_  = nullptr;
  std::string      name_;
  AttrType         arg_type_ = AttrType::UNDEFINED;
  AttrType         result_type_ = AttrType::UNDEFINED;

  int64_t count_ = 0;
  int64_t int_sum_ = 0;
  double  real_sum_ = 0.0;
  Value   extreme_;
  bool    has_extreme_ = false;
};

/// 用列/名信息构造表达式的辅助（解析器与优化器都会用到）
std::unique_ptr<Expression> make_column_expr(const std::string &table_name, const std::string &column_name);
std::unique_ptr<Expression> make_const_expr(const Value &value);
