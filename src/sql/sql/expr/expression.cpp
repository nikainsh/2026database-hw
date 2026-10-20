/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/expr/expression.h"

#include <sstream>

#include "common/log.h"
#include "common/string_util.h"

const char *agg_func_to_string(AggFunc func)
{
  switch (func) {
    case AggFunc::COUNT: return "count";
    case AggFunc::SUM: return "sum";
    case AggFunc::AVG: return "avg";
    case AggFunc::MIN: return "min";
    case AggFunc::MAX: return "max";
    default: return "none";
  }
}

AggFunc agg_func_from_string(const std::string &raw_name)
{
  const std::string name = to_lower(raw_name);
  if (name == "count") return AggFunc::COUNT;
  if (name == "sum") return AggFunc::SUM;
  if (name == "avg") return AggFunc::AVG;
  if (name == "min") return AggFunc::MIN;
  if (name == "max") return AggFunc::MAX;
  return AggFunc::NONE;
}

std::unique_ptr<Expression> make_column_expr(const std::string &table_name, const std::string &column_name)
{
  return std::make_unique<ColumnExpression>(table_name, column_name);
}

std::unique_ptr<Expression> make_const_expr(const Value &value)
{
  return std::make_unique<ConstExpression>(value);
}

// ---------------------------------------------------------------------------
// 类型协调
// ---------------------------------------------------------------------------

/// 两个类型比较时先统一到一个公共类型：整数向 bigint/float 提升，字符串只与字符串比较
static RC common_type(AttrType left, AttrType right, AttrType &result)
{
  if (left == AttrType::UNDEFINED || right == AttrType::UNDEFINED) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  if (left == right) {
    result = left;
    return RC::SUCCESS;
  }
  const bool left_string  = left == AttrType::CHARS;
  const bool right_string = right == AttrType::CHARS;
  if (left_string != right_string) {
    /// 数字与字符串不做隐式比较（miniob 同样要求类型匹配）
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  if (left == AttrType::FLOAT || right == AttrType::FLOAT) {
    result = AttrType::FLOAT;
    return RC::SUCCESS;
  }
  if (left == AttrType::BIGINT || right == AttrType::BIGINT) {
    result = AttrType::BIGINT;
    return RC::SUCCESS;
  }
  if ((left == AttrType::INT || left == AttrType::BOOLEAN) && (right == AttrType::INT || right == AttrType::BOOLEAN)) {
    result = AttrType::INT;
    return RC::SUCCESS;
  }
  return RC::SCHEMA_FIELD_TYPE_MISMATCH;
}

static RC compare_values(const Value &left, const Value &right, int &result)
{
  AttrType common = AttrType::UNDEFINED;
  RC       rc     = common_type(left.type(), right.type(), common);
  if (OB_FAIL(rc)) {
    return rc;
  }
  Value l, r;
  rc = left.cast_to(common, l);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = right.cast_to(common, r);
  if (OB_FAIL(rc)) {
    return rc;
  }
  return l.compare_to(r, result);
}

// ---------------------------------------------------------------------------
// Expression
// ---------------------------------------------------------------------------

RC Expression::bind(const BindContext &context)
{
  if (name_.empty()) {
    name_ = describe();
  }
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// ColumnExpression
// ---------------------------------------------------------------------------

ColumnExpression::ColumnExpression(const std::string &table_name, const std::string &column_name)
    : Expression(ExprType::COLUMN), table_name_(table_name), column_name_(column_name)
{
  name_ = table_name.empty() ? column_name : table_name + "." + column_name;
}

RC ColumnExpression::bind(const BindContext &context)
{
  const BindContext::TableRef *table = context.find_table(table_name_);
  if (table == nullptr) {
    if (table_name_.empty()) {
      LOG_WARN("column '%s' is ambiguous: the query has %d tables", column_name_.c_str(), context.table_count());
    } else {
      LOG_WARN("unknown table '%s'", table_name_.c_str());
    }
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const ColumnMeta *column = table->meta->column(column_name_);
  if (column == nullptr) {
    LOG_WARN("unknown column. table=%s, column=%s", table->table_name.c_str(), column_name_.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  table_name_    = table->table_name;
  table_index_   = table->table_index;
  column_index_  = table->meta->column_index(column->name());
  type_          = column->type();
  name_          = table->table_name + "." + column->name();
  return RC::SUCCESS;
}

RC ColumnExpression::evaluate(const Tuple &tuple, Value &value) const
{
  if (table_index_ < 0) {
    return RC::INTERNAL;  /// 没绑定就求值属于内部错误
  }
  if (!tuple.get(ref(), value)) {
    LOG_WARN("column not found in tuple. column=%s", name_.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  return RC::SUCCESS;
}

std::string ColumnExpression::describe() const
{
  return table_name_.empty() ? column_name_ : table_name_ + "." + column_name_;
}

// ---------------------------------------------------------------------------
// ConstExpression
// ---------------------------------------------------------------------------

ConstExpression::ConstExpression(const Value &value) : Expression(ExprType::CONST), value_(value)
{
  name_ = value.to_string();
}

RC ConstExpression::evaluate(const Tuple &, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

std::string ConstExpression::describe() const
{
  if (value_.type() == AttrType::CHARS) {
    return "'" + value_.get_string() + "'";
  }
  return value_.to_string();
}

// ---------------------------------------------------------------------------
// ComparisonExpression
// ---------------------------------------------------------------------------

ComparisonExpression::ComparisonExpression(std::unique_ptr<Expression> left, CompOp comp,
                                           std::unique_ptr<Expression> right)
    : Expression(ExprType::COMPARISON), left_(std::move(left)), right_(std::move(right)), comp_(comp)
{}

RC ComparisonExpression::bind(const BindContext &context)
{
  RC rc = left_->bind(context);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = right_->bind(context);
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 列与常量比较时，把常量转成列的类型（miniob 的 ExpressionBinder 同样处理）
  auto align = [](std::unique_ptr<Expression> &expr, AttrType target) -> RC {
    if (expr->expr_type() != ExprType::CONST) {
      return RC::SUCCESS;
    }
    ConstExpression *const_expr = static_cast<ConstExpression *>(expr.get());
    Value              casted;
    RC                 rc = const_expr->value().cast_to(target, casted);
    if (OB_FAIL(rc)) {
      LOG_WARN("cannot convert literal '%s' to type %s",
               const_expr->value().to_string().c_str(), attr_type_to_string(target));
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    expr = make_const_expr(casted);
    return RC::SUCCESS;
  };

  if (left_->expr_type() == ExprType::COLUMN && right_->expr_type() == ExprType::CONST) {
    rc = align(right_, left_->result_type());
  } else if (right_->expr_type() == ExprType::COLUMN && left_->expr_type() == ExprType::CONST) {
    rc = align(left_, right_->result_type());
  }
  if (OB_FAIL(rc)) {
    return rc;
  }

  name_ = describe();
  return RC::SUCCESS;
}

void ComparisonExpression::normalize_operands()
{
  /// 让「列 op 常量」成为统一形式，索引选择只需要考虑这一种
  if (left_->expr_type() == ExprType::CONST && right_->expr_type() != ExprType::CONST) {
    std::swap(left_, right_);
    switch (comp_) {
      case CompOp::LESS_THAN: comp_ = CompOp::GREAT_THAN; break;
      case CompOp::LESS_EQUAL: comp_ = CompOp::GREAT_EQUAL; break;
      case CompOp::GREAT_THAN: comp_ = CompOp::LESS_THAN; break;
      case CompOp::GREAT_EQUAL: comp_ = CompOp::LESS_EQUAL; break;
      default: break;
    }
  }
}

bool ComparisonExpression::is_constant_compare() const
{
  return left_->expr_type() == ExprType::CONST && right_->expr_type() == ExprType::CONST;
}

RC ComparisonExpression::evaluate(const Tuple &tuple, Value &value) const
{
  Value left_value, right_value;
  RC    rc = left_->evaluate(tuple, left_value);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = right_->evaluate(tuple, right_value);
  if (OB_FAIL(rc)) {
    return rc;
  }

  int compare = 0;
  rc = compare_values(left_value, right_value, compare);
  if (OB_FAIL(rc)) {
    return rc;
  }

  bool result = false;
  switch (comp_) {
    case CompOp::EQUAL_TO: result = compare == 0; break;
    case CompOp::NOT_EQUAL: result = compare != 0; break;
    case CompOp::LESS_THAN: result = compare < 0; break;
    case CompOp::LESS_EQUAL: result = compare <= 0; break;
    case CompOp::GREAT_THAN: result = compare > 0; break;
    case CompOp::GREAT_EQUAL: result = compare >= 0; break;
    default: return RC::INVALID_ARGUMENT;
  }
  value = Value(result);
  return RC::SUCCESS;
}

std::string ComparisonExpression::describe() const
{
  return left_->describe() + " " + comp_op_to_string(comp_) + " " + right_->describe();
}

// ---------------------------------------------------------------------------
// ConjunctionExpression
// ---------------------------------------------------------------------------

ConjunctionExpression::ConjunctionExpression(std::vector<std::unique_ptr<Expression>> children, bool is_and)
    : Expression(ExprType::CONJUNCTION), children_(std::move(children)), is_and_(is_and)
{}

RC ConjunctionExpression::bind(const BindContext &context)
{
  for (auto &child : children_) {
    RC rc = child->bind(context);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  name_ = describe();
  return RC::SUCCESS;
}

RC ConjunctionExpression::evaluate(const Tuple &tuple, Value &value) const
{
  for (const auto &child : children_) {
    Value child_value;
    RC    rc = child->evaluate(tuple, child_value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (child_value.type() != AttrType::BOOLEAN) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }

    const bool ok = child_value.get_bool();
    if (is_and_ && !ok) {
      value = Value(false);
      return RC::SUCCESS;
    }
    if (!is_and_ && ok) {
      value = Value(true);
      return RC::SUCCESS;
    }
  }
  value = Value(is_and_);
  return RC::SUCCESS;
}

std::string ConjunctionExpression::describe() const
{
  std::ostringstream oss;
  oss << "(";
  for (size_t i = 0; i < children_.size(); ++i) {
    if (i > 0) {
      oss << (is_and_ ? " and " : " or ");
    }
    oss << children_[i]->describe();
  }
  oss << ")";
  return oss.str();
}

// ---------------------------------------------------------------------------
// AggregateExpression
// ---------------------------------------------------------------------------

AggregateExpression::AggregateExpression(AggFunc func, std::unique_ptr<Expression> arg)
    : Expression(ExprType::AGGREGATE), func_(func), arg_(std::move(arg))
{
  name_ = std::string(agg_func_to_string(func_)) + "(" + (arg_ ? arg_->describe() : std::string("*")) + ")";
}

RC AggregateExpression::bind(const BindContext &context)
{
  AttrType arg_type = AttrType::BIGINT;
  if (arg_) {
    RC rc = arg_->bind(context);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (arg_->expr_type() != ExprType::COLUMN) {
      LOG_WARN("aggregate argument must be a column. expr=%s", describe().c_str());
      return RC::UNSUPPORTED;
    }
    arg_type = arg_->result_type();
  }

  switch (func_) {
    case AggFunc::COUNT: result_type_ = AttrType::BIGINT; break;
    case AggFunc::AVG: result_type_ = AttrType::FLOAT; break;
    case AggFunc::SUM:
      result_type_ = arg_type == AttrType::FLOAT ? AttrType::FLOAT : AttrType::BIGINT;
      break;
    default: result_type_ = arg_type; break;
  }

  name_ = describe();
  return RC::SUCCESS;
}

RC AggregateExpression::evaluate(const Tuple &, Value &) const
{
  /// 聚合值由聚合算子算好后放进元组，不会走这条路
  return RC::UNSUPPORTED;
}

std::string AggregateExpression::describe() const
{
  std::string text = agg_func_to_string(func_);
  text += "(";
  text += arg_ ? arg_->describe() : "*";
  text += ")";
  return text;
}

// ---------------------------------------------------------------------------
// AggregateState
// ---------------------------------------------------------------------------

RC AggregateState::init(AggFunc func, const Expression *arg, const std::string &display_name)
{
  func_          = func;
  arg_           = arg;
  name_          = display_name;
  arg_type_      = arg != nullptr ? arg->result_type() : AttrType::BIGINT;
  count_         = 0;
  int_sum_       = 0;
  real_sum_      = 0.0;
  extreme_       = Value();
  has_extreme_   = false;

  switch (func) {
    case AggFunc::COUNT: result_type_ = AttrType::BIGINT; break;
    case AggFunc::AVG: result_type_ = AttrType::FLOAT; break;
    case AggFunc::SUM:
      result_type_ = arg_type_ == AttrType::FLOAT ? AttrType::FLOAT : AttrType::BIGINT;
      break;
    default: result_type_ = arg_type_; break;
  }
  return RC::SUCCESS;
}

RC AggregateState::accumulate(const Tuple &tuple)
{
  if (func_ == AggFunc::COUNT && arg_ == nullptr) {
    ++count_;
    return RC::SUCCESS;
  }
  if (arg_ == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  Value value;
  RC    rc = arg_->evaluate(tuple, value);
  if (OB_FAIL(rc)) {
    return rc;
  }

  ++count_;
  switch (func_) {
    case AggFunc::COUNT: break;
    case AggFunc::SUM: {
      Value staged;
      rc = value.cast_to(result_type_, staged);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (result_type_ == AttrType::FLOAT) {
        real_sum_ += staged.get_float();
      } else {
        int_sum_ += staged.type() == AttrType::FLOAT ? static_cast<int64_t>(staged.get_float()) : staged.get_int64();
      }
      break;
    }
    case AggFunc::AVG: {
      Value staged;
      rc = value.cast_to(AttrType::FLOAT, staged);
      if (OB_FAIL(rc)) {
        return rc;
      }
      real_sum_ += staged.get_float();
      break;
    }
    case AggFunc::MIN:
    case AggFunc::MAX: {
      int compare = 0;
      if (!has_extreme_) {
        extreme_    = value;
        has_extreme_ = true;
      } else {
        rc = compare_values(extreme_, value, compare);
        if (OB_FAIL(rc)) {
          return rc;
        }
        const bool replace = (func_ == AggFunc::MIN && compare > 0) || (func_ == AggFunc::MAX && compare < 0);
        if (replace) {
          extreme_ = value;
        }
      }
      break;
    }
    default: return RC::UNSUPPORTED;
  }
  return RC::SUCCESS;
}

RC AggregateState::result(Value &value) const
{
  switch (func_) {
    case AggFunc::COUNT: value = Value(static_cast<int64_t>(count_)); return RC::SUCCESS;
    case AggFunc::SUM:
      if (count_ == 0) {
        value = result_type_ == AttrType::FLOAT ? Value(0.0f) : Value(static_cast<int64_t>(0));
      } else if (result_type_ == AttrType::FLOAT) {
        value = Value(static_cast<float>(real_sum_));
      } else {
        value = Value(int_sum_);
      }
      return RC::SUCCESS;
    case AggFunc::AVG:
      value = Value(count_ == 0 ? 0.0f : static_cast<float>(real_sum_ / static_cast<double>(count_)));
      return RC::SUCCESS;
    case AggFunc::MIN:
    case AggFunc::MAX:
      if (!has_extreme_) {
        value = Value();
        value.set_type(result_type_);
      } else {
        value = extreme_;
      }
      return RC::SUCCESS;
    default: return RC::UNSUPPORTED;
  }
}
