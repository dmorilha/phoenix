#ifndef DATABASE_H
#define DATABASE_H

#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

#include <cassert>

#include <sqlite3.h>

namespace database {
struct ParameterBase {
  virtual ~ParameterBase() = default;
  virtual int bind(sqlite3_stmt *) = 0;

  int bind(sqlite3_stmt *, int, double);
  int bind(sqlite3_stmt *, int, long long);
  int bind(sqlite3_stmt *, int, int);
  int bind(sqlite3_stmt *, int, char *, int);
  int bind(sqlite3_stmt *, int, const std::string &);

  template<std::size_t N>
  int bind(sqlite3_stmt * statement, int index, const char (&value)[N]) {
    return sqlite3_bind_text(statement, index, value, N, nullptr);
  }
};

using ParameterList = std::initializer_list<ParameterBase *>;

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

struct Connection {
  ~Connection();
  Connection(const char * const);

  int execute(const char *);
  sqlite3_stmt * prepare(const std::string &);

  template<std::size_t N>
  constexpr sqlite3_stmt * prepare(const char (& s)[N]) {
    const std::size_t index = reinterpret_cast<std::size_t>(std::addressof(s));
    sqlite3_stmt * statement = statements_[index];
    if (nullptr == statement) {
      assert(nullptr != connection_);
      const auto result = sqlite3_prepare_v2(connection_, s, strlen(s), &statement, nullptr);
      statements_[index] = statement;
    }
    return statement;
  }


  template<class T>
  constexpr sqlite3_stmt * prepare(T && t) {
    if (std::is_constant_evaluated())
      return prepare(std::forward<T>(t));
    else
      return prepare(std::ref(t));
    return nullptr;
  }

  template<class S>
  int change(S && s, ParameterList && parameters) {
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
  auto read_and_return(S && s, ParameterList && parameters, F && f) {
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
  void read_no_return(S && s, ParameterList && parameters, F && f) {
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
  auto read(S && s, ParameterList && parameters, F && f) {
    using result_type = decltype(std::function{f})::result_type;
    if constexpr (std::is_void_v<result_type>) {
      read_no_return(s, std::move(parameters), f);
    } else {
      return read_and_return<result_type>(s, std::move(parameters), f);
    }
  }

  std::map<const std::size_t, sqlite3_stmt *> statements_;
  sqlite3 * connection_ = nullptr;
};

struct Pool;

struct ConnectionProxy {
  ~ConnectionProxy();

  ConnectionProxy(Connection * connection, Pool * pool) : connection_(connection), pool_(pool) { }

  ConnectionProxy(ConnectionProxy &&);

  ConnectionProxy(const ConnectionProxy &) = delete;
  ConnectionProxy & operator =(const ConnectionProxy &) = delete;
  ConnectionProxy & operator =(ConnectionProxy &&) = delete;

  template<class ... Args>
  auto change(Args && ... args) {
    assert(nullptr != connection_);
    return connection_->change(std::forward<Args>(args)...);
  }

  template<class ... Args>
  auto execute(Args && ... args) {
    assert(nullptr != connection_);
    return connection_->execute(std::forward<Args>(args)...);
  }

  template<class ... Args>
  auto read(Args && ... args) {
    assert(nullptr != connection_);
    return connection_->read(std::forward<Args>(args)...);
  }

  Connection * connection_;
  Pool * pool_;
};

struct Pool {
  friend struct ConnectionProxy;

  ~Pool();
  Pool(const char * path, const size_t size = 4) :
    path_(path), size_(size), available_(0) { }
  std::future<ConnectionProxy> getConnection();
  void release(Connection * const);

  std::string path_;
  const size_t size_;
  size_t available_;
  std::condition_variable condition_variable_;
  std::map<Connection *, bool> connections_;
  std::mutex mutex_;
};

struct Cursor {
  Cursor(sqlite3_stmt * statement) : statement_(statement) { }

  int columns() const;
  bool step() const;
  double real(int) const;
  int integer(int) const;
  const char * name(int) const;

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

  const char * text(int, const char * empty = "") const;
  const char * type(int, const char * empty = "") const;

  sqlite3_stmt * statement_ = nullptr;
};
} // end of database namespace

#endif // DATABASE_H
