//===-- klee_init_env.c ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/klee.h"
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include "fd.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

static void __emit_error(const char *msg) {
  klee_report_error(__FILE__, __LINE__, msg, "user.err");
}

/* Helper function that converts a string to an integer, and
   terminates the program with an error message is the string is not a
   proper number */
static long int __str_to_int(char *s, const char *error_msg) {
  long int res = 0;
  char c;

  if (!*s)
    __emit_error(error_msg);

  if (*s == '0' && (*(s+1) == 'X' || *(s+1) == 'x')) {
    /* hexadecimal */
    s += 2;
    while ((c = *s++)) {
      if (c == '\0')
        break;
      else if (c>='0' && c<='9')
        res = res*16 + (c-'0');
      else if (c>='A' && c<='F')
        res = res*16 + (c-'A') + 10;
      else if (c>='a' && c<='f')
        res = res*16 + (c-'a') + 10;
      else
        __emit_error(error_msg);
    }
  }

  else if (*s == '0') {
    /* octal */
    s++;
    while ((c = *s++)) {
      if (c == '\0')
        break;
      else if (c>='0' && c<='7')
        res = res*10 + (c-'0');
      else
        __emit_error(error_msg);
    }
  }

  else {
    /* decimal */
    while ((c = *s++)) {
      if (c == '\0')
        break;
      else if (c>='0' && c<='9')
        res = res*10 + (c-'0');
      else
        __emit_error(error_msg);
    }
  }

  return res;
}

static int __isprint(const char c) {
  /* Assume ASCII */
  return (32 <= c && c <= 126);
}

static int __getodigit(const char c) {
  return (('0' <= c) && (c <= '7')) ? (c - '0') : -1;
}

static int __getxdigit(const char c) {
  return (('0' <= c) && (c <= '9')) ? (c - '0') :
         (('A' <= c) && (c <= 'F')) ? (c - 'A') + 10:
         (('a' <= c) && (c <= 'f')) ? (c - 'a') + 10: -1;
}

/* Convert in-place, but it's okay because no escape sequences "expand". */
static size_t __convert_escape_sequences(char *s)
{
  char *d0 = s, *d = s;

  while (*s) {
    if (*s != '\\')
      *d++ = *s++;
    else {
      s++;
      switch (*s++) {
        int n[3];
      default:  *d++ = s[-1]; break;
      case 'a': *d++ = '\a'; break;
      case 'b': *d++ = '\b'; break;
      case 'f': *d++ = '\f'; break;
      case 'n': *d++ = '\n'; break;
      case 'r': *d++ = '\r'; break;
      case 't': *d++ = '\t'; break;
      case 'v': *d++ = '\v'; break;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
        n[0] = __getodigit(s[-1]);
        if ((n[1] = __getodigit(*s)) >= 0) {
          s++;
          if ((n[2] = __getodigit(*s)) >= 0) {
            s++;
            *d++ = (n[0] << 6) | (n[1] << 3) | n[2];
          }
          else
            *d++ = (n[0] << 3) | n[1];
        }
        else
          *d++ = n[0];
        break;
      case 'x':
        if ((n[0] = __getxdigit(*s)) >= 0) {
          s++;
          if ((n[1] = __getxdigit(*s)) >= 0) {
            s++;
            *d++ = (n[0] << 4) | n[1];
          }
          else
            *d++ = n[0];
        }
        else /* error */
          __emit_error("invalid escape sequence");
        break;
      }
    }
  }
  return d - d0;
}

static int __streq(const char *a, const char *b) {
  while (*a == *b) {
    if (!*a)
      return 1;
    a++;
    b++;
  }
  return 0;
}

static char *__get_sym_str(int numChars, char *name) {
  int i;
  char *s = malloc(numChars+1);
  klee_mark_global(s);
  klee_make_symbolic(s, numChars+1, name);

  for (i=0; i<numChars; i++)
    klee_prefer_cex(s, __isprint(s[i]));

  s[numChars] = '\0';
  return s;
}

static void __add_arg(int *argc, char **argv, char *arg, int argcMax) {
  if (*argc==argcMax) {
    __emit_error("too many arguments for klee_init_env");
  } else {
    argv[*argc] = arg;
    (*argc)++;
  }
}

void klee_init_env(int* argcPtr, char*** argvPtr) {
  int argc = *argcPtr;
  char** argv = *argvPtr;

  int new_argc = 0, n_args;
  char* new_argv[1024];
  unsigned max_len, min_argvs, max_argvs;
  unsigned sym_files = 0, sym_file_len = 0;
  unsigned sym_streams = 0, sym_stream_len = 0;
  unsigned sym_dgrams = 0, sym_dgram_len = 0;
  int sym_stdout_flag = 0;
  int save_all_writes_flag = 0;
  int one_line_streams_flag = 0;
  fill_info_t stream_fill_info[100];
  unsigned n_stream_fill_info = 0;
  fill_info_t dgram_fill_info[100];
  unsigned n_dgram_fill_info = 0;
  int fd_fail = 0;
  int fd_short = 0;
  char** final_argv;
  char sym_arg_name[5] = "arg";
  unsigned sym_arg_num = 0;
  int k=0, i;

  sym_arg_name[4] = '\0';

  // Recognize --help when it is the sole argument.
  if (argc == 2 && __streq(argv[1], "--help")) {
  __emit_error("klee_init_env\n\n\
usage: (klee_init_env) [options] [program arguments]\n\
  -sym-arg <N>              - Replace by a symbolic argument with length N\n\
  -sym-args <MIN> <MAX> <N> - Replace by at least MIN arguments and at most\n\
                              MAX arguments, each with maximum length N\n\
  -sym-files <NUM> <N>      - Make stdin and up to NUM symbolic files, each\n\
                              with maximum size N.\n\
  -sym-stdout               - Make stdout symbolic.\n\
  -save-all-writes\n\
  -max-fail <N>             - Allow up to <N> injected failures\n\
  -fd-fail                  - Shortcut for '-max-fail 1'\n\
  -fd-short                 - Allow returning amounts that fall short of those requested\n\
  -sym-streams <N> <LEN>    - Prepare <N> streams of <LEN> bytes each\n\
  -sym-datagrams <N> <LEN>  - Prepare <N> datagrams of <LEN> bytes each\n\
  -one-line-streams         - Constrain the streams to single lines\n\
  -fill-{streams|datagrams} - Fill (Concretize) streams or datagrams with:\n\
     <OFF> set <LEN> <VAL>      <LEN> bytes of <VAL> from <OFF>\n\
     <OFF> copy <STR>           <STR> from <OFF>\n\n");
  }

  while (k < argc) {
    if (__streq(argv[k], "--sym-arg") || __streq(argv[k], "-sym-arg")) {
      const char *msg = "--sym-arg expects an integer argument <max-len>";
      if (++k == argc)
        __emit_error(msg);

      max_len = __str_to_int(argv[k++], msg);
      sym_arg_name[3] = '0' + sym_arg_num++;
      __add_arg(&new_argc, new_argv,
                __get_sym_str(max_len, sym_arg_name),
                1024);
    }
    else if (__streq(argv[k], "--sym-args") || __streq(argv[k], "-sym-args")) {
      const char *msg =
        "--sym-args expects three integer arguments <min-argvs> <max-argvs> <max-len>";

      if (k+3 >= argc)
        __emit_error(msg);

      k++;
      min_argvs = __str_to_int(argv[k++], msg);
      max_argvs = __str_to_int(argv[k++], msg);
      max_len = __str_to_int(argv[k++], msg);

      n_args = klee_range(min_argvs, max_argvs+1, "n_args");
      for (i=0; i < n_args; i++) {
        sym_arg_name[3] = '0' + sym_arg_num++;
        __add_arg(&new_argc, new_argv,
                  __get_sym_str(max_len, sym_arg_name),
                  1024);
      }
    }
    else if (__streq(argv[k], "--sym-files") || __streq(argv[k], "-sym-files")) {
      const char* msg = "--sym-files expects two integer arguments <no-sym-files> <sym-file-len>";

      if (k+2 >= argc)
        __emit_error(msg);

      k++;
      sym_files = __str_to_int(argv[k++], msg);
      sym_file_len = __str_to_int(argv[k++], msg);

    }
    else if (__streq(argv[k], "--sym-stdout") || __streq(argv[k], "-sym-stdout")) {
      sym_stdout_flag = 1;
      k++;
    }
    else if (__streq(argv[k], "--save-all-writes") || __streq(argv[k], "-save-all-writes")) {
      save_all_writes_flag = 1;
      k++;
    }
    else if (__streq(argv[k], "--max-fail") || __streq(argv[k], "-max-fail")) {
      const char *msg = "--max-fail expects an integer argument <max-failures>";
      if (++k == argc)
        __emit_error(msg);

      fd_fail = __str_to_int(argv[k++], msg);
    }
    else if (__streq(argv[k], "--fd-fail") || __streq(argv[k], "-fd-fail")) {
      fd_fail = 1;
      k++;
    }
    else if (__streq(argv[k], "--fd-short") || __streq(argv[k], "-fd-short")) {
      fd_short = 1;
      k++;
    }
    /* "sym-connections": for backward compatability */
    else if (__streq(argv[k], "--sym-connections") || __streq(argv[k], "-sym-connections")) {
      const char* msg = "--sym-connections expects two integer arguments <no-connections> <bytes-per-connection>";

      if (k+2 >= argc)
        __emit_error(msg);

      k++;
      sym_streams = __str_to_int(argv[k++], msg);
      sym_stream_len = __str_to_int(argv[k++], msg);
    }
    else if (__streq(argv[k], "--sym-streams") || __streq(argv[k], "-sym-streams")) {
      const char* msg = "--sym-streams expects two integer arguments <no-streams> <bytes-per-stream>";

      if (k+2 >= argc)
        __emit_error(msg);

      k++;
      sym_streams = __str_to_int(argv[k++], msg);
      sym_stream_len = __str_to_int(argv[k++], msg);
    }
    else if (__streq(argv[k], "--sym-datagrams") || __streq(argv[k], "-sym-datagrams")) {
      const char* msg = "--sym-datagrams expects two integer arguments <no-datagrams> <bytes-per-datagram>";

      if (k+2 >= argc)
        __emit_error(msg);

      k++;
      sym_dgrams = __str_to_int(argv[k++], msg);
      sym_dgram_len = __str_to_int(argv[k++], msg);
    }
    else if (__streq(argv[k], "--one-line-streams") || __streq(argv[k], "-one-line-streams")) {
      one_line_streams_flag = 1;
      k++;
    }
    else if (__streq(argv[k], "--fill-streams") || __streq(argv[k], "-fill-streams")) {
      const char* msg = "--fill-streams expects arguments <offset> \"set\" <length> <value> or <offset> \"copy\" <string>";
      const char* msg2 = "--fill-streams: too many blocks";

      if (n_stream_fill_info >= sizeof(stream_fill_info) / sizeof(*stream_fill_info))
        __emit_error(msg2);
      if (k + 2 >= argc)
        __emit_error(msg);
      k++;
      stream_fill_info[n_stream_fill_info].offset = __str_to_int(argv[k++], msg);
      if (__streq(argv[k], "set")) {
        k++;
        if (k + 1 >= argc)
          __emit_error(msg);
        stream_fill_info[n_stream_fill_info].fill_method = fill_set;
        stream_fill_info[n_stream_fill_info].length      = __str_to_int(argv[k++], msg);
        stream_fill_info[n_stream_fill_info].arg.value   = __str_to_int(argv[k++], msg);
        n_stream_fill_info++;
      }
      else if (__streq(argv[k], "copy")) {
        k++;
        if (k >= argc)
          __emit_error(msg);
        stream_fill_info[n_stream_fill_info].fill_method = fill_copy;
        stream_fill_info[n_stream_fill_info].arg.string  = argv[k++];
        stream_fill_info[n_stream_fill_info].length      =
          __convert_escape_sequences(stream_fill_info[n_stream_fill_info].arg.string);
        n_stream_fill_info++;
      }
      else
        __emit_error(msg);

    }
    else if (__streq(argv[k], "--fill-datagrams") || __streq(argv[k], "-fill-datagrams")) {
      const char* msg = "--fill-datagrams expects arguments <offset> \"set\" <length> <value> or <offset> \"copy\" <string>";
      const char* msg2 = "--fill-datagrams: too many blocks";

      if (n_dgram_fill_info >= sizeof(dgram_fill_info) / sizeof(*dgram_fill_info))
        __emit_error(msg2);
      if (k + 2 >= argc)
        __emit_error(msg);
      k++;
      dgram_fill_info[n_dgram_fill_info].offset = __str_to_int(argv[k++], msg);
      if (__streq(argv[k], "set")) {
        k++;
        if (k + 1 >= argc)
          __emit_error(msg);
        dgram_fill_info[n_dgram_fill_info].fill_method = fill_set;
        dgram_fill_info[n_dgram_fill_info].length      = __str_to_int(argv[k++], msg);
        dgram_fill_info[n_dgram_fill_info].arg.value   = __str_to_int(argv[k++], msg);
        n_dgram_fill_info++;
      }
      else if (__streq(argv[k], "copy")) {
        k++;
        if (k >= argc)
          __emit_error(msg);
        dgram_fill_info[n_dgram_fill_info].fill_method = fill_copy;
        dgram_fill_info[n_dgram_fill_info].arg.string  = argv[k++];
        dgram_fill_info[n_dgram_fill_info].length      =
          __convert_escape_sequences(dgram_fill_info[n_dgram_fill_info].arg.string);
        n_dgram_fill_info++;
      }
      else
        __emit_error(msg);

    }
    else {
      /* simply copy arguments */
      __add_arg(&new_argc, new_argv, argv[k++], 1024);
    }
  }

  final_argv = (char**) malloc((new_argc+1) * sizeof(*final_argv));
  klee_mark_global(final_argv);
  memcpy(final_argv, new_argv, new_argc * sizeof(*final_argv));
  final_argv[new_argc] = 0;

  *argcPtr = new_argc;
  *argvPtr = final_argv;

  klee_init_fds(sym_files, sym_file_len,
                sym_stdout_flag, save_all_writes_flag,
                sym_streams, sym_stream_len,
                sym_dgrams, sym_dgram_len,
                fd_fail, fd_short, one_line_streams_flag,
                stream_fill_info, n_stream_fill_info,
                dgram_fill_info, n_dgram_fill_info);
}

