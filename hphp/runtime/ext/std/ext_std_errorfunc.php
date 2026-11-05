<?hh

namespace {

/**
 * Generates a backtrace
 *
 * @param int $options - As of 5.3.6, this parameter is a bitmask for the
 *   following options:  debug_backtrace() options
 *   DEBUG_BACKTRACE_PROVIDE_OBJECT  Whether or not to populate the
 *   "object" index.    DEBUG_BACKTRACE_IGNORE_ARGS  Whether or not to omit
 *   the "args" index, and thus all the function/method arguments, to save
 *   memory.      Before 5.3.6, the only values recognized are TRUE or
 *   FALSE, which are the same as setting or not setting the
 *   DEBUG_BACKTRACE_PROVIDE_OBJECT option respectively.
 * @param int $limit - As of 5.4.0, this parameter can be used to limit
 *   the number of stack frames returned. By default (limit=0) it returns
 *   all stack frames.
 *
 * @return array - Returns an array of associative arrays. The possible
 *   returned elements are as follows:    Possible returned elements from
 *   debug_backtrace()           function string  The current function
 *   name. See also __FUNCTION__.    line integer  The current line number.
 *   See also __LINE__.    file string  The current file name. See also
 *   __FILE__.    class string  The current class name. See also __CLASS__
 *     object object  The current object.    type string  The current call
 *   type. If a method call, "->" is returned. If a static method call,
 *   "::" is returned. If a function call, nothing is returned.    args
 *   array  If inside a function, this lists the functions arguments. If
 *   inside an included file, this lists the included file name(s).
 */
<<__Native("NoInjection", "NoRecording"), __EagerVMSync>>
function debug_backtrace(int $options = DEBUG_BACKTRACE_PROVIDE_OBJECT,
                         int $limit = 0): vec<dict<string, mixed>>;

/**
 * Prints a backtrace
 *
 *
 * @param int $options - As of 5.3.6, this parameter is a bitmask for the
 *   following options:  debug_print_backtrace() options
 *   DEBUG_BACKTRACE_IGNORE_ARGS  Whether or not to omit the "args" index,
 *   and thus all the function/method arguments, to save memory.
 * @param int $limit - As of 5.4.0, this parameter can be used to limit
 *   the number of stack frames printed. By default (limit=0) it prints all
 *   stack frames.
 *
 * @return void -
 */
<<__Native("NoInjection"), __EagerVMSync>>
function debug_print_backtrace(int $options = 0,
                               int $limit = 0): void;

/**
 * Get the last occurred error
 *
 * @return array - Returns an associative array describing the last error
 *   with keys "type", "message", "file" and "line". If the error has been
 *   caused by a PHP internal function then the "message" begins with its
 *   name. Returns NULL if there hasn't been an error yet.
 */
<<__Native>>
function error_get_last(): darray<string, mixed>;

/**
 * Send an error message to the defined error handling routines
 *
 * @param string $message - The error message that should be logged.
 * @param int $message_type - Says where the error should go. The
 *   possible message types are as follows:    error_log() log types    0
 *   message is sent to PHP's system logger, using the Operating System's
 *   system logging mechanism or a file, depending on what the error_log
 *   configuration directive is set to. This is the default option.    1
 *   message is sent by email to the address in the destination parameter.
 *   This is the only message type where the fourth parameter,
 *   extra_headers is used.    2  No longer an option.    3  message is
 *   appended to the file destination. A newline is not automatically added
 *   to the end of the message string.    4  message is sent directly to
 *   the SAPI logging handler.
 * @param string $destination - The destination. Its meaning depends on
 *   the message_type parameter as described above.
 * @param string $extra_headers - The extra headers. It's used when the
 *   message_type parameter is set to 1. This message type uses the same
 *   internal function as mail() does.
 *
 * @return bool -
 */
<<__Native>>
function error_log(string $message,
                   int $message_type = 0,
                   ?string $destination = null,
                   ?string $extra_headers = null): bool;

/**
 * Sets which PHP errors are reported
 *
 * @param int $level - The new error_reporting level. It takes on either
 *   a bitmask, or named constants. Using named constants is strongly
 *   encouraged to ensure compatibility for future versions. As error
 *   levels are added, the range of integers increases, so older
 *   integer-based error levels will not always behave as expected.   The
 *   available error level constants and the actual meanings of these error
 *   levels are described in the predefined constants.
 *
 * @return int - Returns the old error_reporting level or the current
 *   level if no level parameter is given.
 */
<<__Native("NoRecording")>>
function error_reporting(?int $level = null)[leak_safe]: int;

/**
 * Restores the previous error handler function
 *
 * @return bool - This function always returns TRUE.
 */
<<__Native("NoRecording")>>
function restore_error_handler(): bool;

/**
 * Restores the previously defined exception handler function
 *
 *
 * @return bool - This function always returns TRUE.
 */
<<__Native("NoRecording")>>
function restore_exception_handler(): bool;

/**
 * Sets a user-defined error handler function
 *
 * @param callable $error_handler - A callback with the following
 *   signature. NULL may be passed instead, to reset this handler to its
 *   default state.    boolhandler interrno stringerrstr stringerrfile
 *   interrline arrayerrcontext    errno   The first parameter, errno,
 *   contains the level of the error raised, as an integer.     errstr
 *   The second parameter, errstr, contains the error message, as a string.
 *       errfile   The third parameter is optional, errfile, which contains
 *   the filename that the error was raised in, as a string.     errline
 *   The fourth parameter is optional, errline, which contains the line
 *   number the error was raised at, as an integer.     errcontext   The
 *   fifth parameter is optional, errcontext, which is an array that points
 *   to the active symbol table at the point the error occurred. In other
 *   words, errcontext will contain an array of every variable that existed
 *   in the scope the error was triggered in.
 *   Sixth parameter is backtrace.
 *   Seventh parameter is blame associated with the current implicit context.
 *   First part of it is blame from soft make ic inaccessible; while, the
 *   second part is blame from soft ic runWith.
 *   User error handler must not modify error context.
 *   If the function returns FALSE then the normal error handler continues.
 * @param int $error_types - Can be used to mask the triggering of the
 *   error_handler function just like the error_reporting ini setting
 *   controls which errors are shown. Without this mask set the
 *   error_handler will be called for every error regardless to the setting
 *   of the error_reporting setting.
 *
 * @return mixed - Returns a string containing the previously defined
 *   error handler (if any). If the built-in error handler is used NULL is
 *   returned. NULL is also returned in case of an error such as an invalid
 *   callback. If the previous error handler was a class method, this
 *   function will return an indexed array with the class and the method
 *   name.
 */
<<__Native("NoRecording")>>
function set_error_handler(mixed $error_handler,
                           int $error_types = E_ALL): mixed;

/**
 * Sets a user-defined exception handler function
 *
 *
 * @param callable $exception_handler - Name of the function to be called
 *   when an uncaught exception occurs. This function must be defined
 *   before calling set_exception_handler(). This handler function needs to
 *   accept one parameter, which will be the exception object that was
 *   thrown. This is the handler signature:    voidhandler Exceptionex
 *   NULL may be passed instead, to reset this handler to its default
 *   state.
 *
 * @return callable - Returns the name of the previously defined
 *   exception handler, or NULL on error. If no previous handler was
 *   defined, NULL is also returned.
 */
<<__Native("NoRecording")>>
function set_exception_handler(mixed $exception_handler): ?dynamic;

/**
 * Generates a user-level error/warning/notice message
 *
 * @param string $error_msg - The designated error message for this
 *   error. It's limited to 1024 bytes in length. Any additional characters
 *   beyond 1024 bytes will be truncated.
 * @param int $error_type - The designated error type for this error. It
 *   only works with the E_USER family of constants, and will default to
 *   E_USER_NOTICE.
 *
 * @return bool - This function returns FALSE if wrong error_type is
 *   specified, TRUE otherwise.
 */
<<__Native>>
function trigger_error(string $error_msg,
                       int $error_type = E_USER_NOTICE)[]: bool;

<<__Native>>
function trigger_sampled_error(string $error_msg,
                               int $sample_rate,
                               int $error_type = E_USER_NOTICE)[]: bool;

<<__Native>>
function user_error(string $error_msg, int $error_type = E_USER_NOTICE): bool;

/**
 * Displays fatal errors with this PHP document.
 *
 * When 500 fatal error is about to display, it will invoke this PHP page with
 * all global states right at when the error happens. This is useful for
 * gracefully displaying something helpful information to end users when a fatal
 * error has happened. Otherwise, a blank page will be displayed by default.
 *
 * @param string $page - Relative path of the PHP document.
 */
<<__Native>>
function hphp_set_error_page(string $page): void;

/**
 * Raises a fatal error.
 *
 * @param string $error_msg - The error message for the fatal.
 */
<<__Native>>
function hphp_throw_fatal_error(string $error_msg)[]: noreturn;

/**
 * Clears any output contents that have not been flushed to networked.
 *
 * This is useful when handling a fatal error. Before displaying a customized
 * PHP page, one may call this function to clear previously written content, so
 * to replay what will be displayed.
 */
<<__Native>>
function hphp_clear_unflushed(): void;

/**
 * Retrieves information about the caller that invoked the current function or
 * method.
 *
 * @return array - Returns an associative array. On success, the array will
 *    contain keys 'file', 'function', 'line' and optionally 'class' which
 *    indicate the filename, function, line number and class name (if in class
 *    context) of the callsite that invoked the current function or method.
 *    With no caller (a sync entry point, a shutdown function, an exception
 *    handler), 'file' and 'function' are '' and 'line' is 0.
 */
<<__Native, __EagerVMSync>>
function hphp_debug_caller_info()[leak_safe]: darray<string, mixed>;

/**
 * Retrieves the full function name of the caller that invoked the current
 * function or method.
 *
 * @return array - Returns either 'function' or 'class::method' of the callsite
 *     that invoked the current function or method.
 */
<<__Native, __EagerVMSync>>
function hphp_debug_caller_identifier()[leak_safe]: ?string;

<<__Native("NoInjection")>>
function hphp_debug_backtrace_hash(int $options = 0)[leak_safe]: int;

} // root namespace

namespace HH {

  // Type aliases for stack traces and frames
  type StackTrace = vec<StackFrame>;
  type StackFrame = shape(
    ?'file' => string,
    ?'line' => int,
    ?'function' => string,
    ?'args' => vec<mixed>,
    ?'class' => string,
    ?'object' => mixed,
    ?'type' => string,
    ?'metadata' => mixed,
    ...
  );

  /*
   * Retrieve errors generated while inside the error handler. Since the error
   * handler will not be invoked recursively, such errors will not use the
   * normal error handler machinery. Instead, the information for the error will
   * be queued onto a deferred list which can be accessed via this function. To
   * keep memory usage under control, the deferred list is bounded. The deferred
   * list is automatically cleared when the error handler is returned from, so
   * it must be accessed from within the error handler. Calling this function
   * has the side-effect of clearing the deferred list, so a subsequent call
   * will just reflect any new notices generated.
   *
   * The returned value is a vec, with each element a dict containing
   * information for a single error. Each dict has the following fields:
   *
   * - "error-num"       : Error number (first parameter to the error handler)
   * - "error-string"    : Error string (second parameter to the error handler)
   * - "error-file"      : File where error occurred (third parameter to the
   *                       error handler)
   * - "error-line"      : Line number where error occurred (fourth parameter
   *                       tp the error handler)
   * - "error-backtrace" : Backtrace where error occurred (sixth parameter
   *                       to the error handler)
   * - "error-implicit-context-blame : Blame associated with the current
   *                                   implicit context
   *
   * If there were more errors than could be queued, the last entry will have an
   * additional field called "overflow" set to true.
   */
  <<__Native>>
  function deferred_errors(): vec<dict<string, mixed>>;
}
