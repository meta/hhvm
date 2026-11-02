<?hh
/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
 */

namespace {

/**
 * An asynchronous MySQL client.
 *
 * This class allows you to asynchronously connect to a MySQL client. You
 * can directly connect to the MySQL client with the `connect()` method; in
 * addition you can use this class in conjunction with
 * `AsyncMysqlConnectionPool` pools by setting the limit of connections on
 * any given pool, and using `AsyncMysqlConnectionPool::connect()`.
 *
 * There is some duplication with this class. If possible, you should directly
 * construct connection pools via `new AsyncMysqlConnectionPool()` and then
 * call `AsyncMysqlConnectionPool::connect()` to connect to the MySQL client
 * using those pools. Here we optionally set pool limits and call `connect()`
 * on this class. `AsyncMysqlConnectionPool` provides more flexibility with
 * other available options, etc.
 *
 * In fact, there is discussion about deprecating `AsyncMysqlClient` all
 * together to avoid having this choice. But, for now, you can use this class
 * for asynchronous connection(s) to a MySQL database.
 *
 * @guide /hack/async/extensions
 * @guide /hack/async/introduction
 */
final class AsyncMysqlClient {

  /**
   * @internal
   *
   * AsyncMysqlClient objects cannot be directly created.
   *
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Sets the connection limit of all connection pools using this client.
   *
   * Use this function to toggle the number of allowed async connections on the
   * pools connecting to MySQL with this current client. For example, if you
   * set the limit to 2, and you try a third connection on the same pool, an
   * `AsyncMysqlConnectException` exception will be thrown.
   *
   * @param $limit - The limit for all pools.
   *
   */
  <<__Native>>
  public static function setPoolsConnectionLimit(int $limit): void;

  /**
   * Begin an async connection to a MySQL instance.
   *
   * Use this to asynchronously connect to a MySQL instance.
   *
   * Normally you would use this to make one asynchronous connection to the
   * MySQL client.
   *
   * If you want to be able to pool up a bunch of connections, you would call
   * `setPoolsConnectionLimit()`, create a default pool of connections with
   * `AsyncMysqlConnectionPool()::__construct()`, which now
   * has that limit set, and then call `AsyncMysqlConnectionPool()::connect()`.
   *
   * @param $host - The hostname to connect to.
   * @param $port - The port to connect to.
   * @param $dbname - The initial database to use when connecting.
   * @param $user - The user to connect as.
   * @param $password - The password to connect with.
   * @param $timeout_micros - Timeout, in microseconds, for the connect; -1 for
   *                          default, 0 for no timeout.
   * @param $ssl_context - Optionally allow the connection to tunnel via SSL.
   * @param $tcp_timeout_micros - Timeout, in microseconds, for the tcp phase of
   *                          connect operation; Default: 0 for no timeout.
   * @param $sni_server_name - SNI hostname to use when connecting via SSL.
   * @param $server_cert_extensions - collection of name of TLS cert extension
                                      names used to validate server cert
   * @param $server_cert_values - collection of accepted values in server cert
   *                              for "server_cert_extension" extension
   *
   * @return - an `Awaitable` representing an `AsyncMysqlConnection`. `await`
   * or `join` this result to obtain the actual connection.
   */
  <<__Native>>
  public static function connect(string $host,
                                 int $port,
                                 string $dbname,
                                 string $user,
                                 string $password,
                                 int $timeout_micros = -1,
                                 ?MySSLContextProvider $ssl_context = null,
                                 int $tcp_timeout_micros = 0,
                                 string $sni_server_name = "",
                                 string $server_cert_extension = "",
                                 string $server_cert_values = "",
                                ): Awaitable<AsyncMysqlConnection>;

  /**
   * Begin an async connection to a MySQL instance.
   *
   * Use this to asynchronously connect to a MySQL instance.
   *
   * Normally you would use this to make one asynchronous connection to the
   * MySQL client.
   *
   * If you want to be able to pool up a bunch of connections, you would call
   * `setPoolsConnectionLimit()`, create a default pool of connections with
   * `AsyncMysqlConnectionPool()::__construct()`, which now
   * has that limit set, and then call `AsyncMysqlConnectionPool()::connect()`.
   *
   * @param $host - The hostname to connect to.
   * @param $port - The port to connect to.
   * @param $dbname - The initial database to use when connecting.
   * @param $user - The user to connect as.
   * @param $password - The password to connect with.
   * @param $connection_options - A set of options used for connection.
   *
   * @return - an `Awaitable` representing an `AsyncMysqlConnection`. `await`
   * or `join` this result to obtain the actual connection.
   */
  <<__Native>>
  public static function connectWithOpts(string $host,
                                        int $port,
                                        string $dbname,
                                        string $user,
                                        string $password,
                                        AsyncMysqlConnectionOptions $conn_opts,
                                        ): Awaitable<AsyncMysqlConnection>;

  /**
   * Begin an async connection and query  to a MySQL instance.
   *
   * Use this to asynchronously connect and query a MySQL instance.
   *
   * Normally you would use this to make one query to the
   * MySQL client.
   *
   * If you want to be able to reuse the connection use connect or
   * connectWithOpts
   *
   * @param $host - The hostname to connect to.
   * @param $port - The port to connect to.
   * @param $dbname - The initial database to use when connecting.
   * @param $user - The user to connect as.
   * @param $password - The password to connect with.
   * @param $connection_options - A set of options used for connection.
   * @param $query_attributes - Query attributes. Empty by default.
   *
   * @return - an `Awaitable` representing the result of your connect and query
   * This is a tuple where the latter contains information about the connection
   * retrieval, and the former has the query results
   */
  <<__Native>>
  public static function connectAndQuery(
                                        vec<string> $queries,
                                        string $host,
                                        int $port,
                                        string $dbname,
                                        string $user,
                                        string $password,
                                        AsyncMysqlConnectionOptions $conn_opts,
                                        dict<string, string> $query_attributes
                                            = dict[],
                                      ): Awaitable<(
                                          AsyncMysqlConnectResult,
                                          /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
                                          Vector<AsyncMysqlQueryResult>
                                      )>;
}

/**
 * An asynchronous MySQL connection pool.
 *
 * This class provides a mechanism to create a pool of connections to a MySQL
 * client that can be utilized and reused as needed.
 *
 * When a client requests a connection from the pool, it may get one that
 * already exists; this avoids the overhead of establishing a new connection.
 *
 * This is the *highly recommended* way to create connections to a MySQL
 * client, as opposed to using the `AsyncMysqlClient` class which does not give
 * you nearly the flexibility. In fact, there is discussion about deprecating
 * the `AsyncMysqlClient` class all together.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
class AsyncMysqlConnectionPool {

  /**
   * Create a pool of connections to access a MySQL client.
   *
   * You can pass this constructor an `array` of options to tweak the behavior
   * of your pool. If you don't want an options, pass an empty `array()`.
   *
   * Here are the keys for that array, and all values are `int`, except for
   * `expiration_policy`, which is a `string`, and `validate_on_checkout`,
   * which is a `bool`:
   *
   * - `per_key_connection_limit`: The maximum number of connections allowed
   *                               in the pool for a single combination of
   *                               hostname, port, db and username. The default
   *                               is 50.
   * - `pool_connection_limit`: The maximum number of connections allowed in
   *                            the pool. The default is 5000. It is
   *                            interesting to note that this is the option
   *                            that is set when you call
   *                            `AsyncMysqlClient::setPoolsConnectionLimit()`.
   * - `idle_timeout_micros`: The maximum amount of time, in microseconds, that
   *                          a connection is allowed to sit idle in the pool
   *                          before being destroyed. The default is 4 seconds.
   * - `age_timeout_micros`: The maximum age, in microseconds, that a connection
   *                         in the pool will be allowed to reach before being
   *                         destroyed. The default is 60 seconds.
   * - `expiration_policy`: A `string` of either `'IdleTime'` or `'Age'" that
   *                        specifies whether connections in the pool will be
   *                        destroyed based on how long it sits idle or total
   *                        age in the pool. The default is `'Age'`.
   * - `validate_on_checkout`: Unless `false`, a connection taken from the pool
   *                           is discarded instead of returned if it is past
   *                           its expiry or has pending input (a close or
   *                           unrequested data). This does not guarantee the
   *                           next query succeeds. The default is `true`.
   *
   * @param $pool_options - The `array` of options for the connection pool.
   *                        The key to each array element is an option listed
   *                        above, while the value is an `int` or `string`,
   *                        depending on the option.
   */
  <<__Native>>
  public function __construct(darray<string, mixed> $pool_options): void;

  /**
   * Returns statistical information for the current pool.
   *
   * Information provided includes the number of pool connections that were
   * created and destroyed, how many connections were requested, and how many
   * times the pool was hit or missed when creating the connection. The
   * returned `array` keys are:
   *
   * - `created_pool_connections`
   * - `destroyed_pool_connections`
   * - `connections_requested`
   * - `pool_hits`
   * - `pool_misses`
   * - `validation_discards`
   *
   * @return - A string-keyed `array` with the statistical information above.
   */
  <<__Native>>
  public function getPoolStats(): darray<string, mixed>;

  /**
   * Begin an async connection to a MySQL instance.
   *
   * Once you have your pool constructed, you use this method to connect to the
   * MySQL client. The connection pool will either create a new connection or
   * use one of the recently available connections from the pool itself.
   *
   * @param $host - The hostname to connect to.
   * @param $port - The port to connect to.
   * @param $dbname - The initial database to use when connecting.
   * @param $user - The user to connect as.
   * @param $password - The password to connect with.
   * @param $timeout_micros - Timeout, in microseconds, for the connect; -1
   *                          for default, 0 for no timeout.
   * @param $extra_key - An extra parameter to help the internal connection
   *                     pool infrastructure separate connections even better.
   *                     Usually, the default `""` is fine.
   * @param $ssl_context - Optionally allow the connection to tunnel via SSL.
   * @param $tcp_timeout_micros - Timeout, in microseconds, for the tcp phase of
   *                          connect operation; Default: 0 for no timeout.
   * @param $sni_server_name - SNI hostname to use when connecting via SSL.
   * @param $server_cert_extensions - collection of name of TLS cert extension
                                      names used to validate server cert
   * @param $server_cert_values - collection of accepted values in server cert
   *                              for "server_cert_extension" extension
   *
   * @return - an `Awaitable` representing an `AsyncMysqlConnection`. `await`
   * or `join` this result to obtain the actual connection.
   */
  <<__Native>>
  public function connect(string $host,
                          int $port,
                          string $dbname,
                          string $user,
                          string $password,
                          int $timeout_micros = -1,
                          string $extra_key = "",
                          ?MySSLContextProvider $ssl_context = null,
                          int $tcp_timeout_micros = 0,
                          string $sni_server_name = "",
                          string $server_cert_extensions = "",
                          string $server_cert_values = "",
                         ): Awaitable<AsyncMysqlConnection>;

  <<__Native>>
  public function connectWithOpts(string $host,
                          int $port,
                          string $dbname,
                          string $user,
                          string $password,
                          AsyncMysqlConnectionOptions $conn_options,
                          string $extra_key = "",
                         ): Awaitable<AsyncMysqlConnection>;

  <<__Native>>
  public function connectAndQuery(
    vec<string> $queries,
    string $host,
    int $port,
    string $dbname,
    string $user,
    string $password,
    AsyncMysqlConnectionOptions $conn_opts,
    string $extra_key = "",
    dict<string, string> $query_attributes = dict[],
  ):
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  Awaitable<(AsyncMysqlConnectResult, Vector<AsyncMysqlQueryResult>)>;
}

/**
 * An active connection to a MySQL instance.
 *
 * When you call `connect()` with a connection provided by the pool established
 * with `AsyncMysqlConnectionPool`, you are returned this connection object to
 * actual do real work with the MySQL database, with the primary work being
 * querying the database itself.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlConnection {

  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Begin running an unsafe query on the MySQL database client.
   *
   * If you have a direct query that requires no placeholders, then you can
   * use this method. It takes a raw string query that will be executed as-is.
   *
   * You may want to call `escapeString()` to ensure that any queries out of
   * your direct control are safe.
   *
   * We strongly recommend using `queryf()` instead in all cases, which
   * automatically escapes parameters.
   *
   * @param $query - The query itself.
   * @param $timeout_micros - The maximum time, in microseconds, in which the
   *                          query must be completed; -1 for default, 0 for
   *                          no timeout.
   * @param $query_attributes - Query attributes. Empty by default.
   *
   * @return - an `Awaitable` representing the result of your query. Use
   *           `await` or `join` to get the actual `AsyncMysqlQueryResult`
   *           object.
   */
  <<__Native>>
    public function query(string $query,
                  int $timeout_micros = -1,
                  dict<string, string> $query_attributes = dict[],
                  ): Awaitable<AsyncMysqlQueryResult>;

  /**
   * Execute a query with placeholders and parameters.
   *
   * This is probably the more common of the two query methods, given its
   * flexibility and automatic escaping in most string cases.
   *
   * For example:
   *   `queryf("SELECT %C FROM %T WHERE %C %=s", $col1, $table, $col2, $value);`
   *
   * The supported placeholders are:
   *  - `%T`   table name
   *  - `%C`   column name
   *  - `%s`   nullable string (will be escaped)
   *  - `%d`   integer
   *  - `%f`   float
   *  - `%=s`  nullable string comparison - expands to either:
   *             `= 'escaped_string'`
   *             `IS NULL`
   *  - `%=d`  nullable integer comparison
   *  - `%=f`  nullable float comparison
   *  - `%Q`   raw SQL query. The typechecker intentionally does not recognize
   *           this, however, you can use it in combination with // UNSAFE
   *           if absolutely required. Use this at your own risk as it could
   *           open you up for SQL injection.
   *  - `%Lx`  where `x` is one of `T`, `C`, `s`, `d`, or `f`, represents a list
   *           of table names, column names, nullable strings, integers or
   *           floats, respectively. Pass a `Vector` of values to have it
   *           expanded into a comma-separated list. Parentheses are not
   *           added automatically around the placeholder in the query string,
   *           so be sure to add them if necessary.
   *
   * With the exception of `%Q`, any strings provided will be properly
   * escaped.
   *
   * @param $pattern - The query string with any placeholders.
   * @param $args - The real values for all of the placeholders in your query
   *                string. You must have as many values as you do
   *                placeholders.
   *
   * @return - an `Awaitable` representing the result of your query. Use
   *           `await` or `join` to get the actual `AsyncMysqlQueryResult`
   *           object.
   */
  <<__Native>>
  public function queryf(
    string $pattern,
    mixed ...$args
  ): Awaitable<AsyncMysqlQueryResult>;
  <<__Native>>
  public function queryAsync(
    \HH\Lib\SQL\Query $query,
  ): Awaitable<AsyncMysqlQueryResult>;

  /**
   * Begin running a query with multiple statements.
   *
   * `AsyncMysqlConnection::multiQuery()` is similar to
   * `AsyncMysqlConnection::query()`, except that you can pass an array of
   * `string` queries to run one after the other. Then when you `await` or
   * `join` on the returned `Awaitable`, you will get a `Vector` of
   * `AsyncMysqlQueryResult`, one result for each query.
   *
   * We strongly recommend using multiple calls to `queryf()` instead as it
   * escapes parameters; multiple queries can be executed simultaneously by
   * combining `queryf()` with `HH\Asio\v()`.
   *
   * @param $queries - A `Vector` of queries, with each query being a `string`
   *                    in the array.
   * @param $timeout_micros - The maximum time, in microseconds, in which the
   *                          query must be completed; -1 for default, 0 for
   *                          no timeout.
   * @param $query_attributes - Query attributes. Empty by default.
   *
   * @return - an `Awaitable` representing the result of your multi-query. Use
   *           `await` or `join` to get the actual `Vector` of
   *           `AsyncMysqlQueryResult` objects.
   */
  <<__Native>>
  public function multiQuery(vec<string> $queries,
                      int $timeout_micros = -1,
                      dict<string, string> $query_attributes = dict[],
                      ):
                      /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
                      Awaitable<Vector<AsyncMysqlQueryResult>>;

  /**
   * Escape a string to be safe to include in a raw query.
   *
   * Use this method to ensure your query is safe from, for example, SQL
   * injection if you are not using an API that automatically escapes
   * queries.
   *
   * We strongly recommend using `queryf()` instead, which automatically
   * escapes string parameters.
   *
   * This method is equivalent to PHP's
   * [mysql_real_escape_string()](http://goo.gl/bnxqtE).
   *
   * @param $data - The string to properly escape.
   *
   * @return - The escaped string.
   */
  <<__Native>>
  public function escapeString(string $data): ?string;

  /**
   * Close the current connection.
   */
  <<__Native>>
  public function close(): void;

  /**
   * Checks if the data inside `AsyncMysqlConnection` object is valid. For
   * example, during a timeout in a query, the MySQL connection gets closed.
   *
   * @return - `true` if MySQL resource is valid and can be accessed;
   *           `false` otherwise.
   */
  <<__Native>>
  public function isValid(): bool;



  /**
   * The MySQL server version associated with the current connection.
   *
   * @return - The server version as a `string`.
   */
  <<__Native>>
  public function serverInfo(): ?string;

  /**
   * Returns whether or not the current connection reused the SSL session
   * from another SSL connection. The session is set by MySSLContextProvider.
   * Some cases, the server can deny the session that was set and the handshake
   * will create a new one, in those cases this function will return `false`.
   * If this connections isn't SSL, `false` will be returned as well.
   *
   * @return - `true` if this is a SSL connection and the SSL session was
   *           reused; `false` otherwise.
   */
  <<__Native>>
  public function sslSessionReused(): bool;


  /**
   * Returns whether or not the current connection was established as SSL based
   * on client flag exchanged during handshake.
   *
   * @return - `true` if this is a SSL connection; `false` otherwise
   */
  <<__Native>>
  public function isSSL(): bool;

  /**
   * The number of errors, warnings, and notes returned during execution of
   * the previous SQL statement.
   *
   * @return - The `int` count of errors, warnings, etc.
   */
  <<__Native>>
  public function warningCount(): int;

  /**
   * The hostname associated with the current connection.
   *
   * @return - The hostname as a `string`.
   */
  <<__Native>>
  public function host(): ?string;

  /**
   * The port on which the MySQL instance is running.
   *
   * @return - The port as an `int`.
   */
  <<__Native>>
  public function port(): int;

  /**
   * Sets if the current connection can be recycled without any clean up.
   *
   * By default, the current connection *is* reusable.
   *
   * If a connection in a `AsyncMysqlConnectionPool` is used, but you call
   * `setReusable(false)`, then you will have to create a whole new connection
   * instead of reusing this particular connection.
   *
   * @param $reusable - Pass `true` to make the connection reusable; `false`
   *                    otherwise.
   */
  <<__Native>>
  public function setReusable(bool $reusable): void;

  /**
   * Returns whether or not the current connection is reusable.
   *
   * By default, the current connection is reusable by the pool. But if you call
   * `setResuable(false)`, then the current connection will not be reusable by
   * the connection pool.
   *
   * @return - `true` if the connection is reusable; `false` otherwise.
   */
  <<__Native>>
  public function isReusable(): bool;

  /**
   * Last time a successful activity was made in the current connection, in
   * seconds since epoch.
   *
   * The first successful activity of the current connection is its creation.
   *
   * @return - A `float` representing the number of seconds ago since epoch
   *           that we had successful activity on the current connection.
   */
  <<__Native>>
  public function lastActivityTime(): float;

  /**
   * Returns the `AsyncMysqlConnectResult` for the current connection.
   *
   * An `AsyncMysqlConnectResult` provides information about the timing for
   * creating the current connection.
   *
   * @return - An `AsyncMysqlConnectResult` object or `null` if the
   *           `AsyncMysqlConnection` was not created in the MySQL client.
   */
  <<__Native>>
  public function connectResult(): ?AsyncMysqlConnectResult;

  /**
   * Returns Common Name attribute of the TLS certificate presented
   * by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a string containing Common Name value from the server
   *           certificate presented by MySQL.
   */
  <<__Native>>
  public function getSslCertCn(): ?string;

  /**
   * Returns Server Alternative Names attribute of the TLS certificate
   * presented by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a vector of strings containing a collection of Server
   *           Alternative Names values from the server certificate presented
   *           by MySQL.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function getSslCertSan(): Vector<string>;

  /**
   * Returns values from the selected cert extensions of the TLS certificate
   * presented by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a vector of strings containing a collection of the selected
   *           cert extension values from the server certificate presented
   *           by MySQL.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function getSslCertExtensions(): Vector<string>;

  /**
   * Returns a boolean value indicating if server cert validation was enforced
   * for this connection.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - "true" if server cert validation was enforced during TLS
   *           handshake for this connection, "false" otherwise.
   */
  <<__Native>>
  public function isSslCertValidationEnforced(): bool;

  /**
   * Retrieve the SSL/TLS version for a connection
   *
   * This information can be logged to have visibility on what was resolved during the TLS handshake.
   *
   * @return - The TLS version or an empty string
   */
  <<__Native>>
  public function getSslVersion(): ?string;
}

/**
 * This class holds the SSL Context Provider that MySQL client will use to
 * establish a SSL connection.
 *
 * While not required, you can pass a `MySSLContextProvider` to
 * `AsyncMysqlClient::connect()`.
 *
 * The SSL context data isn't accessible by PHP; it is for internal use only.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
class MySSLContextProvider {
  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Determines if the current SSL Context Provider is valid.
   *
   * @return - `true` if the provider is valid; `false` otherwise.
   */
  <<__Native>>
  public function isValid(): bool;

  /**
   * Allows to disable TLS session resumption for the connections
   * created using this provider.
   * The resumption is enabled by default. Disabling sesison resumption
   * can be helpful in test scenarios. It allows to force full TLS
   * handshake for every newly created connection.
   *
   * @param $allow - Pass `true` to enable session resumption; `false`
   *                 otherwise.
   */
  <<__Native>>
  public function allowSessionResumption(bool $allow): void;
}

/**
 * This class holds the Connection Options that MySQL client will use to
 * establish a connection.
 *
 * The `AsyncMysqlConnectionOptions` will be passed to
 * `AsyncMysqlClient::connectWithOpts()`.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
class AsyncMysqlConnectionOptions {

  // Sets the Connect Timeout for each connection attempt
  <<__Native>>
  public function setConnectTimeout(int $timeout): void;

  // Sets the Connect Tcp Timeout for each connection attempt
  // This timeout is for only tcp handshake phase of the connect op
  <<__Native>>
  public function setConnectTcpTimeout(int $timeout): void;

  // Sets the number of attempts this operation will retry connecting
  <<__Native>>
  public function setConnectAttempts(int $attempts): void;

  // Sets the total timeout that the connect will use
  <<__Native>>
  public function setTotalTimeout(int $timeout): void;

  // Sets the maximum time for a running query
  <<__Native>>
  public function setQueryTimeout(int $timeout): void;

  // Sets a map of connection attributes that will be sent to Mysql in the
  // Connection Attributes Handshake field
  <<__Native>>
  public function setConnectionAttributes(darray<string,string> $attrs): void;

  // SSL Configuration if SSL is to be used for connection
  <<__Native>>
  public function setSSLOptionsProvider(?MySSLContextProvider $ssl_opts): void;

  // SNI hostname to use when connecting via SSL
  <<__Native>>
  public function setSniServerName(string $sni_server_name) : void;

  // Enable reset conn before pooled conn is returned to pool
  <<__Native>>
  public function enableResetConnBeforeClose(): void;

  // Enable delayed reset conn before pooled conn is returned to pool
  <<__Native>>
  public function enableDelayedResetConn(): void;

  // Enable change_user feature in connection pool
  <<__Native>>
  public function enableChangeUser(): void;

  // TLS cert extension parameters to validate server cert
  <<__Native>>
  public function setServerCertValidation(string $extensions = "",
                                          string $values = ""): void;

  // Gets the Connect Timeout for each connection attempt
  <<__Native>>
  public function getConnectTimeout(): int;

  // Gets the Connect Tcp Timeout for each connection attempt
  // Returns -1 if not set
  <<__Native>>
  public function getConnectTcpTimeout(): int;

  // Gets the number of attempts this operation will retry connecting
  <<__Native>>
  public function getConnectAttempts(): int;

  // Gets the total timeout that the connect will use
  <<__Native>>
  public function getTotalTimeout(): int;

  // Gets the maximum time for a running query
  <<__Native>>
  public function getQueryTimeout(): int;

  // Gets the map of connection attributes that will be sent to Mysql in the
  // Connection Attributes Handshake field
  <<__Native>>
  public function getConnectionAttributes(): darray<string, string>;
}
/**
 * Provides timing statistics about the MySQL client.
 *
 * This class provides round-trip and callback timing information for various
 * operations on the MySQL client.
 *
 * This information can be used to know how the performance of the MySQL client
 * may have affected a given result.
 *
 * For example, if you have a `AsyncMysqlConnection`, you can call:
 *
 * `$conn->connectResult()->clientStats()->ioEventLoopMicrosAvg()`
 *
 * to get round-trip timing information on the connection event itself.
 *
 * Basically any concrete implementation of `AsyncMysqlResult` can provide
 * these type of statistics by calling its `clientStats()` method and a method
 * on this class.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
class AsyncMysqlClientStats {
  /**
   * @internal
   *
   * The statistics should be retrieved in the EventBase thread; they are
   * appended to the result of an operation so the performance state can be
   * logged.
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Average loop time of the MySQL client event, in microseconds.
   *
   * An event can include a connection, an error condition, a query, etc.
   *
   * This returns an exponentially-smoothed average.
   *
   * @return - A `float` representing the average for an event to happen on this
   *           MySQL client.
   */
  <<__Native>>
  public function ioEventLoopMicrosAvg() : float;

  /**
   * Average delay between when a callback is scheduled in the MySQL client
   * and when it's actually ran, in microseconds.
   *
   * The callback can be from creating a connection, inducing an error
   * condition, executing a query, etc.
   *
   * This returns an exponentially-smoothed average.
   *
   * @return - A `float` representing the average callback delay on this
   *           MySQL client.
   */
  <<__Native>>
  public function callbackDelayMicrosAvg() : float;

  /**
   * Average of reported busy time in the client's IO thread.
   *
   * This returns an exponentially-smoothed average.
   *
   * @return - A `float` representing the average busy time of this
   *           MySQL client's IO Thread.
   */
  <<__Native>>
  public function ioThreadBusyMicrosAvg() : float;

  /**
   * Average of reported idle time in the client's IO thread.
   *
   * This returns an exponentially-smoothed average.
   *
   * @return - A `float` representing the average busy time of this
   *           MySQL client's IO Thread.
   */
  <<__Native>>
  public function ioThreadIdleMicrosAvg() : float;

  /**
   * Size of this client's event base notification queue.
   * Value is collected at the end of the operation.
   *
   * @return - A `int` representing the size of notification queue of this
   *           MySQL client's IO Thread.
   */
  <<__Native>>
  public function notificationQueueSize() : int;

}

/**
 * A base class for connection, query and error results.
 *
 * This class is `abstract` and cannot be instantiated, but provides the methods
 * that concrete classes must implement, which are timing information methods
 * regarding a query, connection or a resulting error.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
abstract class AsyncMysqlResult {

  /**
   * @internal
   *
   * Abstract class
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * The total time for the specific MySQL operation, in microseconds.
   *
   * @return - the total operation time as `int` microseconds.
   */
  public abstract function elapsedMicros(): int;

  /**
   * The start time for the specific MySQL operation, in seconds since epoch.
   *
   * @return - the start time as `float` seconds since epoch.
   */
  public abstract function startTime(): float;

  /**
   * The end time for the specific MySQL operation, in seconds since epoch.
   *
   * @return - the end time as `float` seconds since epoch.
   */
  public abstract function endTime(): float;

  /**
   * Returns the MySQL client statistics at the moment the result was created.
   *
   * This information can be used to know how the performance of the MySQL
   * client may have affected the result.
   *
   * @return - an `AsyncMysqlClientStats` object to query about event and
   *           callback timing to the MySQL client for the specific result.
   */
  public abstract function clientStats(): AsyncMysqlClientStats;

  /**
   * Returns whether or not the current connection reused the SSL session
   * from another SSL connection. The session is set by MySSLContextProvider.
   * Some cases, the server can deny the session that was set and the handshake
   * will create a new one, in those cases this function will return `false`.
   * If this connections isn't SSL, `false` will be returned as well.
   *
   * @return - `true` if this is a SSL connection and the SSL session was
   *           reused; `false` otherwise.
   */
  <<__Native>>
  public function sslSessionReused(): bool;

  /**
   * Returns Common Name attribute of the TLS certificate presented
   * by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a string containing Common Name value from the server
   *           certificate presented by MySQL.
   */
  <<__Native>>
  public function getSslCertCn(): ?string;

  /**
   * Returns Server Alternative Names attribute of the TLS certificate
   * presented by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a vector of strings containing Server Alternative Names values
   *           from the server certificate presented by MySQL.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function getSslCertSan(): Vector<string>;

  /**
   * Returns values from the selected cert extensions of the TLS certificate
   * presented by MySQL server.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - a vector of strings containing the selected cert extension
   *           values from the server certificate presented by MySQL.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function getSslCertExtensions(): Vector<string>;

  /**
   * Returns a boolean value indicating if server cert validation was enforced
   * for this connection.
   *
   * This information can be used while troubleshooting TLS handshake
   * failures happening on connect stage.
   *
   * @return - `true` if server cert validation was enforced during TLS
   *           handshake for this connection, `false` otherwise.
   */
  <<__Native>>
  public function isSslCertValidationEnforced(): bool;

  /**
   * Returns the TLS protocol version negotiated for this connection (e.g.
   * "TLSv1.2" or "TLSv1.3"), as resolved during the TLS handshake.
   *
   * Unlike `AsyncMysqlConnection::getSslVersion()`, this is available on the
   * result object, so it can be read on the connect-and-query path where no
   * `AsyncMysqlConnection` is returned.
   *
   * @return - The TLS version, or an empty string for non-SSL connections.
   */
  <<__Native>>
  public function getSslVersion(): ?string;

}

/**
 * Provides the result information for when the connection to the MySQL
 * client is made successfully.
 *
 * This class is instantiated through a call from the connection object
 * via `AsyncMysqlConnection::connectResult()`.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlConnectResult extends AsyncMysqlResult {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * The total time for the establishment of the MySQL connection,
   * in microseconds.
   *
   * @return - the total establishing connection time as `int` microseconds.
   */
  <<__Native>>
  public function elapsedMicros(): int;

  /**
   * The start time for the connection operation, in seconds since epoch.
   *
   * @return - the start time as `float` seconds since epoch.
   */
  <<__Native>>
  public function startTime(): float;

  /**
   * The end time of the connection operation, in seconds since epoch.
   *
   * @return - the end time as `float` seconds since epoch.
   */
  <<__Native>>
  public function endTime(): float;

  /**
   * Returns the MySQL client statistics at the moment the connection was
   * established.
   *
   * This information can be used to know how the performance of the
   * MySQL client may have affected the connecting operation.
   *
   * @return - an `AsyncMysqlClientStats` object to query about event and
   *           callback timing to the MySQL client for the connection.
   */
  <<__Native>>
  public function clientStats(): AsyncMysqlClientStats;

}

/**
 * Contains error information for a failed operation (e.g., connection, query).
 *
 * This class is instantiated when an `AsyncMysqlException` is thrown and
 * `AsyncMysqlException::getResult()` is called.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
class AsyncMysqlErrorResult extends AsyncMysqlResult {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * The total time for the MySQL error condition to occur, in microseconds.
   *
   * @return - the total error producing time as `int` microseconds.
   */
  <<__Native>>
  public function elapsedMicros(): int;

  /**
   * The start time when the error was produced, in seconds since epoch.
   *
   * @return - the start time as `float` seconds since epoch.
   */
  <<__Native>>
  public function startTime(): float;

  /**
   * The end time when the error was produced, in seconds since epoch.
   *
   * @return - the end time as `float` seconds since epoch.
   */
  <<__Native>>
  public function endTime(): float;

  /**
   * Returns the MySQL client statistics for the events that produced the error.
   *
   * This information can be used to know how the performance of the
   * MySQL client may have affected the operation that produced the error.
   *
   * @return - an `AsyncMysqlClientStats` object to query about event and
   *           callback timing to the MySQL client for whatever caused the
   *            error.
   */
  <<__Native>>
  public function clientStats(): AsyncMysqlClientStats;

  /**
   * Returns the MySQL error number for this result.
   *
   * See MySQL's
   * [mysql_errno()](http://dev.mysql.com/doc/refman/5.0/en/mysql-errno.html)
   * for information on the error numbers.
   *
   * @return - The error number as an `int`.
   */
  <<__Native>>
  public function mysql_errno(): int;

  /**
   * Returns a human-readable string for the error encountered in this result.
   *
   * @return - The error string.
   */
  <<__Native>>
  public function mysql_error(): ?string;

  /**
   * The type of failure that produced this result.
   *
   * The string returned will be either `'TimedOut'`, representing a timeout, or
   * `'Failed'`, representing the server rejecting the connection or query.
   *
   * @return - the type of failure, either `'TimedOut'` or `'Failed'`.
   */
  <<__Native>>
  public function failureType(): ?string;
}

/**
 * Contains the information about results for query statements that ran before
 * a MySQL error.
 *
 * This class is instantiated when an `AsyncMysqlQueryException` is thrown and
 * `AsyncMysqlQueryException::getResult()` is called.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlQueryErrorResult extends AsyncMysqlErrorResult {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Returns the number of successfully executed queries.
   *
   * If there were any successful queries before receiving the error, this will
   * let you know how many of those there were.
   *
   * @return - The number of successful queries before the error as an `int`.
   */
  <<__Native>>
  public function numSuccessfulQueries(): int;

  /**
   * Returns the results that were fetched by the successful query statements.
   *
   * @return - A `Vector` of `AsyncMysqlQueryResult` objects for each result
   *           produced by a successful query statement.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function getSuccessfulResults(): Vector<AsyncMysqlQueryResult>;
}

/**
 * The result of a successfully executed MySQL query.
 *
 * Not only does this class provide timing information about retrieving the
 * successful result, it provides the actual result information (e.g., result
 * rows).
 *
 * You get an `AsyncMysqlQueryResult` through calls to
 * `AsyncMysqlConnection::query()`, `AsyncMysqlConection::queryf()` and
 * `AsyncMysqlConnection::multiQuery()`
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlQueryResult extends AsyncMysqlResult {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * The total time for the successful query to occur, in microseconds.
   *
   * @return - the total successful result producing time as `int` microseconds.
   */
  <<__Native>>
  public function elapsedMicros(): int;

  /**
   * The start time when the successful query began, in seconds since epoch.
   *
   * @return - the start time as `float` seconds since epoch.
   */
  <<__Native>>
  public function startTime(): float;

  /**
   * The end time when the successful query began, in seconds since epoch.
   *
   * @return - the end time as `float` seconds since epoch.
   */
  <<__Native>>
  public function endTime(): float;

  /**
   * Returns the MySQL client statistics at the moment the successful query
   * ended.
   *
   * This information can be used to know how the performance of the
   * MySQL client may have affected the query operation.
   *
   * @return - an `AsyncMysqlClientStats` object to query about event and
   *           callback timing to the MySQL client for the query.
   */
  <<__Native>>
  public function clientStats(): AsyncMysqlClientStats;

  /**
   * The number of database rows affected in the current result.
   *
   * This is particularly useful for `INSERT`, `DELETE`, `UPDATE` statements.
   *
   * This is complementary to `numRows()` as they might be the same value, but
   * if this was an `INSERT` query, for example, then this might be a non-zero
   * value, while `numRows()` would be 0.
   *
   * See the MySQL's [mysql_affected_rows()](http://goo.gl/1Sj2zS)
   * documentation for more information.
   *
   * @return - The number of rows affected as an `int`.
   */
  <<__Native>>
  public function numRowsAffected(): int;

  /**
   * The last ID inserted, if one existed, for the query that produced the
   * current result.
   *
   * See the MySQL's [mysql_insert_id()](http://goo.gl/qxIcPz) documentation for
   * more information.
   *
   * @return - The last insert id, or 0 if none existed.
   */
  <<__Native>>
  public function lastInsertId(): int;

  /**
   * The number of rows in the current result.
   *
   * This is particularly useful for `SELECT` statements.
   *
   * This is complementary to `numRowsAffected()` as they might be the same
   * value, but if this was an `INSERT` query, for example, then this might be
   * 0, while `numRowsAffected()` could be non-zero.
   *
   * See the MySQL's [mysql_num_rows()](http://goo.gl/Rv5NaL) documentation for
   * more information.
   *
   * @return - The number of rows in the current result as an `int`.
   */
  <<__Native>>
  public function numRows(): int;

  /**
   * Returns the actual rows returned by the successful query, each row
   * including the name and value for each column.
   *
   *  All values come back as `string`s. If you want typed values, use
   * `mapRowsTyped()`.
   *
   * The rows are returned as a `Vector` of `Map` objects. The `Map` objects map
   * column names to (possibly `null`) `string` values.
   *
   * @return - A `Vector` of `Map` objects, where the `Vector` elements are the
   *           rows and the `Map` elements are the column names and values
   *           associated with that row.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function mapRows(): Vector<Map<string, ?string>>;

  /**
   * Returns the actual rows returned by the successful query, each row
   * including the values for each column.
   *
   * All values come back as `string`s. If you want typed values, use
   * `vectorRowsTyped()`.
   *
   * The rows are returned as a `Vector` of `Vector` objects which hold the
   * (possibly `null`) `string` values of each column in the order of the
   * original query.
   *
   * @return - A `Vector` of `Vector` objects, where the outer `Vector`
   *           represents the rows and each inner `Vector` represent the
   *           column values for each row.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function vectorRows(): Vector<KeyedContainer<int, ?string>>;

  /**
   * Returns the actual rows returned by the successful query, each row
   * including the name and typed-value for each column.
   *
   * The rows are returned as a `Vector` of `Map` objects. The `Map` objects map
   * column names to (possibly `null`) `mixed` values (e.g., an `INTEGER` column
   * will come back as an `int`.)
   *
   * @return - A `Vector` of `Map` objects, where the `Vector` elements are the
   *           rows and the `Map` elements are the column names and typed values
   *           associated with that row.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function mapRowsTyped():  Vector<Map<string, mixed>>;

  <<__Native>>
  public function dictRowsTyped(): vec<dict<string, arraykey>>;

  /**
   * Returns the actual rows returned by the successful query, each row
   * including the typed values for each column.
   *
   * The rows are returned as a `Vector` of `Vector` objects which hold the
   * (possibly `null`) `mixed` values of each column in the order of the
   * original query (e.g., an `INTEGER` column will come back as an `int`.).
   *
   * @return - A `Vector` of `Vector` objects, where the outer `Vector`
   *           represents the rows and each inner `Vector` represent the typed
   *           column values for each row.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function vectorRowsTyped(): Vector<KeyedContainer<int, mixed>>;

  /**
   * Returns a `Vector` representing all row blocks returned by the successful
   * query.
   *
   * A row block can be the full result of the query (if there is only one
   * row block), or it can be the partial result of the query (if there are
   * more than one row block). The total number of row blocks makes up the
   * entire result of the successful query.
   *
   * Usually, there will be only one row block in the vector because the
   * query completed in full in one attempt. However, if, for example, the
   * query represented something that exceeded some network parameter, the
   * result could come back in multiple blocks.
   *
   * @return - A `Vector` of `AsyncMysqlRowBlock` objects, the total number
   *           of which represent the full result of the query.
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function rowBlocks(): Vector<AsyncMysqlRowBlock>;

  /**
   * Returns whether or not any of the queries executed did not use an index
   * during execution
   *
   * @return - 'true' if no index was used for any of the queries executed,
   *              'false' otherwise
   */
  <<__Native>>
  public function noIndexUsed(): bool;

  /**
   * The GTID of database returned for the current commit.
   *
   * This is particularly useful for `INSERT`, `DELETE`, `UPDATE` statements.
   *
   * @return - The gtid of the current commit as a `string`.
   */
  <<__Native>>
  public function recvGtid(): ?string;

  /**
   * The response attributes returned for the current query
   *
   * @return - A Map<string, string> of the response attributes from MySQL
   */
  <<__Native>>
  /* HH_FIXME[2049] TODO(T121423772) [systemlib] Hack Collections */
  public function responseAttributes(): Map<string, string>;

  /**
   * The number of bytes in the current result set.
   *
   * This is particularly useful for `SELECT` statements.
   *
   * See the MySQL's mysql_fetch_lengths() api documentation for
   * more information.
   *
   * @return - The size of result set in bytes as an `int`.
   */
  <<__Native>>
  public function resultSizeBytes(): int;

  /**
   * The MySQL info string for the current query.
   *
   * This contains information about the affected rows for DML statements
   * like INSERT, UPDATE, DELETE. For UPDATE statements, it includes
   * "Rows matched: X  Changed: Y  Warnings: Z".
   *
   * @return - The MySQL info string, or `null` if not available.
   */
  <<__Native>>
  public function mysqlInfo(): ?string;

  /**
   * The number of rows matched by an UPDATE or DELETE statement.
   *
   * This is different from `numRowsAffected()` for UPDATE statements where
   * a row is matched but not changed (because the new values are the same
   * as the old values).
   *
   * @return - The number of matched rows, or `null` if not available.
   */
  <<__Native>>
  public function rowsMatched(): ?int;

  /**
   * The number of warnings generated by the current query.
   *
   * @return - The number of warnings as an `int`.
   */
  <<__Native>>
  public function warningsCount(): int;
}

/**
 * Represents a row block.
 *
 * A row block is either a full or partial set of result rows from a MySQL
 * query.
 *
 * In a query result, the sum total of all the row blocks is the full result
 * of the query. Most of the time there is only one row block per query result
 * since the query was never interrupted or otherwise deterred by some outside
 * condition like exceeding network packet parameters.
 *
 * You can get an instance of `AsyncMysqlRowBlock` via the
 * `AsyncMysqlQueryResult::rowBlocks()` call.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlRowBlock implements IteratorAggregate<mixed>, Countable {
  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Get a field (column) value.
   *
   * @param $row - the row index.
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The value of the field (column) at the given row.
   *
   */
  <<__Native>>
  public function at(int $row, mixed $field): mixed;

  /**
   * Get a certain field (column) value from a certain row as `int`.
   *
   * If the column from which you are retrieving the value is not an integral
   * type, then an `Exception` is thrown.
   *
   * @param $row - the row index.
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `int` value of the field (column); or an `Exception` if it
   *           the column is not integral.
   */
  <<__Native>>
  public function getFieldAsInt(int $row, mixed $field): int;

  /**
   * Get a certain field (column) value from a certain row as `double`.
   *
   * If the column from which you are retrieving the value is not an numeric
   * type, then an `Exception` is thrown.
   *
   * @param $row - the row index.
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `double` value of the field (column); or an `Exception` if it
   *           the column is not numeric.
   */
  <<__Native>>
  public function getFieldAsDouble(int $row, mixed $field): float;

  /**
   * Get a certain field (column) value from a certain row as `string`.
   *
   * @param $row - the row index.
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `string` value of the field (column).
   */
  <<__Native>>
  public function getFieldAsString(int $row, mixed $field): ?string;

  /**
   * Returns whether a field (column) value is `null`.
   *
   * @param $row - the row index.
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - `true` if the column value is `null`; `false` otherwise.
   */
  <<__Native>>
  public function isNull(int $row, mixed $field): bool;

  /**
   * Returns the type of the field (column).
   *
   * See [here](http://goo.gl/TbnKJy) and [here](https://goo.gl/aSEMeg) for the
   * integer mappings to SQL types.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The type of the field as an `int`.
   */
  <<__Native>>
  public function fieldType(mixed $field): int;

  /**
   * Returns the flags of the field (column).
   *
   * This gets the bitwise `OR` of the flags that are set for a given column.
   *
   * See [here](http://goo.gl/1RCN2l) for the possible flags.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The flags of the column as an `int`.
   */
  <<__Native>>
  public function fieldFlags(mixed $field): int;

  /**
   * Returns the name of the field (column).
   *
   * @param $field - the field index.
   *
   * @return - The name of the column as a `string`.
   */
  <<__Native>>
  public function fieldName(int $field): ?string;

  /**
   * Returns whether there were any rows are returned in the current row block.
   *
   * @return - `true` if there are rows; `false` otherwise.
   */
  <<__Native>>
  public function isEmpty(): bool;

  /**
   * Returns the number of fields (columns) associated with the current row
   * block.
   *
   * @return - The number of columns in the current row block.
   */
  <<__Native>>
  public function fieldsCount(): int;

  /**
   * Returns the number of rows in the current row block.
   *
   * @return - The number of rows in the current row block.
   */
  <<__Native>>
  public function count()[]: int;

  /**
   * Get the iterator for the rows in the block.
   *
   * @return - An `AsyncMysqlRowBlockIterator` to iterate over the current
   *            row block.
   */
  <<__Native>>
  public function getIterator(): AsyncMysqlRowBlockIterator;

  /**
   * Get a certain row in the current row block.
   *
   * @param $row - the row index.
   *
   * @return - The `AsyncMysqlRow` representing one specific row in the current
   *           row block.
   */
  <<__Native>>
  public function getRow(int $row): AsyncMysqlRow;
}

/**
 * A class to represent an iterator over the rows of a `AsyncMysqlRowBlock`.
 *
 * You can iterate over all the rows of an `AsyncMysqlRowBlock` one by one until
 * the iterator is not valid any longer.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlRowBlockIterator implements HH\KeyedIterator<string, AsyncMysqlRow> {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Check if iterator is at a valid `AsyncMysqlRow`.
   *
   * @return - `true` if the iterator is still pointing to a valid row;
   *            otherwise `false`.
   */
  <<__Native>>
  public function valid(): bool;

  /**
   * Advance the iterator to the next row.
   *
   */
  <<__Native>>
  public function next(): void;

  /**
   * Get the current row.
   *
   * @return - The `AsyncMysqlRow` associated with the current iterator
   *           position.
   */
  <<__Native>>
  public function current(): AsyncMysqlRow;

  /**
   * Get the current row number
   *
   * @return - The current row number associated with the current iterator
   *           position.
   */
  <<__Native>>
  public function key(): int;

  /**
   * Reset the iterator to the first row.
   *
   */
  <<__Native>>
  public function rewind(): void;
}

/**
 * A class to represent a row.
 *
 * You can think of a row just like you do a database row that might be
 * returned as a result from a query. The row has values associated with
 * each column.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlRow implements MysqlRow {

  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Get field (column) value indexed by the `field`.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The value of the field (column).
   *
   */
  <<__Native>>
  public function at(mixed $field): mixed;

  /**
   * Get a certain field (column) value as an `int`.
   *
   * If the column from which you are retrieving the value is not an integral
   * type, then an `Exception` is thrown.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `int` value of the field (column); or an `Exception` if it
   *           the column is not integral.
   */
  <<__Native>>
  public function getFieldAsInt(mixed $field): int;

  /**
   * Get a certain field (column) value as a `double`.
   *
   * If the column from which you are retrieving the value is not an numeric
   * type, then an `Exception` is thrown.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `double` value of the field (column); or an `Exception` if it
   *           the column is not numeric.
   */
  <<__Native>>
  public function getFieldAsDouble(mixed $field): float;

  /**
   * Get a certain field (column) value as a `string`.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The `string` value of the field (column).
   */
  <<__Native>>
  /* HH_FIXME[4341] override returns `?string` to match underlying OptString */
  public function getFieldAsString(mixed $field): ?string;

  /**
   * Returns whether a field (column) value is `null`.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - `true` if the column value is `null`; `false` otherwise.
   */
  <<__Native>>
  public function isNull(mixed $field): bool;

  /**
   * Returns the type of the field (column).
   *
   * See [here](http://goo.gl/TbnKJy) and [here](https://goo.gl/aSEMeg) for the
   * integer mappings to SQL types.
   *
   * @param $field - the field index (`int`) or field name (`string`).
   *
   * @return - The type of the field as an `int`.
   */
  <<__Native>>
  public function fieldType(mixed $field): int;

  /**
   * Get the number of fields (columns) in the current row.
   *
   * @return - The number of columns in the current row.
   */
  <<__Native>>
  public function count()[]: int;

  /**
   * Get the iterator over the fields in the current row.
   *
   * @return - An `AsyncMysqlRowIterator` to iterate over the current row.
   */
  <<__Native>>
  public function getIterator(): AsyncMysqlRowIterator;
}

/**
 * A class to represent an iterator over the fields (columns) in a row.
 *
 * You can iterate over all the fields (columns) of an `AsyncMysqlBlock` one by
 * one until the iterator is not valid any longer.
 *
 * @guide /hack/async/introduction
 * @guide /hack/async/extensions
 */
<<__NativeData>>
final class AsyncMysqlRowIterator implements HH\KeyedIterator<string, string> {
  /**
   * @internal
   */
  private function __construct(): void {
    throw new InvalidOperationException(
      __CLASS__ . " objects cannot be directly created");
  }

  /**
   * Check if the iterator is at a valid field (column).
   *
   * @return - `true` if the iterator is still pointing to a valid column;
   *            otherwise `false`.
   */
  <<__Native>>
  public function valid(): bool;

  /**
   * Advance the iterator to the next field (column).
   *
   */
  <<__Native>>
  public function next(): void;

  /**
   * Get the current field (column) name.
   *
   * @return - The column name associated with the current iterator
   *           position.
   */
  <<__Native>>
  /* HH_FIXME[4341] override returns `?string` to match underlying OptString */
  public function current(): ?string;

  /**
   * Get the current field (column) number.
   *
   * @return - The column number associated with the current iterator position.
   *
   */
  <<__Native>>
  public function key(): int;

  /**
   * Reset the iterator to the first field (column).
   *
   */
  <<__Native>>
  public function rewind(): void;
}

}

namespace HH\Lib\SQL {
  type QueryFormatString = string;

  final class Query {
    private QueryFormatString $format;
    private Container<mixed> $args;
    public function __construct(QueryFormatString $format, mixed ...$args) {
      $this->format = $format;
      $this->args = $args;
    }

    <<__Native, \NoDoc>>
    public function toString__FOR_DEBUGGING_ONLY(
      \AsyncMysqlConnection $conn,
    ): ?string;

    <<__Native, \NoDoc>>
    public function toUnescapedString__FOR_DEBUGGING_ONLY__UNSAFE(): ?string;
  }
}
