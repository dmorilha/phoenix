#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <type_traits>

#include <cassert>
#include <cstring>

#include <sqlite3.h>


struct Connection;

struct ParameterBase {
  int bind(sqlite3_stmt * statement, int index, double value) {
    return sqlite3_bind_double(statement, index, value);
  }

  int bind(sqlite3_stmt * statement, int index, long long value) {
    return sqlite3_bind_int64(statement, index, value);
  }

  int bind(sqlite3_stmt * statement, int index, int value) {
    return sqlite3_bind_int(statement, index, value);
  }

  template<std::size_t N>
  int bind(sqlite3_stmt * statement, int index, const char (&value)[N]) {
    return sqlite3_bind_text(statement, index, value, N, nullptr);
  }

  int bind(sqlite3_stmt * statement, int index, char * value, int size) {
    return sqlite3_bind_text(statement, index, value, size, nullptr);
  }

  virtual ~ParameterBase() = default;
  virtual int bind(sqlite3_stmt *) = 0;
};

template<class V>
struct NamedParameter : ParameterBase {
  NamedParameter(const char * n, V v) : name(n), value(v) { }
  virtual int bind(sqlite3_stmt * statement) {
    assert(nullptr != statement);
    int index = sqlite3_bind_parameter_index(statement, name);
    assert(0 < index);
    assert(sqlite3_bind_parameter_count(statement) >= index);
    return ParameterBase::bind(statement, index, value);
  }
  const char * name;
  V value;
};

template<class V>
struct Parameter : ParameterBase {
  Parameter(int i, V v) : index(i), value(v) { }
  virtual int bind(sqlite3_stmt * statement) {
    assert(nullptr != statement);
    assert(0 < index);
    assert(sqlite3_bind_parameter_count(statement) >= index);
    return ParameterBase::bind(statement, index, value);
  }
  int index;
  V value;
};

template<class V>
NamedParameter<V> * makeParameter(const char * name, V && value) {
  return new NamedParameter<V>(name, std::forward<V>(value));
}

template<class V>
Parameter<V> * makeParameter(int index, V && value) {
  return new Parameter<V>(index, std::forward<V>(value));
}

struct Cursor {
  Cursor(sqlite3_stmt * statement) : statement_(statement) { }

  int columns() const {
    assert(nullptr != statement_);
    return sqlite3_column_count(statement_);
  }

  bool step() const {
    assert(nullptr != statement_);
    return sqlite3_step(statement_) == SQLITE_ROW;
  }


  double real(int index) const {
    assert(nullptr != statement_);
    return sqlite3_column_double(statement_, index);
  }

  int integer(int index) const {
    assert(nullptr != statement_);
    return sqlite3_column_int(statement_, index);
  }

  const char * name(int index) const {
    assert(nullptr != statement_);
    return sqlite3_column_name(statement_, index);
  }

#if 0
  const char * origin(const int index, const char * empty = "") const {
    assert(nullptr != statement_);
    const auto origin = sqlite3_column_origin_name(statement_, index);
    return nullptr != origin ? origin : empty;
  }

  const char * database(const int index, const char * empty = "") const {
    assert(nullptr != statement_);
    const auto database = sqlite3_column_database_name(statement_, index);
    return nullptr != database ? database : empty;
  }

  const char * table(const int index, const char * empty = "") const {
    assert(nullptr != statement_);
    const auto table = sqlite3_column_table_name(statement_, index);
    return nullptr != table ? table : empty;
  }
#endif

  const char * text(int index, const char * empty = "") const {
    assert(nullptr != statement_);
    const auto text = reinterpret_cast<const char *>(sqlite3_column_text(statement_, index));
    return nullptr != text ? text : empty;
  }

  const char * type(int index, const char * empty = "") const {
    assert(nullptr != statement_);
    const auto type = sqlite3_column_decltype(statement_, index);
    return nullptr != type ? type : empty;
  }

  sqlite3_stmt * statement_ = nullptr;
};

struct Connection {
  ~Connection() {
    if (nullptr != connection_) {
      for (auto & [_, statement] : statements_) {
        sqlite3_finalize(statement);
      }
      sqlite3_close(connection_);
      connection_ = nullptr;
    }
  }

  Connection() {
    sqlite3_open(":memory:", &connection_);
  }

  int execute(const char * sql) {
    assert(nullptr != connection_);
    char * error = nullptr;
    const int result = sqlite3_exec(connection_, sql, nullptr, nullptr, &error);
    if (nullptr != error) {
      std::cerr << "error " << error << std::endl;
      sqlite3_free(error);
      error = nullptr;
    }
    return result;
  }

  template<std::size_t N>
  constexpr sqlite3_stmt * prepare(const char (& s)[N]) {
    sqlite3_stmt * statement = statements_[s];
    if (nullptr == statement) {
      assert(nullptr != connection_);
      const auto result = sqlite3_prepare_v2(connection_, s, strlen(s), &statement, nullptr);
    }
    return statement;
  }

  template<class T>
  constexpr sqlite3_stmt * prepare(T && t) {
    if (std::is_constant_evaluated())
      return prepare(std::forward<T>(t));
    else
      std::cerr << "don't bother for now..." << std::endl;
      assert(!"don't bother for now...");
    return nullptr;
  }

  template<class S>
  int change(S && s, std::initializer_list<ParameterBase *> && parameters) {
    int result = 0;
    auto statement = prepare(s);
    if (nullptr != statement) {
      for (auto & parameter : parameters) {
        parameter->bind(statement);
        delete parameter;
      }
      result = sqlite3_step(statement);
      sqlite3_reset(statement);
      sqlite3_clear_bindings(statement);
    } else {
      std::cerr << "Unable to parse SQL command..." << std::endl;
    }
    return result;
  }

  template<typename R, class S, typename F>
  auto read_and_return(S && s, std::initializer_list<ParameterBase *> && parameters, F && f) {
    std::optional<R> result;
    auto statement = prepare(s);
    if (nullptr != statement) {
      for (auto & parameter : parameters) {
        parameter->bind(statement);
        delete parameter;
      }
      result = f({statement});
      sqlite3_reset(statement);
      sqlite3_clear_bindings(statement);
    } else {
      std::cerr << "Unable to parse SQL command..." << std::endl;
    }
    return result;
  }

  template<class S, typename F>
  void read_no_return(S && s, std::initializer_list<ParameterBase *> && parameters, F && f) {
    auto statement = prepare(s);
    if (nullptr != statement) {
      for (auto & parameter : parameters) {
        parameter->bind(statement);
        delete parameter;
      }
      f({statement});
      sqlite3_reset(statement);
      sqlite3_clear_bindings(statement);
    } else {
      std::cerr << "Unable to parse SQL command..." << std::endl;
    }
  }

  template<class S, typename F>
  auto read(S && s, std::initializer_list<ParameterBase *> && parameters, F && f) {
    using result_type = decltype(std::function{f})::result_type;
    if constexpr (std::is_void_v<result_type>) {
      read_no_return(s, std::move(parameters), f);
    } else {
      return read_and_return<result_type>(s, std::move(parameters), f);
    }
  }

  std::map<const char *, sqlite3_stmt *> statements_;
  sqlite3 * connection_ = nullptr;
};

int main(int, const char * * argv) {
  Connection connection;

  connection.execute("CREATE TABLE Person (Name TEXT, Age INTEGER, Address TEXT);");

  connection.change("INSERT INTO Person (Name, Age, Address) VALUES (:Name, :Age, :Address);", {
    makeParameter(":Name", "Daniel"),
    makeParameter(":Age", 38),
    makeParameter(":Address", "Colombia"),
  });

  connection.change("INSERT INTO Person (Name, Age, Address) VALUES (:Name, :Age, :Address);", {
    makeParameter(":Name", "Alberto"),
    makeParameter(":Age", 42),
    makeParameter(":Address", "Argentina"),
  });

  auto table = connection.read("SELECT * FROM Person;", {}, [](Cursor && cursor) {
      auto trim = [](std::string value, const size_t size) {
        if (value.size() > size) {
          value.substr(0, size - 3);
          value += "...";
        } else
          value.resize(size, ' ');
        return value;
      };

      std::stringstream buffer;

      const int columns = cursor.columns();
      std::vector<size_t> column_size(columns);

      for (int i = 0; i < columns; ++i) {
        std::string columnName = cursor.name(i);
        columnName += " (";
        columnName += cursor.type(i);
        columnName += ')';
        column_size[i] = columnName.size();
        columnName += " |";
        if (i + 1 < columns)
          columnName += ' ';
        buffer << columnName.c_str();
      }

      const size_t header_size = buffer.str().size();
      buffer << std::endl << std::string(header_size, '-') << std::endl;

      while (cursor.step()) {
        for (int i = 0; i < columns; ++i)
          buffer << trim(cursor.text(i), column_size[i]) << " | ";
        buffer << std::endl;
      }

      return buffer.str();
  });

  if (table) {
    std::cout << *table;
  }
}
