# Copyright (c) Meta Platforms, Inc. All Rights Reserved.

lib = File.expand_path('../lib', __FILE__)
$LOAD_PATH.unshift(lib) unless $LOAD_PATH.include?(lib)
require 'ruby-watchman/version'

Gem::Specification.new do |s|
  s.name          = 'ruby-watchman'
  s.version       = RubyWatchman::VERSION
  s.authors       = ['Greg Hurrell']
  s.email         = ['glh@fb.com']
  s.summary       = 'Ruby implementation of Watchman binary protocol'
  s.description   = <<-DESC.gsub(/^\s+/, '')
    A fast implementation of the Watchman binary protocol written in C.
  DESC
  s.homepage      = 'https://github.com/meta/watchman/tree/main/watchman/ruby/ruby-watchman'
  s.license       = 'BSD'
  s.extensions    = 'ext/ruby-watchman/extconf.rb'

  s.files         = Dir[
    'ext/ruby-watchman/**/*.{c,h,rb}',
    'ext/ruby-watchman/depend',
    'lib/**/*.rb',
    'spec/**/*.rb'
  ]
  s.test_files    = s.files.grep(%r{^spec/})
  s.require_paths = %w[lib]

  s.add_development_dependency 'bundler', '~> 1.5'
  s.add_development_dependency 'rake'
  s.add_development_dependency 'rspec', '3.0.0'
end
