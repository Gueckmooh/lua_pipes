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
#include <fcntl.h>

#define LUA_PIPEHANDLE "PIPE*"

#ifndef isclosed
#define isclosed(p)	((p)->closef == NULL)
#endif

#define READ  STDIN_FILENO
#define WRITE STDOUT_FILENO
#define ERR   STDERR_FILENO

typedef struct luaL_Spipe {
  int fdin, fdout, fderr;
  pid_t pid;
  lua_CFunction closef;
  int running;
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

static int
aux_read (lua_State *L, int mode) {
  LSpipe *p = tolpipe(L);
  static char buffer[4096];
  int count = 0;
  switch (mode)
    {
    case READ:
      count = read(p->fdout, buffer, 4096);
      break;
    case ERR:
      count = read(p->fderr, buffer, 4096);
      break;
    default:
      lua_pushnil (L);
      return 1;
    }
  if(count < 0 && errno == EAGAIN) {
    /* There is nothing to read */
    lua_pushnil (L);
    return 1;
  }
  else if(count >= 0) {
    /* We read count bytes */
    buffer[count+1] = '\0';
    lua_pushstring (L, buffer);
  }
  else perror("read");
  return 1;
}

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
  p->closef = &pipe_fclose;
  return p;
}

static int
pipe_open (lua_State *L)
{
  const char* cmd = lua_tostring(L,1);
  int inf = 0, outf = 0, errf = 0;
  int pid = popen2(cmd, &inf, &outf, &errf);
  int flags;
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
    flags = fcntl(outf, F_GETFL, 0);
    if (fcntl(outf, F_SETFL, flags | O_NONBLOCK)) perror("fcntl");;
    flags = fcntl(errf, F_GETFL, 0);
    if (fcntl(errf, F_SETFL, flags | O_NONBLOCK)) perror("fcntl");
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
  topipe(L);
  return aux_read (L, READ);
}

static int
p_is_running (lua_State *L)
{
  topipe(L);
  return pipe_is_running(L);
}

static int
p_read_err (lua_State *L) {
  topipe(L);
  return aux_read (L, ERR);
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

static int p_gc (lua_State *L) {
  LSpipe *p = tolpipe(L);
  if (!isclosed(p) && p->fdin != 0 && p->fdout != 0)
    aux_close(L);  /* ignore closed and incompletely open files */
  return 0;
}

/* END OF THE PIPE METATABLE */

static const luaL_Reg pipelib[] = {
  {"close", pipe_close},
  {"read", p_read},
  {"read_err", p_read_err},
  {"write", p_write},
  {"is_running", p_is_running},
  {"open", pipe_open},
  {NULL, NULL}
};


static const luaL_Reg plib[] = {
  {"close", p_close},
  {"read", p_read},
  {"read_err", p_read_err},
  {"write", p_write},
  {"is_running", p_is_running},
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
