#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>

#define LUA_PIPEHANDLE "PIPE*"

#ifndef isclosed
#define isclosed(p)	((p)->closef == NULL)
#endif

#define READ    STDIN_FILENO
#define WRITE   STDOUT_FILENO
#define ERR     STDERR_FILENO
#define BUFSIZE 4096
#define POLL    1
#define NOPOLL  0

typedef struct luaL_Spipe {
  int fdin, fdout, fderr;
  pid_t pid;
  lua_CFunction closef;
  int running;
  char outbuffer[4096];
  char errbuffer[4096];
  int outb;
  int errb;
  int tryerr, tryout;
} luaL_Spipe;

#define LSpipe luaL_Spipe

#define tolpipe(L)    ((luaL_Spipe *)luaL_checkudata(L, 1, LUA_PIPEHANDLE))

static inline pid_t
popen2(const char *command, int *infp, int *outfp, int *errfp)
{
  int p_stdin[2], p_stdout[2], p_stderr[2];
  pid_t pid;

  if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr) != 0)
    return -1;

  pid = fork();

  if (pid < 0)
    return pid;
  else if (pid == 0)
    {
      close(p_stdin[WRITE]);
      dup2(p_stdin[READ], READ);

      close(p_stdout[READ]);
      dup2(p_stdout[WRITE], WRITE);

      close(p_stderr[READ]);
      dup2(p_stderr[WRITE], ERR);

      execl("/bin/sh", "sh", "-c", command, NULL);
      // if we get here, then something's wrong!
      perror("execl");
      exit(1);
    }

  // note that it is NB to close the other ends of the pipes!
  if (infp == NULL)
    close(p_stdin[WRITE]);
  else {
    close(p_stdin[READ]);
    *infp = p_stdin[WRITE];
  }

  if (outfp == NULL)
    close(p_stdout[READ]);
  else {
    close(p_stdout[WRITE]);
    *outfp = p_stdout[READ];
  }

  if (errfp == NULL)
    close(p_stderr[READ]);
  else {
    close(p_stderr[WRITE]);
    *errfp = p_stderr[READ];
  }

  return pid;
}

static int
p_close (lua_State *L);

static int pipe_close (lua_State *L) {
  return p_close(L);
}

static int
aux_close (lua_State *L) {
  LSpipe *p = tolpipe(L);
  volatile lua_CFunction cf = p->closef;
  p->closef = NULL;  /* mark stream as closed */
  return (*cf)(L);  /* close it */
}

static inline int
aux_fillbuf (lua_State *L, int mode, int do_poll)
{
  LSpipe *p = tolpipe(L);
  /* static char buffer[4096]; */
  int count = 0;
  int* pos;
  char* buffer;
  switch (mode)
    {
    case READ:
      if (do_poll)
        {
          struct pollfd pp = { p->fdout, POLLIN, 0 };
          int i;
          if ((i = poll (&pp, 1, 10)) > 0)
            count = read(p->fdout, p->outbuffer, BUFSIZE-1-p->outb);
          else
            return 0;
        }
      else
        count = read(p->fdout, p->outbuffer, BUFSIZE-1-p->outb);
      p->outb += count;
      pos = &p->outb;
      buffer = p->outbuffer;
      break;
    case ERR:
      if (do_poll)
        {
          struct pollfd pp = { p->fderr, POLLIN, 0 };
          if (poll (&pp, 1, 10) > 1)
            count = read(p->fdout, p->outbuffer, BUFSIZE-1-p->outb);
          else
            return 0;
        }
      else
        count = read(p->fderr, p->errbuffer, BUFSIZE-1-p->outb);
      p->errb += count;
      pos = &p->errb;
      buffer = p->errbuffer;
      break;
    default:
      lua_pushnil (L);
      return 1;
    }
  if(count < 0 && errno == EAGAIN) {
    /* There is nothing to read */
    return 0;
  }
  else if(count >= 0) {
    /* We read count bytes */
    buffer[*pos] = '\0';
    return 1;
  }
  else perror("read");
  return 0;
}

static inline
int read_all(lua_State *L, char* buffer, int* pos)
{
  lua_pushstring (L, (const char*) buffer);
  *pos = 0;
  return 1;
}

static inline
int read_line(lua_State *L, char* buffer, int* pos, int chop)
{
  char* str;
  char* npos = strstr (buffer, "\n");
  if (*pos == 0)
    return 0;
  if (npos != NULL)
    {
      long ll = npos - buffer;
      if (ll != 0)
        {
          str = (char*) malloc (ll + 2);
          strncpy(str, buffer, ll);
          if (chop)
            str[ll] = '\0';
          else
            {
              str[ll] = '\n';
              str[ll+1] = '\0';
            }
        }
      else
        {
          str = (char*) malloc (2);
          if (chop)
            *str = '\0';
          else
            {
              *str = '\n';
              *(str + 1) = '\0';
            }
        }
    }
  else
    {
      return read_all (L, buffer, pos);
    }
  lua_pushstring (L, (const char*) str);
  free (str);
  *pos -= npos - buffer +1;
  strcpy (buffer, npos + 1);
  return 1;
}

static int
aux_read (lua_State *L, int mode, int do_poll)
{
  LSpipe *p = tolpipe (L);
  int *pos;
  char* buffer;
  int success = 0;
  const char *s = NULL;
  if (!lua_isnone(L, 2))  /* no argument? */
      s = luaL_checkstring(L, 2);
  switch (mode)
    {
    case READ:
      pos = &p->outb;
      buffer = p->outbuffer;
      break;
    case ERR:
      pos = &p->errb;
      buffer = p->errbuffer;
      break;
    default:
      lua_pushnil (L);
      return 1;
      break;
    }
  if (aux_fillbuf(L, mode, do_poll))
    {
      if (s)
        {
          if (*s == '*') s++;  /* skip optional '*' (for compatibility) */
          switch (*s) {
            /* case 'n':  /\* number *\/ */
            /*   success = read_number(L, f); */
            /*   break; */
            case 'l':  /* line */
              success = read_line(L, buffer, pos, 1);
              break;
            case 'L':  /* line with end-of-line */
              success = read_line(L, buffer, pos, 0);
              break;
          case 'a':  /* file */
            success = read_all(L, buffer, pos);
            break;
          default:
            return luaL_argerror(L, 2, "invalid format");
          }
        }
      else success = read_all(L, buffer, pos);
    }
  if (!success)
    {
      lua_pop(L, 1);  /* remove last result */
      lua_pushnil(L);  /* push nil instead */
    }
  return 1;
}

/* static int io_readline (lua_State *L) { */
/*   LSpipe *p = (LSpipe *)lua_touserdata(L, lua_upvalueindex(1)); */
/*   int i; */
/*   int n = (int)lua_tointeger(L, lua_upvalueindex(2)); */
/*   if (isclosed(p))  /\* file is already closed? *\/ */
/*     return luaL_error(L, "file is already closed"); */
/*   lua_settop(L , 1); */
/*   luaL_checkstack(L, n, "too many arguments"); */
/*   for (i = 1; i <= n; i++)  /\* push arguments to 'g_read' *\/ */
/*     lua_pushvalue(L, lua_upvalueindex(3 + i)); */
/*   n = g_read(L, p->f, 2);  /\* 'n' is number of results *\/ */
/*   lua_assert(n > 0);  /\* should return at least a nil *\/ */
/*   if (lua_toboolean(L, -n))  /\* read at least one value? *\/ */
/*     return n;  /\* return them *\/ */
/*   else {  /\* first result is nil: EOF or error *\/ */
/*     if (n > 1) {  /\* is there error information? *\/ */
/*       /\* 2nd result is error message *\/ */
/*       return luaL_error(L, "%s", lua_tostring(L, -n + 1)); */
/*     } */
/*     if (lua_toboolean(L, lua_upvalueindex(3))) {  /\* generator created file? *\/ */
/*       lua_settop(L, 0); */
/*       lua_pushvalue(L, lua_upvalueindex(1)); */
/*       aux_close(L);  /\* close it *\/ */
/*     } */
/*     return 0; */
/*   } */
/* } */


static inline LSpipe *
newprepipe (lua_State *L) {
  LSpipe *p = (LSpipe *)lua_newuserdata(L, sizeof(LSpipe));
  p->closef = NULL;  /* mark file handle as 'closed' */
  luaL_setmetatable(L, LUA_PIPEHANDLE);
  return p;
}

int
pipe_fclose (lua_State *L)
{
  LSpipe *p = tolpipe(L);
  close (p->fdin);
  close (p->fdout);
  close (p->fderr);

  if (p->running)
    {
      kill (p->pid, SIGINT);
      waitpid (p->pid, NULL, 0);
    }

  return 0;
}

static inline LSpipe *
newpipe (lua_State *L) {
  LSpipe *p = newprepipe(L);
  p->fdin = -1;
  p->fdout = -1;
  p->fderr = -1;
  p->pid = 0;
  p->running = 0;
  p->outb = 0;
  p->errb = 0;
  p->closef = &pipe_fclose;
  return p;
}

static int
pipe_open (lua_State *L)
{
  const char* cmd = lua_tostring(L,1);
  int inf = 0, outf = 0, errf = 0;
  int pid = popen2(cmd, &inf, &outf, &errf);
  LSpipe *file;
  if (pid == -1) {
    lua_pushboolean(L,0);
    return 1;
  } else {
    file = newpipe (L);
    file->fdin = inf;
    file->fdout = outf;
    file->fderr = errf;
    file->pid = pid;
    file->running = 1;
    file->tryerr = 0;
    file->tryout = 0;
    return 1;
  }
}

static LSpipe* topipe (lua_State *L) {
  LSpipe *p = tolpipe(L);
  if (isclosed(p))
    luaL_error(L, "attempt to use a closed pipe");
  lua_assert(p->f);
  return p;
}

static int
pipe_is_running (lua_State *L)
{
  LSpipe *p = tolpipe (L);
  if (p->running)
    {
      pid_t res = waitpid(p->pid, NULL, WNOHANG);
      if (res > 0)
        p->running = 0;
    }
  lua_pushboolean (L, p->running);
  return 1;
}

/* BEGINING OF THE PIPE METATABLE */
static int
p_close (lua_State *L) {
  topipe(L);  /* make sure argument is an open stream */
  return aux_close(L);
}

static int
p_read (lua_State *L) {
  LSpipe *p = topipe(L);
  if (p->tryout)
    {
      int flags = fcntl(p->fdout, F_GETFL, 0);
      if (fcntl(p->fdout, F_SETFL, flags & (~O_NONBLOCK))) perror("fcntl");
      p->tryout = 1;
    }
  return aux_read (L, READ, POLL);
}

static int
p_try_read (lua_State *L) {
  LSpipe *p = topipe(L);
  if (!p->tryout)
    {
      int flags = fcntl(p->fdout, F_GETFL, 0);
      if (fcntl(p->fdout, F_SETFL, flags | O_NONBLOCK)) perror("fcntl");
      p->tryout = 1;
    }
  return aux_read (L, READ, NOPOLL);
}

static int
p_is_running (lua_State *L)
{
  topipe(L);
  return pipe_is_running(L);
}

static int
p_read_err (lua_State *L) {
  LSpipe *p = topipe(L);
  if (p->tryout)
    {
      int flags = fcntl(p->fderr, F_GETFL, 0);
      if (fcntl(p->fderr, F_SETFL, flags & (~O_NONBLOCK))) perror("fcntl");
      p->tryout = 1;
    }
  return aux_read (L, ERR, POLL);
}

static int
p_try_read_err (lua_State *L) {
  LSpipe *p = topipe(L);
  if (!p->tryout)
    {
      int flags = fcntl(p->fderr, F_GETFL, 0);
      if (fcntl(p->fderr, F_SETFL, flags | O_NONBLOCK)) perror("fcntl");
      p->tryout = 1;
    }
  return aux_read (L, ERR, NOPOLL);
}

static int
p_write (lua_State *L) {
  LSpipe *p = topipe(L);
  if (lua_isnone(L, 2))  /* no argument? */
    luaL_error(L, "need a string to write");
  const char* cmd = lua_tostring(L,2);
  lua_pushnil(L);
  write (p->fdin, cmd, strlen(cmd));
  return 1;
}

static int
p_flush (lua_State *L)
{
  LSpipe *p = topipe(L);
  fsync (p->fdin);
  lua_pushnil (L);
  return 1;
}

static int p_gc (lua_State *L) {
  LSpipe *p = tolpipe(L);
  if (!isclosed(p) && p->fdin != 0 && p->fdout != 0)
    aux_close(L);  /* ignore closed and incompletely open files */
  return 0;
}

/* END OF THE PIPE METATABLE */

int
pipe_readlines (lua_State *L)
{
  LSpipe *p = ((luaL_Spipe *)luaL_checkudata(L, lua_upvalueindex(1),
                                             LUA_PIPEHANDLE));
  int* pos = &p->outb;
  char* buffer = p->outbuffer;
  int success = read_line(L, buffer, pos, 1);
  if (!success)
    {
      lua_pop(L, 1);  /* remove last result */
      lua_pushnil(L);  /* push nil instead */
    }
  return 1;
}

int
p_lines (lua_State *L)
{
  topipe (L);
  aux_fillbuf(L,  READ, POLL);
  lua_pushcclosure (L, pipe_readlines, 1);
  return 1;
}

int
pipe_readlines_err (lua_State *L)
{
  LSpipe *p = ((luaL_Spipe *)luaL_checkudata(L, lua_upvalueindex(1),
                                             LUA_PIPEHANDLE));
  int* pos = &p->errb;
  char* buffer = p->errbuffer;
  int success = read_line(L, buffer, pos, 1);
  if (!success)
    {
      lua_pop(L, 1);  /* remove last result */
      lua_pushnil(L);  /* push nil instead */
    }
  return 1;
}

int
p_lines_err (lua_State *L)
{
  topipe (L);
  aux_fillbuf(L, ERR, POLL);
  lua_pushcclosure (L, pipe_readlines_err, 1);
  return 1;
}


static const luaL_Reg pipelib[] = {
  {"close", pipe_close},
  {"read", p_read},
  {"try_read", p_try_read},
  {"read_err", p_read_err},
  {"try_read_err", p_try_read_err},
  {"write", p_write},
  {"flush", p_flush},
  {"is_running", p_is_running},
  {"open", pipe_open},
  {"lines", p_lines},
  {"lines_err", p_lines_err},
  {NULL, NULL}
};


static const luaL_Reg plib[] = {
  {"close", p_close},
  {"read", p_read},
  {"try_read", p_try_read},
  {"read_err", p_read_err},
  {"try_read_err", p_try_read_err},
  {"write", p_write},
  {"flush", p_flush},
  {"is_running", p_is_running},
  {"lines", p_lines},
  {"lines_err", p_lines_err},
  {"__gc", p_gc},
  {NULL, NULL}
};

static void createmeta (lua_State *L) {
  luaL_newmetatable(L, LUA_PIPEHANDLE);  /* create metatable for file handles */
  lua_pushvalue(L, -1);  /* push metatable */
  lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
  luaL_setfuncs(L, plib, 0);  /* add file methods to new metatable */
  lua_pop(L, 1);  /* pop new metatable */
}

LUAMOD_API int luaopen_pipe (lua_State *L) {
  luaL_newlib(L, pipelib);  /* new module */
  createmeta(L);
  return 1;
}

/* Local Variables: */
/* flycheck-clang-include-path: (quote ("/usr/include/lua5.3")) */
/* End: */
