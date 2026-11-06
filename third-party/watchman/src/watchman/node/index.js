/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 * @flow
 */

'use strict';

const bser = require('bser');
const childProcess = require('child_process');
const {EventEmitter} = require('events');
const net = require('net');

// We'll emit the responses to these when they get sent down to us
const unilateralTags = ['subscription', 'log'];

/*::
interface BunserBuf extends EventEmitter {
  append(buf: Buffer): void;
}

type Options = {
  watchmanBinaryPath?: string,
  ...
};

type Response = {
  capabilities?: ?{[key: string]: boolean},
  version: string,
  error?: string,
  ...
};

type CommandCallback = (error: ?Error, result?: Response) => void;

type Command = {
  cb: CommandCallback,
  cmd: mixed,
};
*/

class WatchmanError extends Error {
  watchmanResponse /*: mixed */;
}

/**
 * @param options An object with the following optional keys:
 *   * 'watchmanBinaryPath' (string) Absolute path to the watchman binary.
 *     If not provided, the Client locates the binary using the PATH specified
 *     by the node child_process's default env.
 */
class Client extends EventEmitter {
  bunser /*: ?BunserBuf */;
  commands /*: Array<Command> */;
  connecting /*: boolean */;
  currentCommand /*: ?Command */;
  socket /*: ?net.Socket */;
  watchmanBinaryPath /*: string */;

  constructor(options /*: Options */) {
    super();

    this.watchmanBinaryPath = 'watchman';
    if (options && options.watchmanBinaryPath) {
      this.watchmanBinaryPath = options.watchmanBinaryPath.trim();
    }
    this.commands = [];
  }

  // Try to send the next queued command, if any
  sendNextCommand() {
    if (this.currentCommand) {
      // There's a command pending response, don't send this new one yet
      return;
    }

    this.currentCommand = this.commands.shift();
    if (!this.currentCommand) {
      // No further commands are queued
      return;
    }

    if (this.socket) {
      this.socket.write(bser.dumpToBuffer(this.currentCommand.cmd));
    } else {
      this.emit(
        'error',
        new Error('socket is null attempting to send command'),
      );
    }
  }

  cancelCommands(why /*: string*/) {
    const error = new Error(why);

    // Steal all pending commands before we start cancellation, in
    // case something decides to schedule more commands
    const cmds = this.commands;
    this.commands = [];

    if (this.currentCommand) {
      cmds.unshift(this.currentCommand);
      this.currentCommand = null;
    }

    // Synthesize an error condition for any commands that were queued
    cmds.forEach(cmd => {
      cmd.cb(error);
    });
  }

  connect() {
    const makeSock = (sockname /*:string*/) => {
      // bunser will decode the watchman BSER protocol for us
      const bunser = (this.bunser = new bser.BunserBuf());
      // For each decoded line:
      bunser.on('value', obj => {
        // Figure out if this is a unliteral response or if it is the
        // response portion of a request-response sequence.  At the time
        // of writing, there are only two possible unilateral responses.
        let unilateral /*: false | string */ = false;
        for (let i = 0; i < unilateralTags.length; i++) {
          const tag = unilateralTags[i];
          if (tag in obj) {
            unilateral = tag;
          }
        }

        if (unilateral) {
          this.emit(unilateral, obj);
        } else if (this.currentCommand) {
          const cmd = this.currentCommand;
          this.currentCommand = null;
          if ('error' in obj) {
            const error = new WatchmanError(obj.error);
            error.watchmanResponse = obj;
            cmd.cb(error);
          } else {
            cmd.cb(null, obj);
          }
        }

        // See if we can dispatch the next queued command, if any
        this.sendNextCommand();
      });
      bunser.on('error', err => {
        this.emit('error', err);
      });

      const socket = (this.socket = net.createConnection(sockname));
      socket.on('connect', () => {
        this.connecting = false;
        this.emit('connect');
        this.sendNextCommand();
      });
      socket.on('error', err => {
        this.connecting = false;
        this.emit('error', err);
      });
      socket.on('data', buf => {
        if (this.bunser) {
          this.bunser.append(buf);
        }
      });
      socket.on('end', () => {
        this.socket = null;
        this.bunser = null;
        this.cancelCommands('The watchman connection was closed');
        this.emit('end');
      });
    };

    // triggers will export the sock path to the environment.
    // If we're invoked in such a way, we can simply pick up the
    // definition from the environment and avoid having to fork off
    // a process to figure it out
    if (process.env.WATCHMAN_SOCK) {
      makeSock(process.env.WATCHMAN_SOCK);
      return;
    }

    // We need to ask the client binary where to find it.
    // This will cause the service to start for us if it isn't
    // already running.
    const args = ['--no-pretty', 'get-sockname'];

    // We use the more elaborate spawn rather than exec because there
    // are some error cases on Windows where process spawning can hang.
    // It is desirable to pipe stderr directly to stderr live so that
    // we can discover the problem.
    let proc = null;
    let spawnFailed = false;

    const spawnError = (
      error /*: Error | {message: string, code?: string, errno?: string}*/,
    ) => {
      if (spawnFailed) {
        // For ENOENT, proc 'close' will also trigger with a negative code,
        // let's suppress that second error.
        return;
      }
      spawnFailed = true;
      if (error.code === 'EACCES' || error.errno === 'EACCES') {
        error.message =
          'The Watchman CLI is installed but cannot ' +
          'be spawned because of a permission problem';
      } else if (error.code === 'ENOENT' || error.errno === 'ENOENT') {
        error.message =
          'Watchman was not found in PATH.  See ' +
          'https://meta.github.io/watchman/docs/install.html ' +
          'for installation instructions';
      }
      console.error('Watchman: ', error.message);
      this.emit('error', error);
    };

    try {
      proc = childProcess.spawn(this.watchmanBinaryPath, args, {
        stdio: ['ignore', 'pipe', 'pipe'],
        windowsHide: true,
      });
    } catch (error) {
      spawnError(error);
      return;
    }

    const stdout = [];
    const stderr = [];
    proc.stdout.on('data', data => {
      stdout.push(data);
    });
    proc.stderr.on('data', data => {
      data = data.toString('utf8');
      stderr.push(data);
      console.error(data);
    });
    proc.on('error', error => {
      spawnError(error);
    });

    proc.on('close', (code, signal) => {
      if (code !== 0) {
        spawnError(
          new Error(
            this.watchmanBinaryPath +
              ' ' +
              args.join(' ') +
              ' returned with exit code=' +
              code +
              ', signal=' +
              signal +
              ', stderr= ' +
              stderr.join(''),
          ),
        );
        return;
      }
      try {
        const obj = JSON.parse(stdout.join(''));
        if ('error' in obj) {
          const error = new WatchmanError(obj.error);
          error.watchmanResponse = obj;
          this.emit('error', error);
          return;
        }
        makeSock(obj.sockname);
      } catch (e) {
        this.emit('error', e);
      }
    });
  }

  command(args /*: mixed*/, done /*: CommandCallback */ = () => {}) {
    // Queue up the command
    this.commands.push({cmd: args, cb: done});

    // Establish a connection if we don't already have one
    if (!this.socket) {
      if (!this.connecting) {
        this.connecting = true;
        this.connect();
        return;
      }
      return;
    }

    // If we're already connected and idle, try sending the command immediately
    this.sendNextCommand();
  }

  // This is a helper that we expose for testing purposes
  _synthesizeCapabilityCheck /*:: <T: { capabilities?: ?{[key: string]: boolean}, version: string, error?: string, ... }> */(
    resp /*: T */,
    optional /*: $ReadOnlyArray<string> */,
    required /*: $ReadOnlyArray<string> */,
  ) /*: T & { error?: string, ...} */ {
    const capabilities /*:{[key: string]: boolean} */ = (resp.capabilities =
      {});
    const version = resp.version;
    optional.forEach(name => {
      capabilities[name] = have_cap(version, name);
    });
    required.forEach(name => {
      const have = have_cap(version, name);
      capabilities[name] = have;
      if (!have) {
        resp.error =
          'client required capability `' +
          name +
          '` is not supported by this server';
      }
    });
    return resp;
  }

  capabilityCheck(
    caps /*: $ReadOnly<{optional?: $ReadOnlyArray<string>, required?: $ReadOnlyArray<string>, ...}>*/,
    done /*: CommandCallback */,
  ) {
    const optional = caps.optional || [];
    const required = caps.required || [];
    this.command(
      [
        'version',
        {
          optional: optional,
          required: required,
        },
      ],
      (error, resp /*: ?Response */) => {
        if (error || !resp) {
          done(error || new Error('no watchman response'));
          return;
        }
        if (!('capabilities' in resp)) {
          // Server doesn't support capabilities, so we need to
          // synthesize the results based on the version
          resp = this._synthesizeCapabilityCheck(resp, optional, required);
          if (resp.error) {
            error = new WatchmanError(resp.error);
            error.watchmanResponse = resp;
            done(error);
            return;
          }
        }
        done(null, resp);
      },
    );
  }

  // Close the connection to the service
  end() {
    this.cancelCommands('The client was ended');
    if (this.socket) {
      this.socket.end();
      this.socket = null;
    }
    this.bunser = null;
  }
}

const cap_versions /*: $ReadOnly<{[key:string]: ?string}>*/ = {
  'cmd-watch-del-all': '3.1.1',
  'cmd-watch-project': '3.1',
  relative_root: '3.3',
  'term-dirname': '3.1',
  'term-idirname': '3.1',
  wildmatch: '3.7',
};

// Compares a vs b, returns < 0 if a < b, > 0 if b > b, 0 if a == b
function vers_compare(aStr /*:string*/, bStr /*:string*/) {
  const a = aStr.split('.');
  const b = bStr.split('.');
  for (let i = 0; i < 3; i++) {
    const d = parseInt(a[i] || '0') - parseInt(b[i] || '0');
    if (d != 0) {
      return d;
    }
  }
  return 0; // Equal
}

function have_cap(vers /*:string */, name /*:string*/) {
  if (cap_versions[name] != null) {
    return vers_compare(vers, cap_versions[name]) >= 0;
  }
  return false;
}

module.exports.Client = Client;
