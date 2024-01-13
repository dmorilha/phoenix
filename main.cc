#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

#include <cassert>
#include <cstring>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <sqlite3.h>

#include "database.h"

namespace database {
  struct Pool;

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

    int bind(sqlite3_stmt * statement, int index, const std::string & value) {
      char * const dup = strdup(value.c_str());
      return sqlite3_bind_text(statement, index, dup, value.size(), nullptr);
    }

    virtual ~ParameterBase() = default;
    virtual int bind(sqlite3_stmt *) = 0;
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

    Connection(const char * const path) {
      assert(nullptr != path);
      sqlite3_open(path, &connection_);
    }

    int execute(const char * sql) {
      assert(nullptr != connection_);
      char * error = nullptr;
      const int result = sqlite3_exec(connection_, sql, nullptr, nullptr, &error);
      if (nullptr != error) {
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

    std::map<const char *, sqlite3_stmt *> statements_;
    sqlite3 * connection_ = nullptr;
  };

  struct ConnectionProxy {
    ~ConnectionProxy() {
      if (nullptr != connection_) {
        assert(nullptr != pool_);
        pool_->release(connection_);
      }
    }

    ConnectionProxy(Connection * connection, Pool * pool) : connection_(connection), pool_(pool) { }

    ConnectionProxy(ConnectionProxy && other) {
      pool_ = other.pool_;
      other.pool_ = nullptr;
      connection_ = other.connection_;
      other.connection_ = nullptr;
    }

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

  Pool::~Pool() {
    for (auto & item : connections_) {
      delete item.first;
    }
    connections_.clear();
  }

  Pool::Pool(const char * path, const size_t size = 4) :
    path_(path), size_(size), available_(0) { }

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
}

using DatabasePool = std::shared_ptr<database::Pool>;

namespace http {
  namespace beast = boost::beast;
  namespace asio = boost::asio;
  using tcp = boost::asio::ip::tcp;

  struct SessionBase : public std::enable_shared_from_this<SessionBase> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    beast::http::request<beast::http::string_body> request_;

    virtual ~SessionBase() = default;

    SessionBase(tcp::socket && socket) :
      stream_(std::move(socket)) { }

    void run() {
      asio::dispatch(stream_.get_executor(),
          std::bind_front(&SessionBase::read, shared_from_this()));
    }

    void read() {
      beast::http::async_read(stream_, buffer_, request_,
          std::bind_front(&SessionBase::on_read, shared_from_this()));
    }

    virtual void on_request() = 0;

    void on_read(beast::error_code error_code, std::size_t bytes_transferred) {
      static const bool keep_alive = true;

      if (beast::http::error::end_of_stream == error_code) {
        close();
      }

      on_request();
    }

    void on_write(bool keep_alive, beast::error_code error_code, std::size_t bytes_transferred) {
      if (error_code) { }
      close();
    }

    void close() {
      beast::error_code error_code;
      stream_.socket().shutdown(tcp::socket::shutdown_send, error_code);
    }
  };

  struct Listener : public std::enable_shared_from_this<Listener> {
    Listener(asio::io_context & io_context) :
      io_context_(io_context),
      acceptor_(asio::make_strand(io_context_)) { }

    Listener * listen(tcp::endpoint endpoint) {
      beast::error_code error_code;

      acceptor_.open(endpoint.protocol(), error_code);
      if (error_code) { }

      acceptor_.set_option(asio::socket_base::reuse_address(true), error_code);
      if (error_code) { }

      acceptor_.bind(endpoint, error_code);
      if (error_code) { }

      acceptor_.listen(/* number of simultaneous connections */ 16, error_code);
      if (error_code) { }

      return this;
    }

    template<class Session, class Tuple>
    void accept(Tuple && tuple) {
      acceptor_.async_accept(asio::make_strand(io_context_),
          std::bind_front(&Listener::on_accept<Session, Tuple>, shared_from_this(), std::move(tuple)));
    }

    template<class Session, class Tuple>
    void on_accept(Tuple && tuple, beast::error_code error_code, tcp::socket socket) {
      if (error_code) { return; }
      auto session = std::make_shared<Session>(std::move(socket));
      session->initialize(tuple);
      session->run();
      accept<Session>(std::move(tuple));
    }

    void close() {
      acceptor_.close();
    }

    asio::io_context & io_context_;
    tcp::acceptor acceptor_;
  };
};

struct MySession : public http::SessionBase {
  std::shared_ptr<database::Pool> pool_;

  template <class ... Args>
    MySession(Args && ... args) : SessionBase(std::forward<Args>(args)...) { }

  void initialize(const std::tuple<DatabasePool> & input) {
    pool_ = std::move(std::get<0>(input));
  }

  template <class Body, class Allocator>
  std::string serializeRequest(const boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> & request) {
    std::stringstream buffer;
    buffer << request.method() << " " << request.target() << " " << request.version() << " ; ";
    for (const auto & item : request) {
      buffer << item.name() << ": " << item.value() << " ; ";
    }
    return buffer.str();
  }

  template <class Body, class Allocator>
    boost::beast::http::message_generator
    handle_request(boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> && request) {
      assert(nullptr != pool_);

      auto connection = pool_->getConnection().get();

      connection.change("INSERT INTO Request (request) VALUES (:request);",
          database::ParameterList{database::makeParameter(":request", serializeRequest(request))});

      auto table = connection.read("SELECT * FROM Request;", database::ParameterList{}, [](database::Cursor && cursor) {
          std::stringstream buffer;

          const int columns = cursor.columns();

          for (int i = 0; i < columns; ++i) {
            std::string columnName = cursor.name(i);
            columnName += " (";
            columnName += cursor.type(i);
            columnName += ')';
            columnName += " | ";
            if (i + 1 < columns)
              columnName += ' ';
            buffer << columnName.c_str();
          }

          const size_t header_size = buffer.str().size();
          buffer << std::endl << std::string(header_size, '-') << std::endl;

          while (cursor.step()) {
            for (int i = 0; i < columns; ++i)
              buffer << cursor.text(i);
            buffer << std::endl;
          }

          return buffer.str();
      });

      boost::beast::error_code error_code;
      boost::beast::http::file_body::value_type body;

      boost::beast::http::response<boost::beast::http::string_body> response(
          boost::beast::http::status::ok, request.version());

      response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response.set(boost::beast::http::field::content_type, "text/plain");
      response.keep_alive(request.keep_alive());
      std::string content = "(empty)";
      if (table) {
        content = *table;
      }
      response.content_length(content.size());
      response.body() = content;
      response.prepare_payload();

      return response;
    }

  void on_request() {
    boost::beast::async_write(stream_,
        handle_request(std::move(request_)),
        std::bind_front(&SessionBase::on_write, shared_from_this(), false));
  }
};


int main(int, const char * * argv) {
  auto pool = DatabasePool(new database::Pool("database.sql"));
  auto connection = pool->getConnection().get();

  connection.execute("CREATE TABLE IF NOT EXISTS Request (request TEXT);");

  http::asio::io_context io_context;

  auto listener = std::make_shared<http::Listener>(io_context);

  listener->listen(http::tcp::endpoint(http::asio::ip::make_address("127.0.0.1"), 8080))
    ->accept<MySession>(std::make_tuple(pool));

  const size_t THREAD_COUNT = 4;
  std::vector<std::thread> threads;
  threads.reserve(THREAD_COUNT);
  for (auto i = THREAD_COUNT - 1; i > 0; --i) {
    threads.emplace_back([&io_context]{
      io_context.run();
    });
  }

  boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
  signals.async_wait([&io_context, &listener](const std::error_code error_code, const int signal){
    listener->close();
    io_context.stop();
  });

  io_context.run();

  for (auto & thread : threads) {
    thread.join();
  }

  return 0;
}
