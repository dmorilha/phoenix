#include <functional>
#include <iostream>
#include <map>
#include <type_traits>

#include <cassert>
#include <cstring>

#include <sqlite3.h>

namespace {
template<class ... A>
void null(A && ... a) { }
}

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
  ~Cursor() { }
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

  int execute(const char *sql) {
    assert(nullptr != connection_);
    char *error = nullptr;
    const int result = sqlite3_exec(connection_, sql, nullptr, nullptr, &error);
    if (nullptr != error) {
      std::cerr << "error " << error << std::endl;
      sqlite3_free(error);
      error = nullptr;
    }
    return result;
  }

  template<std::size_t N>
  constexpr sqlite3_stmt * prepare(const char (&s)[N]) {
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
  int change(S && s, std::initializer_list<ParameterBase*> && parameters) {
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

  std::map<const char *, sqlite3_stmt *> statements_;
  sqlite3 * connection_ = nullptr;
};

int main(int, const char * * argv) {
  Connection connection;

  connection.execute("CREATE TABLE Person (name TEXT, Age INTEGER, Address TEXT);");

  connection.change("INSERT INTO Person (name, age, address) VALUES (:Name, :Age, :Address);", {
    makeParameter(":Name", "Daniel"),
    makeParameter(":Age", 38),
    makeParameter(":Address", "Colombia"),
  });
}
