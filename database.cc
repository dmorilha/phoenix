#include <cstring>

#include "database.h"

namespace database {
int ParameterBase::bind(sqlite3_stmt * statement, int index, double value) {
  return sqlite3_bind_double(statement, index, value);
}

int ParameterBase::bind(sqlite3_stmt * statement, int index, long long value) {
  return sqlite3_bind_int64(statement, index, value);
}

int ParameterBase::bind(sqlite3_stmt * statement, int index, int value) {
  return sqlite3_bind_int(statement, index, value);
}

int ParameterBase::bind(sqlite3_stmt * statement, int index, char * value, int size) {
  return sqlite3_bind_text(statement, index, value, size, nullptr);
}

int ParameterBase::bind(sqlite3_stmt * statement, int index, const std::string & value) {
  char * const dup = strdup(value.c_str());
  return sqlite3_bind_text(statement, index, dup, value.size(), nullptr);
}

int Cursor::columns() const {
  assert(nullptr != statement_);
  return sqlite3_column_count(statement_);
}

bool Cursor::step() const {
  assert(nullptr != statement_);
  return sqlite3_step(statement_) == SQLITE_ROW;
}

double Cursor::real(int index) const {
  assert(nullptr != statement_);
  return sqlite3_column_double(statement_, index);
}

int Cursor::integer(int index) const {
  assert(nullptr != statement_);
  return sqlite3_column_int(statement_, index);
}

const char * Cursor::name(int index) const {
  assert(nullptr != statement_);
  return sqlite3_column_name(statement_, index);
}

const char * Cursor::text(int index, const char * empty) const {
  assert(nullptr != statement_);
  const auto text = reinterpret_cast<const char *>(sqlite3_column_text(statement_, index));
  return nullptr != text ? text : empty;
}

const char * Cursor::type(int index, const char * empty) const {
  assert(nullptr != statement_);
  const auto type = sqlite3_column_decltype(statement_, index);
  return nullptr != type ? type : empty;
}

Connection::~Connection() {
  if (nullptr != connection_) {
    for (auto & [_, statement] : statements_) {
      sqlite3_finalize(statement);
    }
    sqlite3_close(connection_);
    connection_ = nullptr;
  }
}

Connection::Connection(const char * const path) {
  assert(nullptr != path);
  sqlite3_open(path, &connection_);
}

int Connection::execute(const char * sql) {
  assert(nullptr != connection_);
  char * error = nullptr;
  const int result = sqlite3_exec(connection_, sql, nullptr, nullptr, &error);
  if (nullptr != error) {
    sqlite3_free(error);
    error = nullptr;
  }
  return result;
}

sqlite3_stmt * Connection::prepare(const std::string & s) {
  static std::hash<std::string> hash;
  const std::size_t index = hash(s);
  sqlite3_stmt * statement = statements_[index];
  if (nullptr == statement) {
    assert(nullptr != connection_);
    const auto result = sqlite3_prepare_v2(connection_, s.c_str(), s.size(), &statement, nullptr);
    statements_[index] = statement;
  }
  return statement;
}

Pool::~Pool() {
  for (auto & item : connections_) {
    delete item.first;
  }
  connections_.clear();
}

// it should provide with a timeout parameter.
std::future<ConnectionProxy> Pool::getConnection() {
  std::unique_lock<std::mutex> lock(mutex_);
  std::promise<ConnectionProxy> promise;
  if (size_ > connections_.size()) {
    auto * const connection = new Connection(path_.c_str());
    connections_.insert({connection, false});
    promise.set_value(ConnectionProxy(new Connection(path_.c_str()), this));
    return promise.get_future();
  }

  while (true) {
    condition_variable_.wait(lock, [this]{ return 0 < available_; });
    for (auto & item : connections_) {
      if (item.second) {
        item.second = false;
        --available_;
        promise.set_value(ConnectionProxy(item.first, this));
        return promise.get_future();
      }
    }
  }
}

void Pool::release(Connection * const connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  connections_[connection] = true;
  ++available_;
  condition_variable_.notify_one();
}

ConnectionProxy::~ConnectionProxy() {
  if (nullptr != connection_) {
    assert(nullptr != pool_);
    pool_->release(connection_);
  }
}


ConnectionProxy::ConnectionProxy(ConnectionProxy && other) {
  pool_ = other.pool_;
  other.pool_ = nullptr;
  connection_ = other.connection_;
  other.connection_ = nullptr;
}
} //end of database namespace
