/* -*- mode: C; c-basic-offset: 4  -*- */
/*
 * Copyright (c) 2008, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the Georgia Tech Research Corporation nor
 *       the names of its contributors may be used to endorse or
 *       promote products derived from this software without specific
 *       prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY GEORGIA TECH RESEARCH CORPORATION ''AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GEORGIA
 * TECH RESEARCH CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/** \file achpipe.c
 *  \author Neil T. Dantam
 *
 * \todo Extend protocol so that it only sends frames when requested
 */


/** \page "Pipe Protocol"
 *
 * \section frame-format Frame Format
 *
 * <tt>
 * -----------------------------------\n
 * | "size" | int32 | "data" | $DATA |\n
 * -----------------------------------\n
 * </tt>
 *
 *
 * Ach frames are sent across the pipe as the
 * four ascii bytes "size", and big-endian 32-bit integer indicating
 * the size in bytes of the frame, the four ascii bytes "data", and
 * the proper number of data bytes.
 *
 * \section simple-mode
 *
 * A subscribing achpipe process will push frames across the pipe as
 * soon as they appear in the channel.  A publishing achpipe process
 * will read a frame from the pipe and the put it on the channel.
 *
 * \section sync-mode Synchronous Mode
 *
 * Subscribing achpipe processes can operate in synchronous mode.  In
 * this case, the process will read a command from the pipe and then
 * perform an action based on that command.  Current commands are:
 *
 *
 * <ul>
 * <li> \c next Send the next frame from channel. \sa ach_wait_next </li>
 * <li> \c last Send the last frame from channel. \sa ach_wait_last </li>
 * </ul>
 *
 * These commands are sent as four ascii bytes, no '\\n' and no '\\0',
 * just like the aforementioned "size" and "data" delimiters.  If
 * future commands need arguments, it may be best in increase the
 * length of all commands to 8 bytes so that a command can be obtained
 * with a single read().  That would avoid the additional system
 * call without having to resort to userspace buffering.
 *
 * \section comparison-proper
 *
 * Note that ach normally communicates using shared memory.  The pipe
 * protocol is only for intermachine communication or increasing
 * robustness if one does not want a process to touch shared memory
 * directly.
 *
 *
 * \sa Todo List
 */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <argp.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include "ach.h"


// lets pick the number POSIX specifies for atomic reads/writes
/// Initial size of ach frame buffer
#define INIT_BUF_SIZE 512

/*
  #define HEADER_LINE_MAX 4096
  #define HEADER_LABEL_MAX 512
  #define HEADER_VALUE_MAX 4096
*/

//#define DEBUGF(fmt, a... )
//#define DEBUGF(fmt, a... )
//fprintf(stderr, (fmt), ## a )

/// CLI option: channel name
char opt_chan_name[ACH_CHAN_NAME_MAX + 2] = {0};
/// CLI option: remote channel name
char opt_remote_chan_name[ACH_CHAN_NAME_MAX + 2] = {0};
/// CLI option: publish mode
int opt_pub = 0;
/// CLI option: subscribe mode
int opt_sub = 0;
/// CLI option: verbosity level
int opt_verbosity = 0;
/// CLI option: send only most recent frames
int opt_last = 0;
/// CLI option: synchronous mode
int opt_sync = 0;
/*
/// CLI option: read option headers
int opt_read_headers = 0;
/// CLI option: write option headers
int opt_write_headers = 0;
*/

/// argp junk

static struct argp_option options[] = {
    {
        .name = "publish",
        .key = 'p',
        .arg = NULL,
        .flags = 0,
        .doc = "Read input and publish to a channel"
    },
    {
        .name = "subscribe",
        .key = 's',
        .arg = NULL,
        .flags = 0,
        .doc = "Subscribe to a channel and write to output"
    },
    {
        .name = "synchronous",
        .key = 'c',
        .arg = NULL,
        .flags = 0,
        .doc = "Operate synchronously, only in subscribe mode"
    },
    {
        .name = "last",
        .key = 'l',
        .arg = NULL,
        .flags = 0,
        .doc = "gets the most recent message in subscribe mode (default is next)"
    },
    /*
      {
      .name = "read-headers",
      .key = 'R',
      .arg = NULL,
      .flags = 0,
      .doc = "Reads options from stdin"
      },
      {
      .name = "write-headers",
      .key = 'W',
      .arg = NULL,
      .flags = 0,
      .doc = "Write options to stdout"
      },
    */
    {
        .name = "remote-channel",
        .key = 'z',
        .arg = "channel",
        .flags = 0,
        .doc = "Channel on other end of pipe for reader writing"
    },
    {
        .name = "verbose",
        .key = 'v',
        .arg = NULL,
        .flags = 0,
        .doc = "say more stuff"
    },
    {
        .name = NULL,
        .key = 0,
        .arg = NULL,
        .flags = 0,
        .doc = NULL
    },

};

/// argp parsing function
static int parse_opt( int key, char *arg, struct argp_state *state);
/// argp program version
const char *argp_program_version = "achpipe-" ACH_VERSION_STRING;
/// argp program arguments documention
static char args_doc[] = "[-p|-s] channel";
/// argp program doc line
static char doc[] = "copy ach frames to/from stdio";
/// argp object
static struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL };


/// print stuff based on verbosity level
void verbprintf( int level, const char fmt[], ... ) {
    va_list argp;
    va_start( argp, fmt );
    if( level <= opt_verbosity ) {
        fprintf(stderr, "achpipe: ");
        vfprintf( stderr, fmt, argp );
    }
    va_end( argp );
}

/// Check test, if false, print error and abort.
void hard_assert(int test, const char fmt[], ... ) {
    if( !test ) {
        va_list argp;
        va_start( argp, fmt );
        fprintf(stderr, "achpipe FAIL: ");
        vfprintf( stderr, fmt, argp );
        va_end( argp );
        putc( '\n', stderr );
        abort();
        exit(1);
    }
}

static void *xmalloc( size_t size ) {
    void *p = malloc( size );
    if( NULL == p ) {
        perror("malloc");
        abort();
    }
    return p;
}
/*
  int read_header( char label[HEADER_LABEL_MAX],
  char value[HEADER_VALUE_MAX] ) {
  // read line
  char line[HEADER_LINE_MAX];
  size_t n_line = ach_read_line( STDIN_FILENO, line,
  HEADER_LINE_MAX );
  assert( '\0' == line[n_line] );
  hard_assert( '\n' == line[n_line-1],
  "Couldn't read header line: %s", line );
  size_t i = 0;
  // skip whitespace
  while( (' ' ==  line[i] || '\t' == line[i]) &&
  i < HEADER_LINE_MAX - 1 ) {
  i++;
  }
  assert( i < HEADER_LINE_MAX );
  // check for blank line
  if( '\n' == line[i] ) {
  assert( i + 1 == n_line );
  return 1;
  }
  // split label
  {
  size_t j = 0;
  while( line[i] != ':' &&
  j < HEADER_LABEL_MAX - 1 &&
  i < HEADER_LINE_MAX - 1) {
  label[j++] = line[i++];
  }
  hard_assert( i < HEADER_LABEL_MAX,
  "Label too long in header: %s", line);
  assert( ':' == line[i] );
  label[i++] = '\0';
  }
  // skip white space
  while( (' ' ==  line[i] || '\t' == line[i]) &&
  i < HEADER_LINE_MAX ) {
  i++;
  }
  // split value
  {
  size_t j = 0;
  while( line[i] != '\n' &&
  i < HEADER_LINE_MAX - 1 &&
  j < HEADER_VALUE_MAX - 1) {
  assert( '\0' != line[i] );
  value[j++] = line[i++];
  }
  assert( i < HEADER_LINE_MAX - 1  );
  assert( '\0' == line[i+1] );
  assert( i + 1 == n_line );
  hard_assert( j < HEADER_VALUE_MAX,
  "Value too long in header: %s", line);
  value[j] = '\0';
  }

  return 0;
  }

  void parse_headers() {
  char label[HEADER_LABEL_MAX];
  char value[HEADER_VALUE_MAX];

  while( 0 == read_header( label, value ) )  {
  if( 0 == strcasecmp( "mode", label ) ) {
  if( 0 == strcasecmp( "publish", value ) ) {
  opt_pub = 1;
  } else if ( 0 == strcasecmp( "subscribe", value ) ) {
  opt_sub = 1;
  } else {
  hard_assert( 0, "Invalid mode header: %s", value );
  }
  } else if( 0 == strcasecmp( "channel", label ) ) {
  hard_assert( strlen( label ) < ACH_CHAN_NAME_MAX-1,
  "Channel name too long" );
  strcpy( opt_chan_name, value );
  } else {
  hard_assert( 0, "Invalid header label: %s", label );
  }
  }
  }

  void write_header( char *label, char *value ) {
  char line[HEADER_LINE_MAX];
  size_t n_label = strnlen( label, HEADER_LINE_MAX );
  size_t n_value = strnlen( value, HEADER_LINE_MAX );
  hard_assert( n_label + n_value + 5 < HEADER_LINE_MAX,
  "trying to write too long header\n" );
  strcpy( line, label );
  strcat( line, ": " );
  strcat( line, value );
  strcat( line, "\n" );
  int n = strlen( line );
  assert( n + 1 < HEADER_LINE_MAX );
  int r = ach_stream_write_fill( STDOUT_FILENO, line, n );
  hard_assert( n == r,
  "Failed to write header, n = %d, r = %d\n", n, r );
  }

  void write_headers() {
  write_header( "mode", opt_pub ? "subscribe" : "publish" );
  write_header( "channel", opt_remote_chan_name[0] ? opt_remote_chan_name : opt_chan_name );
  int r = ach_stream_write_fill( STDOUT_FILENO, "\n", 1 );
  hard_assert( 1 == r, "Couldn't write header newline\n" );
  }
*/

/// publishing loop
void publish( int fd, char *chan_name )  {
    verbprintf(1, "Publishing()\n");
    assert(STDIN_FILENO == fd );
    ach_channel_t chan;
    int r;

    { // open channel
        r = ach_open( &chan, chan_name, NULL );
        if( ACH_OK != r ) {
            fprintf(stderr, "Failed to open channel %s for publish: %s\n",
                    chan_name, ach_result_to_string(r) );
            return;
        }
    }

    { // publish loop
        int max = INIT_BUF_SIZE;
        int cnt;
        char *buf = xmalloc( max );

        while(1) {
            // get size
            r = ach_stream_read_msg_size( fd, &cnt );
            if( r <= 0 ) break;
            // make sure buf can hold it
            if( cnt > max ) {
                max = cnt;
                free( buf );
                buf = xmalloc( max );
            }
            // get data
            r = ach_stream_read_msg_data( fd, buf, cnt, max );
            if( r <= 0 ) break;
            assert( cnt == r );
            // put data
            r = ach_put( &chan, buf, cnt );
            hard_assert( r == ACH_OK, "Invalid ach put %s\n",
                         ach_result_to_string( r ) );
        }
        free(buf);

    }
    ach_close( &chan );

}



/// subscribing loop
void subscribe(int fd, char *chan_name) {
    verbprintf(1, "Subscribing()\n");
    verbprintf(1, "Synchronous: %s\n", opt_sync ? "yes" : "no");
    assert(STDOUT_FILENO == fd);
    // get channel
    ach_channel_t chan;
    {
        int r = ach_open( &chan, chan_name, NULL );
        if( ACH_OK != r ) {
            fprintf(stderr, "Failed to open channel %s for subscribe: %s\n",
                    chan_name, ach_result_to_string(r) );
            return;
        }
    }
    // frame buffer
    int max = INIT_BUF_SIZE;
    char *buf = xmalloc(max);
    int t0 = 1;

    char cmd[5] = {0};

    // read loop
    while(1) {
        size_t frame_size = -1;
        int r = -1;
        int got_frame = 0;
        if( opt_sync ) {
            // wait for the pull command
            int rc = ach_stream_read_fill(STDIN_FILENO, cmd, 4);
            hard_assert(4 == rc, "Invalid command read: %d\n", rc );
            verbprintf(2, "Command %s\n", cmd );
        }
        // read the data
        do {
            if( opt_sync ) {
                // parse command
                if ( 0 == strcmp("next", cmd ) ) {
                    r = ach_wait_next(&chan, buf, max, &frame_size,  NULL ) ;
                }else if ( 0 == strcmp("last", cmd ) ){
                    r = ach_wait_last(&chan, buf, max, &frame_size,  NULL ) ;
                }else {
                    hard_assert(0, "Invalid command: %s\n", cmd );
                }
            } else {
                // push the data
                r = opt_last ?
                    ach_wait_last(&chan, buf, max, &frame_size,  NULL ) :
                    ach_wait_next(&chan, buf, max, &frame_size,  NULL ) ;
            }
            // check return code
            if( ACH_OK != r )  {
                // enlarge buffer and retry on overflow
                if( ACH_OVERFLOW == r ) {
                    int fs = frame_size;
                    assert(fs > max );
                    free(buf);
                    max = frame_size;
                    buf = xmalloc( max );
                }else {
                    // abort on other errors
                    hard_assert( t0 || r == ACH_MISSED_FRAME,
                                 "sub: ach_error: %s\n",
                                 ach_result_to_string(r) );
                    got_frame = 1;
                }
            } else {
                got_frame = 1;
            }
        }while( !got_frame );

        verbprintf(2, "Got ach frame %d\n", frame_size );

        // stream send
        {
            size_t r = ach_stream_write_msg( fd, buf, frame_size );
            hard_assert( r == frame_size + ACH_STREAM_PREFIX_SIZE,
                         "Invalid data write, r: %d, frame: %d\n",
                         r, frame_size );
            if( opt_sync ) {
                fsync( fd ); // fails w/ sbcl, and maybe that's ok
            }
            verbprintf( 2, "Printed output\n");
        }
        t0 = 0;
    }
    free(buf);
    ach_close( &chan );
}

/// main
int main( int argc, char **argv ) {
    argp_parse (&argp, argc, argv, 0, NULL, NULL);

    /*
      hard_assert( ! ( opt_write_headers &&  opt_read_headers ),
      "can't read and write headers\n" );

      if( opt_read_headers ) {
      parse_headers();
      }else if( opt_write_headers ) {
      write_headers();
      }
    */

    // validate arguments
    hard_assert( 0 < strlen( opt_chan_name ),
                 "must specify channel\n" );
    hard_assert( ! ( opt_pub &&  opt_sub ),
                 "must specify publish or subscribe mode\n" );
    hard_assert( opt_pub || opt_sub ,
                 "must specify publish xor subscribe mode\n" ) ;

    // maybe print
    verbprintf( 1, "Channel: %s\n", opt_chan_name );
    verbprintf( 1, "Publish: %s\n",  opt_pub ? "yes" : "no" );
    verbprintf( 1, "Subscribe: %s\n", opt_sub ? "yes" : "no" );

    // run
    if (opt_pub) {
        publish( STDIN_FILENO, opt_chan_name );
    } else if (opt_sub) {
        subscribe( STDOUT_FILENO, opt_chan_name );
    } else {
        assert(0);
    }
    return 0;
}


static int parse_opt( int key, char *arg, struct argp_state *state) {
    (void) state; // ignore unused parameter
    switch(key) {
    case 'p':
        opt_pub = 1;
        break;
    case 's':
        opt_sub = 1;
        break;
    case 'v':
        opt_verbosity ++;
        break;
    case 'l':
        opt_last = 1;
        break;
    case 'c':
        opt_sync = 1;
        break;
        /*
          case 'R':
          opt_read_headers = 1;
          break;
          case 'W':
          opt_write_headers = 1;
          break;
        */
    case 'z':
        hard_assert( strlen( arg ) < ACH_CHAN_NAME_MAX-1,
                     "Channel name argument to long" );
        strncpy( opt_remote_chan_name, arg, ACH_CHAN_NAME_MAX );
    case 0:
        hard_assert( strlen( arg ) < ACH_CHAN_NAME_MAX-1,
                     "Channel name argument to long" );
        strncpy( opt_chan_name, arg, ACH_CHAN_NAME_MAX );
        break;
    }
    return 0;
}