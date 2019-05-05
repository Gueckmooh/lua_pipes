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

#define LUA_PIPEHANDLE "PIPE*"

#ifndef isclosed
#define isclosed(p)	((p)->closef == NULL)
#endif

#define READ 0
#define WRITE 1

typedef struct luaL_Spipe {
  /* FILE* f; */
  int fdin, fdout;
  pid_t pid;
  lua_CFunction closef;
} luaL_Spipe;

#define LSpipe luaL_Spipe

#define tolpipe(L)    ((luaL_Spipe *)luaL_checkudata(L, 1, LUA_PIPEHANDLE))

static inline pid_t
popen2(const char *command, int *infp, int *outfp)
{
  int p_stdin[2], p_stdout[2];
  pid_t pid;

  if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0)
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

  kill (p->pid, SIGINT);
  waitpid (p->pid, NULL, 0);

  return 0;
}

static inline LSpipe *
newpipe (lua_State *L) {
  LSpipe *p = newprepipe(L);
  p->fdin = -1;
  p->fdout = -1;
  p->pid = 0;
  p->closef = &pipe_fclose;
  return p;
}

static int
pipe_open (lua_State *L)
{
  const char* cmd = lua_tostring(L,1);
  int inf = 0, outf = 0;
  printf ("-> %s\n", cmd);
  int pid = popen2(cmd,&inf,&outf);
  LSpipe *file;
  if (pid == -1) {
    lua_pushboolean(L,0);
    return 1;
  } else {
    file = newpipe (L);
    file->fdin = inf;
    file->fdout = outf;
    file->pid = pid;
    return 1;
  }
}

static LSpipe* topipe (lua_State *L) {
  LSpipe *p = tolpipe(L);
  if (isclosed(p))
    luaL_error(L, "attempt to use a closed file");
  lua_assert(p->f);
  return p;
}


/* BEGINING OF THE PIPE METATABLE */
static int
p_close (lua_State *L) {
  topipe(L);  /* make sure argument is an open stream */
  return aux_close(L);
}

static int
p_read (lua_State *L) {
  LSpipe *p = tolpipe(L);
  static char str[4096];
  int pos = 0;
  pos = read (p->fdout, str+pos, 4096);
  str[pos+1] = '\0';
  lua_pushstring (L, str);
  return 1;
}

static int
p_write (lua_State *L) {
  LSpipe *p = tolpipe(L);
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
  {"write", p_write},
  {"open", pipe_open},
  {NULL, NULL}
};


static const luaL_Reg plib[] = {
  {"close", p_close},
  {"read", p_read},
  {"write", p_write},
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
