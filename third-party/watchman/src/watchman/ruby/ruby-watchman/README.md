# RubyWatchman

RubyWatchman is a gem that implements the [Watchman binary
protocol](https://meta.github.io/watchman/docs/bser.html). It is
implemented in C for speed, and is much faster than talking to
[Watchman](https://github.com/meta/watchman) using the JSON protocol.

## Installation

Add this line to your application's Gemfile:

```ruby
gem 'ruby-watchman'
```

And then execute:

    $ bundle

Or install it yourself as:

    $ gem install ruby-watchman

## Usage

This example shows:

1. Asking a running Watchman process for its local socket
2. Checking with Watchman whether a given path (in this case the current working
   directory) is being watched
3. Adding the path to the watch list if necessary
4. Asking Watchman for the names of all files in the watch (files which exist,
   or have existed since watching started), then printing them

```ruby
require 'ruby-watchman'
require 'socket'
require 'pathname'

sockname = RubyWatchman.load(
  %x{watchman --output-encoding=bser get-sockname}
)['sockname']
raise unless $?.exitstatus.zero?

UNIXSocket.open(sockname) do |socket|
  root = Pathname.new('.').realpath.to_s
  roots = RubyWatchman.query(['watch-list'], socket)['roots']
  if !roots.include?(root)
    # this path isn't being watched yet; try to set up watch
    result = RubyWatchman.query(['watch', root], socket)

    # root_restrict_files setting may prevent Watchman from working
    raise if result.has_key?('error')
  end

  query = ['query', root, {
    'expression' => ['type', 'f'],
    'fields'     => ['name'],
  }]
  paths = RubyWatchman.query(query, socket)

  # could return error if watch is removed
  raise if paths.has_key?('error')

  p paths['files']
end
```
