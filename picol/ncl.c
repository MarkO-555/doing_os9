/* ncl: Modified and enhanced for cmoc for NitrOS9/OS9 by strick.  BSD licensed */

/* Tcl in ~ 500 lines of code by Salvatore antirez Sanfilippo.  BSD licensed */

#define BUF_SIZE 200            /* instead of 1024 */

#define NULL 0
#define false 0
#define true 1

typedef unsigned char byte;
typedef unsigned int uint;

extern int Error(char *argv0, int err);
extern int ResultD(int x);
extern int ResultS(const char *msg, const char *x);

extern char *malloc(int);
extern void free(void *);
extern char *realloc(void *, int);

// In order to get all code into a single assembly listing for the module,
// we include these C files instead of using the linker to link the usual stuff.

#include "os9.c"

#include "puthex.c"

#include "std.c"

#include "malloc.c"

#include "buf.c"

#include "util.c"

#include "re.c"

//////////////////////////

//////////////////////////

// Start actual picol.

enum { PICOL_OK, PICOL_ERR, PICOL_RETURN, PICOL_BREAK, PICOL_CONTINUE };
enum { PT_ESC, PT_STR, PT_CMD, PT_VAR, PT_SEP, PT_EOL, PT_EOF };

struct picolParser {
  char *text;
  char *p;                      /* current text position */
  int len;                      /* remaining length */
  char *start;                  /* token start */
  char *end;                    /* token end */
  int type;                     /* token type, PT_... */
  int insidequote;              /* True if inside " " */
};

struct picolVar {
  char *name, *val;
  struct picolVar *next;
};

struct picolArray {
  char *name;
  struct picolVar *vars;
  struct picolArray *next;
};

typedef int (*picolCmdFunc)(int argc, char **argv, void *privdata);

struct picolCmd {
  char *name;
  picolCmdFunc func;
  void *privdata;
  struct picolCmd *next;
};

struct picolCallFrame {
  struct picolVar *vars;
  struct picolCallFrame *parent;        /* parent is NULL at top level */
};

// Were in struct picolInterp; now are global:
struct picolCallFrame *Callframe;
struct picolCmd *Commands;
struct picolArray *Arrays;
char *Result;

void FreeDope(int c, const char **v)
{
  for (int j = 0; j < c; j++)
    free((void *) v[j]);        // Free the strings.
  free(v);                      // Free the vector.
}

void picolInitParser(struct picolParser *p, const char *text)
{
  p->text = p->p = (char *) text;
  p->len = strlen(text);
  p->start = 0;
  p->end = 0;
  p->insidequote = 0;
  p->type = PT_EOL;
}

int picolParseSep(struct picolParser *p)
{
  p->start = p->p;
  while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r') {
    p->p++;
    p->len--;
  }
  p->end = p->p - 1;
  p->type = PT_SEP;
  return PICOL_OK;
}

int picolParseEol(struct picolParser *p)
{
  p->start = p->p;
  while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r' || *p->p == ';') {
    p->p++;
    p->len--;
  }
  p->end = p->p - 1;
  p->type = PT_EOL;
  return PICOL_OK;
}

int picolParseCommand(struct picolParser *p)
{
  int level = 1;
  int blevel = 0;
  p->start = ++p->p;
  p->len--;
  while (1) {
    if (p->len == 0) {
      break;
    } else if (*p->p == '[' && blevel == 0) {
      level++;
    } else if (*p->p == ']' && blevel == 0) {
      if (!--level)
        break;
    } else if (*p->p == '\\') {
      p->p++;
      p->len--;
    } else if (*p->p == '{') {
      blevel++;
    } else if (*p->p == '}') {
      if (blevel != 0)
        blevel--;
    }
    p->p++;
    p->len--;
  }
  p->end = p->p - 1;
  p->type = PT_CMD;
  if (*p->p == ']') {
    p->p++;
    p->len--;
  }
  return PICOL_OK;
}

int picolParseVar(struct picolParser *p)
{
  p->start = ++p->p;
  p->len--;                     /* skip the $ */
  while (1) {
    if ((*p->p >= 'a' && *p->p <= 'z')
        || (*p->p >= 'A' && *p->p <= 'Z') || (*p->p >= '0' && *p->p <= '9')
        || *p->p == '_') {
      p->p++;
      p->len--;
      continue;
    }
    break;
  }
  if (p->start == p->p) {       /* It's just a single char string "$" */
    p->start = p->end = p->p - 1;
    p->type = PT_STR;
  } else {
    p->end = p->p - 1;
    p->type = PT_VAR;
  }
  return PICOL_OK;
}

int picolParseBrace(struct picolParser *p)
{
  int level = 1;
  p->start = ++p->p;
  p->len--;
  while (1) {
    if (p->len >= 2 && *p->p == '\\') {
      p->p++;
      p->len--;
    } else if (p->len == 0 || *p->p == '}') {
      level--;
      if (level == 0 || p->len == 0) {
        p->end = p->p - 1;
        if (p->len) {
          p->p++;
          p->len--;             /* Skip final closed brace */
        }
        p->type = PT_STR;
        return PICOL_OK;
      }
    } else if (*p->p == '{')
      level++;
    p->p++;
    p->len--;
  }
  return PICOL_OK;              /* unreached */
}

int picolParseString(struct picolParser *p)
{
  int newword = (p->type == PT_SEP || p->type == PT_EOL || p->type == PT_STR);
  if (newword && *p->p == '{')
    return picolParseBrace(p);
  else if (newword && *p->p == '"') {
    p->insidequote = 1;
    p->p++;
    p->len--;
  }
  p->start = p->p;
  while (1) {
    if (p->len == 0) {
      p->end = p->p - 1;
      p->type = PT_ESC;
      return PICOL_OK;
    }
    switch (*p->p) {
    case '\\':
      if (p->len >= 2) {
        p->p++;
        p->len--;
      }
      break;
    case '$':
    case '[':
      p->end = p->p - 1;
      p->type = PT_ESC;
      return PICOL_OK;
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case ';':
      if (!p->insidequote) {
        p->end = p->p - 1;
        p->type = PT_ESC;
        return PICOL_OK;
      }
      break;
    case '"':
      if (p->insidequote) {
        p->end = p->p - 1;
        p->type = PT_ESC;
        p->p++;
        p->len--;
        p->insidequote = 0;
        return PICOL_OK;
      }
      break;
    }
    p->p++;
    p->len--;
  }
  return PICOL_OK;              /* unreached */
}

int picolParseComment(struct picolParser *p)
{
  while (p->len && *p->p != '\n') {
    p->p++;
    p->len--;
  }
  return PICOL_OK;
}

int picolGetToken(struct picolParser *p)
{
TOP:
  while (1) {
    if (!p->len) {
      if (p->type != PT_EOL && p->type != PT_EOF)
        p->type = PT_EOL;
      else
        p->type = PT_EOF;
      return PICOL_OK;
    }
    switch (*p->p) {
    case ' ':
    case '\t':
    case '\r':
      if (p->insidequote)
        return picolParseString(p);
      return picolParseSep(p);
    case '\n':
    case ';':
      if (p->insidequote)
        return picolParseString(p);
      return picolParseEol(p);
    case '[':
      return picolParseCommand(p);
    case '$':
      return picolParseVar(p);
    case '#':
      if (p->type == PT_EOL) {
        picolParseComment(p);
        goto TOP;               /* continue; */
      }
      return picolParseString(p);
    default:
      return picolParseString(p);
    }
  }
  return PICOL_OK;              /* unreached */
}

const char *Explode(char *s, int n)
{
  if (n < 0)
    n = strlen(s);

  struct Buf result;
  BufInit(&result);
  for (int i = 0; i < n; i++) {
    char tmp[8];
    snprintf_d(tmp, 10, "%d", s[i]);
    BufAppElemS(&result, tmp);
  }
  BufFinish(&result);

  return BufTake(&result);
}

void picolInitInterp()
{
  Callframe = (struct picolCallFrame *) malloc(sizeof(struct picolCallFrame));
  Callframe->vars = NULL;
  Callframe->parent = NULL;
  Commands = NULL;
  Arrays = NULL;
  Result = strdup("");
}

void picolAppendResult(const char *s)
{
  int sn = strlen(s);
  int rn = strlen(Result);
  Result = realloc(Result, sn + rn + 1);
  strcat(Result, s);
}

void picolSetResult(const char *s)
{
  free(Result);
  Result = strdup(s);
}

void picolMoveToResult(const char *s)
{
  free(Result);
  Result = (char *) s;
}

struct picolVar *picolGetVarFromRoot(struct picolVar *v, const char *name)
{
  for (; v; v = v->next) {
    if (strcasecmp(v->name, name) == 0)
      return v;
  }
  return NULL;
}

struct picolVar *picolGetVar(const char *name)
{
  return picolGetVarFromRoot(Callframe->vars, name);
}

int picolSetVarFromRoot(struct picolVar **root, const char *name, const char *val)
{
  struct picolVar *v = picolGetVarFromRoot(*root, name);
  if (v) {
    free(v->val);
    v->val = strdup(val);
  } else {
    v = (struct picolVar *) malloc(sizeof(*v));
    v->name = strdup(name);
    v->val = strdup(val);
    v->next = *root;
    *root = v;
  }
  return PICOL_OK;
}

int picolSetVar(const char *name, const char *val)
{
  return picolSetVarFromRoot(&Callframe->vars, name, val);
}

struct picolCmd *picolGetCommand(const char *name)
{
  for (struct picolCmd * c = Commands; c; c = c->next) {
    if (strcasecmp(c->name, name) == 0) {
      return c;
    }
  }
  return NULL;
}

struct picolArray *picolGetArray(const char *name)
{
  for (struct picolArray * p = Arrays; p; p = p->next) {
    if (strcasecmp(p->name, name) == 0) {
      return p;
    }
  }
  return NULL;
};

int picolRegisterCommand(const char *name, picolCmdFunc f, void *privdata)
{
  struct picolCmd *c = picolGetCommand(name);
  if (c) {
    // redefine the command, so re-use the struct *c.
    free(c->name);
    if (c->privdata && c->func == picolCommandCallProc) {
      // procdata always has two malloced slots.
      free(((char **) c->privdata)[0]);
      free(((char **) c->privdata)[1]);
      free((char *) c->privdata);
    }                           // or else it is a memory leak because we don't understand the privdata.
  } else {
    // define a new command, so malloc a new struct *c.
    c = (struct picolCmd *) malloc(sizeof(*c));
    c->next = Commands;
    Commands = c;
  }
  c->name = strdup(name);
  c->func = f;
  c->privdata = privdata;
  return PICOL_OK;
}

/* EVAL! */
int picolEval(const char *t, const char *where)
{
  struct picolParser p;
  int argc = 0, j;
  char **argv = NULL;
  char errbuf[BUF_SIZE];
  int retcode = PICOL_OK;
  picolSetResult("");
  picolInitParser(&p, t);
  while (1) {
    char *t;
    int tlen;
    int prevtype = p.type;
    picolGetToken(&p);
    if (p.type == PT_EOF)
      break;
    tlen = p.end - p.start + 1;
    if (tlen < 0)
      tlen = 0;
    t = (char *) malloc(tlen + 1);
    memcpy(t, p.start, tlen);
    t[tlen] = '\0';
    if (p.type == PT_VAR) {
      struct picolVar *v = picolGetVar(t);
      if (!v) {
        snprintf_s(errbuf, BUF_SIZE, "No such variable '%s'", t);
        free(t);
        picolSetResult(errbuf);
        retcode = PICOL_ERR;
        goto err;
      }
      free(t);
      t = strdup(v->val);
    } else if (p.type == PT_CMD) {
      retcode = picolEval(t, "[...]");
      free(t);
      if (retcode != PICOL_OK)
        goto err;
      t = strdup(Result);
    } else if (p.type == PT_ESC) {
      /* XXX: TODO: escape handling missing! */
    } else if (p.type == PT_SEP) {
      prevtype = p.type;
      free(t);
      continue;
    }
    /* We have a complete command + args. Call it! */
    if (p.type == PT_EOL) {
      struct picolCmd *c;
      free(t);
      prevtype = p.type;
      if (argc) {
        if ((c = picolGetCommand(argv[0])) == NULL) {
          if (strcasecmp(argv[0], "unknown")) {
            c = picolGetCommand("unknown");
            if (c) {
              argv = (char **) realloc((char *) argv, (argc + 1) * sizeof(char *));
              argc++;
              // shift everything down one slot.
              for (int k = argc - 2; k >= 0; k--) {
                argv[k + 1] = argv[k];
              }
              // and shove "unknown" in front of them.
              argv[0] = strdup("unknown");
              goto call;
            }
          }
          snprintf_s(errbuf, BUF_SIZE, "No such command '%s'", argv[0]);
          picolSetResult(errbuf);
          retcode = PICOL_ERR;
          goto err;
        }
      call:
        retcode = c->func(argc, argv, c->privdata);
        if (retcode != PICOL_OK)
          goto err;
      }
      /* Prepare for the next command */
      for (j = 0; j < argc; j++)
        free(argv[j]);
      free(argv);
      argv = NULL;
      argc = 0;
      continue;
    }
    /* We have a new token, append to the previous or as new arg? */
    if (prevtype == PT_SEP || prevtype == PT_EOL) {
      argv = (char **) realloc(argv, sizeof(char *) * (argc + 1));
      argv[argc] = t;
      argc++;
    } else {                    /* Interpolation */
      int oldlen = strlen(argv[argc - 1]), tlen = strlen(t);
      argv[argc - 1] = (char *) realloc(argv[argc - 1], oldlen + tlen + 1);
      memcpy(argv[argc - 1] + oldlen, t, tlen);
      argv[argc - 1][oldlen + tlen] = '\0';
      free(t);
    }
    prevtype = p.type;
  }
err:
  for (j = 0; j < argc; j++)
    free(argv[j]);
  free(argv);
  if (retcode == PICOL_ERR) {
    picolAppendResult("; in ");
    picolAppendResult(where);
  }
  return retcode;
}

/* ACTUAL COMMANDS! */
int picolArityErr(char *name)
{
  char buf[BUF_SIZE];
  snprintf_s(buf, BUF_SIZE, "Wrong number of args for %s", name);
  picolSetResult(buf);
  return PICOL_ERR;
}

int picolCommandString(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  char m1 = Up(argv[0][0]);
  char m2 = Up(argv[0][1]);
  int cmp = strcasecmp(argv[1], argv[2]);
  int b;

  if (m1 == 'E' && m2 == 'Q')
    b = (cmp == 0);
  else if (m1 == 'N' && m2 == 'E')
    b = (cmp != 0);
  else if (m1 == 'L' && m2 == 'T')
    b = (cmp < 0);
  else if (m1 == 'L' && m2 == 'E')
    b = (cmp <= 0);
  else if (m1 == 'G' && m2 == 'T')
    b = (cmp > 0);
  else if (m1 == 'G' && m2 == 'E')
    b = (cmp >= 0);
  else {
    puthex('1', m1);
    puthex('2', m2);
    panic("WUT");
  }
  picolSetResult(b ? "1" : "0");
  return PICOL_OK;
}

int picolCommandMath(int argc, char **argv, void *pd)
{
  char m1 = argv[0][0];
  char m2 = argv[0][1];
  char buf[8];
  int a, b, c;
  if (m1 == '+' || m1 == '*') {
    // + and * allow any number of args.
    c = (m1 == '+') ? 0 : 1;
    for (int j = 1; j < argc; j++) {
      b = atoi(argv[j]);
      c = (m1 == '+') ? c + b : c * b;
    }
  } else {
    // The rest apply to only 2 numbers.
    if (argc != 3)
      return picolArityErr(argv[0]);
    a = atoi(argv[1]);
    b = atoi(argv[2]);
    if (m1 == '-')
      c = a - b;
    else if (m1 == '/')
      c = a / b;
    else if (m1 == '%')
      c = a % b;
    else if (m1 == '>' && m2 == '\0')
      c = a > b;
    else if (m1 == '>' && m2 == '=')
      c = a >= b;
    else if (m1 == '<' && m2 == '\0')
      c = a < b;
    else if (m1 == '<' && m2 == '=')
      c = a <= b;
    else if (m1 == '=' && m2 == '=')
      c = a == b;
    else if (m1 == '!' && m2 == '=')
      c = a != b;
    else if (strcasecmp(argv[0], "bitand") == 0)
      c = a & b;
    else if (strcasecmp(argv[0], "bitor") == 0)
      c = a | b;
    else if (strcasecmp(argv[0], "bitxor") == 0)
      c = a ^ b;
    else
      c = 0;                    /* I hate warnings */
  }
  snprintf_d(buf, 8, "%d", c);
  picolSetResult(buf);
  return PICOL_OK;
}

int NotFound()
{
  picolSetResult("not found");
  return PICOL_ERR;
}

int picolCommandArray(int argc, char **argv, void *pd)
{
  struct Buf buf;
  BufInit(&buf);

  switch (argc) {
  default:
    return picolArityErr(argv[0]);

  case 1:{
      // List arrays.
      for (struct picolArray * p = Arrays; p; p = p->next) {
        BufAppElemS(&buf, p->name);
      }
      BufFinish(&buf);
      picolMoveToResult(BufTake(&buf));
    }
    break;
  case 2:{
      // List keys of named array.
      struct picolArray *array = picolGetArray(argv[1]);
      if (!array) {
        return NotFound();
      }

      char *list = strdup("");
      for (struct picolVar * q = array->vars; q; q = q->next) {
        BufAppElemS(&buf, q->name);
      }
      BufFinish(&buf);
      picolMoveToResult(BufTake(&buf));
    }
    break;
  case 3:{
      struct picolArray *array = picolGetArray(argv[1]);
      if (!array) {
        return NotFound();
      }
      struct picolVar *var = picolGetVarFromRoot(array->vars, argv[2]);
      if (!var) {
        return NotFound();
      }
      picolSetResult(var->val);
    }
    break;
  case 4:{
      // Set variable.
      struct picolArray *array = picolGetArray(argv[1]);
      if (!array) {
        array = (struct picolArray *) malloc(sizeof *array);
        array->name = strdup(argv[1]);
        array->vars = NULL;
        array->next = Arrays;
        Arrays = array;
      }
      picolSetVarFromRoot(&array->vars, argv[2], argv[3]);
    }
    break;
  }
  BufDel(&buf);
  return PICOL_OK;
}

int SplitList(const char *s, int *argcP, const char ***argvP)
{
  struct Buf dope;
  BufInit(&dope);

  while (*s) {
    while (*s && *s <= 32) {    // skip white
      s++;
    }
    if (!s)
      break;

    const char *end;
    int len = ElemLen(s, &end);
    const char *elem = ElemDecode(s);
    s = end;

    BufAppDope(&dope, elem);
  }
  *argvP = BufTakeDope(&dope, argcP);
  return PICOL_OK;
}

int picolCommandJoin(int argc, char **argv, void *pd)
{
  int c = 0;
  const char **v = NULL;
  int err = SplitList(argv[1], &c, &v);

  char delim;
  switch (argc) {
  default:
    return picolArityErr(argv[0]);

  case 2:
    delim = 0;                  // Join with empty string.
    break;

  case 3:
    delim = argv[2][0];         // Join with first char of 2nd arg.
    break;
  }

  struct Buf result;
  BufInit(&result);
  for (int j = 0; j < c; j++) {
    if (j && delim) {
      BufAppC(&result, delim);
    }
    BufAppS(&result, v[j], -1);
  }
  BufFinish(&result);

  FreeDope(c, v);
  picolMoveToResult(BufTake(&result));
  return PICOL_OK;
}

int picolCommandSplit(int argc, char **argv, void *pd)
{
  char delim;
  char *s = argv[1];

  switch (argc) {
  default:
    return picolArityErr(argv[0]);

  case 2:
    delim = 0;                  // Split on white space.
    break;

  case 3:
    delim = argv[2][0];         // Split on first char of 2nd arg.
    break;
  }

  byte final_delim = false;
  struct Buf list;
  BufInit(&list);
  while (*s) {
    struct Buf part;
    BufInit(&part);
    while (*s) {
      if (delim) {
        // Use specified delimiter.
        if (*s == delim) {
          final_delim = true;
          break;
        }
      } else {
        // Use any whitespace.
        if (*s <= 32)
          break;
      }

      // Not at a delimiter.
      BufAppC(&part, *s);
      s++;
      final_delim = false;
    }
    if (*s)
      s++;                      // past delim.

    // Finished a part.
    if (delim || part.n) {      // no empties if split on white.
      BufAppElemS(&list, BufFinish(&part));
    }
    BufDel(&part);
  }
  if (final_delim) {
    BufAppElemS(&list, "");
  }
  BufFinish(&list);
  picolMoveToResult(BufTake(&list));
  return PICOL_OK;
}

int picolCommandStringMatch(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  // Always do case-independant matching.
  char *pattern = strdup_upper(argv[1]);
  char *s = strdup_upper(argv[2]);
  int z = (Up(argv[0][0]) == 'R') ? re_match(pattern, s) : Tcl_StringMatch(s, pattern);
  free(pattern);
  free(s);
  return ResultD(z);
}

int picolCommandSet(int argc, char **argv, void *pd)
{
  if (argc != 2 && argc != 3)
    return picolArityErr(argv[0]);
  if (argc == 2) {
    // with one argument, get var.
    struct picolVar *s = picolGetVar(argv[1]);
    if (!s) {
      picolSetResult("no such var");
      ResultS("no such var: %s", argv[1]);
      return PICOL_ERR;
    }
    picolSetResult(s->val);
    return PICOL_OK;
  }
  // with two arguments, set var.
  picolSetVar(argv[1], argv[2]);
  picolSetResult(argv[2]);
  return PICOL_OK;
}

int picolCommand9Read(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  int fd = atoi(argv[1]);
  int n = atoi(argv[2]);
  char *buf = malloc(n + 1);
  int bytes_read = 0;
  int e = Os9Read(fd, buf, n, &bytes_read);
  if (e)
    return Error(argv[0], e);
  picolMoveToResult(Explode(buf, bytes_read));
  free(buf);
  return PICOL_OK;
}

int picolCommandGets(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  int fd = atoi(argv[1]);
  char *varname = argv[2];
  char buf[BUF_SIZE + 1];
  int bytes_read = 0;
  bzero(buf, BUF_SIZE + 1);
  int e = Os9ReadLn(fd, buf, BUF_SIZE, &bytes_read);
  if (e)
    return Error(argv[0], e);
  picolSetVar(varname, buf);
  return ResultD(bytes_read);
}

int picolCommandPuts(int argc, char **argv, void *pd)
{
  char *argv0 = argv[0];        // because argv may increment.
  byte nonewline = false;
  // any dash argument must be -nonewline.
  if (argc > 2 && argv[1][0] == '-') {
    nonewline = true;
    argc--, argv++;
  }
  if (argc != 2 && argc != 3)
    return picolArityErr(argv0);
  // defaults to path 1.
  int path = (argc == 3) ? atoi(argv[1]) : 1;
  int unused;
  int e = Os9WritLn(path, argv[argc - 1], strlen(argv[argc - 1]), &unused);
  if (e)
    return Error(argv0, e);
  if (!nonewline) {
    e = Os9WritLn(path, "\r", 1, &unused);
    if (e)
      return Error(argv0, e);
  }
  return PICOL_OK;
}

int picolCommandIf(int argc, char **argv, void *pd)
{
  int retcode;
  if (argc != 3 && argc != 5)
    return picolArityErr(argv[0]);
  if ((retcode = picolEval(argv[1], "cond of if")) != PICOL_OK)
    return retcode;
  if (atoi(Result))
    return picolEval(argv[2], "then of if");
  else if (argc == 5)
    return picolEval(argv[4], "else of if");
  return PICOL_OK;
}

int picolCommandAnd(int argc, char **argv, void *pd)
{
  int n = 1;
  for (int j = 1; j < argc; j++) {
    int e = picolEval(argv[j], "clause of and");
    if (e)
      return e;
    n = atoi(Result);
    if (!n)
      return ResultD(0);
  }
  return ResultD(n);
}

int picolCommandOr(int argc, char **argv, void *pd)
{
  for (int j = 1; j < argc; j++) {
    int e = picolEval(argv[j], "clause of or");
    if (e)
      return e;
    int n = atoi(Result);
    if (n)
      return ResultD(n);
  }
  return ResultD(0);
}

int picolCommandWhile(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  while (1) {
    int retcode = picolEval(argv[1], "cond of while");
    if (retcode != PICOL_OK)
      return retcode;
    if (atoi(Result)) {
      if ((retcode = picolEval(argv[2], "body of while")) == PICOL_CONTINUE)
        continue;
      else if (retcode == PICOL_OK)
        continue;
      else if (retcode == PICOL_BREAK)
        return PICOL_OK;
      else
        return retcode;
    } else {
      return PICOL_OK;
    }
  }
}

int picolCommandRetCodes(int argc, char **argv, void *pd)
{
  if (argc != 1)
    return picolArityErr(argv[0]);
  if (strcasecmp(argv[0], "break") == 0)
    return PICOL_BREAK;
  else if (strcasecmp(argv[0], "continue") == 0)
    return PICOL_CONTINUE;
  return PICOL_OK;
}

void picolDropCallFrame()
{
  struct picolCallFrame *cf = Callframe;
  struct picolVar *v = cf->vars, *t;
  while (v) {
    t = v->next;
    free(v->name);
    free(v->val);
    free(v);
    v = t;
  }
  Callframe = cf->parent;
  free(cf);
}

int picolCommandCallProc(int argc, char **argv, void *pd)
{
  char **pair = (char **) pd, *alist = pair[0], *body = pair[1];

  struct picolCallFrame *cf = (struct picolCallFrame *) malloc(sizeof(*cf));
  cf->vars = NULL;
  cf->parent = Callframe;
  Callframe = cf;

  // TODO: preprocess the alist.
  int c = 0;
  const char **v = NULL;
  int err = SplitList(alist, &c, &v);

  byte varargs = false;
  if (c && strcasecmp(v[c - 1], "args") == 0) {
    varargs = true;
  }
  if ((!varargs && c != argc - 1) || (varargs && argc - 1 < c - 1)) {
    char errbuf[BUF_SIZE];
    snprintf_s(errbuf, BUF_SIZE, "Proc '%s' called with wrong num args", argv[0]);
    picolSetResult(errbuf);
    picolDropCallFrame();       /* remove the called proc callframe */
    return PICOL_ERR;
  }

  for (int i = 0; i < c - varargs; i++) {
    picolSetVar(v[i], argv[i + 1]);
  }

  if (varargs) {
    struct Buf rest;
    BufInit(&rest);
    for (int j = c; j < argc; j++) {
      BufAppElemS(&rest, argv[j]);
    }
    BufFinish(&rest);
    picolSetVar("args", rest.s);
    BufDel(&rest);
  }

  int errcode = picolEval(body, argv[0]);
  if (errcode == PICOL_RETURN)
    errcode = PICOL_OK;
  picolDropCallFrame();         /* remove the called proc callframe */
  return errcode;
}

int picolCommandProc(int argc, char **argv, void *pd)
{
  char **procdata = (char **) malloc(sizeof(char *) * 2);
  if (argc != 4)
    return picolArityErr(argv[0]);
  procdata[0] = strdup(argv[2]);        /* arguments list */
  procdata[1] = strdup(argv[3]);        /* procedure body */
  return picolRegisterCommand(argv[1], picolCommandCallProc, procdata);
}

int picolCommandReturn(int argc, char **argv, void *pd)
{
  if (argc != 1 && argc != 2)
    return picolArityErr(argv[0]);
  picolSetResult((argc == 2) ? (const char *) argv[1] : "");
  return PICOL_RETURN;
}

int picolCommandInfo(int argc, char **argv, void *pd)
{
  puts(" procs:\r");
  struct picolCmd *c;
  for (c = Commands; c; c = c->next) {
    if (c->func != picolCommandCallProc)
      continue;
    puts("   proc ");
    puts(c->name);
    puts(" {");
    puts(((const char **) c->privdata)[0]);
    puts("} {");
    puts(((const char **) c->privdata)[1]);
    puts("}\r");
  }

  puts(" commands: ");
  for (c = Commands; c; c = c->next) {
    if (c->func == picolCommandCallProc)
      continue;
    puts(c->name);
    puts(" ");
  }
  puts("\r");

  for (struct picolCallFrame * f = Callframe; f; f = f->parent) {
    puts(f->parent ? " frame: " : " globals: ");
    for (struct picolVar * v = f->vars; v; v = v->next) {
      puts(v->name);
      puts("=");
      puts(v->val);
      puts(" ");
    }
    puts("\r");
  }

  puts(" arrays:\r");
  for (struct picolArray * array = Arrays; array; array = array->next) {
    printf_s("   %s: ", array->name);
    for (struct picolVar * v = array->vars; v; v = v->next) {
      puts(v->name);
      puts("=");
      puts(v->val);
      puts(" ");
    }
    puts("\r");
  }

  picolSetResult("");
  return PICOL_OK;
}

int picolCommandEval(int argc, char **argv, void *pd)
{
  struct Buf buf;
  BufInit(&buf);
  // Join the args simply with spaces.
  for (int j = 1; j < argc; j++) {
    if (j)
      BufAppC(&buf, ' ');
    BufAppS(&buf, argv[j], -1);
  }
  BufFinish(&buf);
  int e = picolEval(BufPeek(&buf), "eval");
  BufDel(&buf);
  return e;
}

int picolCommandCatch(int argc, char **argv, void *pd)
{
  if (argc != 2 && argc != 3)
    return picolArityErr(argv[0]);
  char *body = argv[1];
  char *resultVar = (argc == 3) ? argv[2] : (char *) NULL;
  int e = picolEval(body, "catch");
  if (resultVar) {
    picolSetVar(resultVar, Result);
  }
  return ResultD(e);
}

int picolCommandExplode(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);

  picolMoveToResult(Explode(argv[1], -1));
  return PICOL_OK;
}

int picolCommandImplode(int argc, char **argv, void *pd)
{
  int c = 0;
  const char **v = NULL;
  int err = SplitList(argv[1], &c, &v);

  char *z = malloc(c + 1);
  int j;
  for (j = 0; j < c; j++) {
    z[j] = (char) atoi(v[j]);
  }
  z[j] = '\0';

  FreeDope(c, v);
  picolMoveToResult(z);
  return PICOL_OK;
}

int picolCommandListAppend(int argc, char **argv, void *pd)
{
  if (argc < 2)
    return picolArityErr(argv[0]);

  struct picolVar *var = picolGetVar(argv[1]);
  if (!var) {
    picolSetVar(argv[1], "");
    var = picolGetVar(argv[1]);
  }
  struct Buf buf;
  BufInit(&buf);
  free(buf.s);
  buf.s = var->val;
  buf.n = strlen(buf.s);

  for (int j = 2; j < argc; j++) {
    BufAppElemS(&buf, argv[j]);
  }
  BufFinish(&buf);
  var->val = buf.s;
  return PICOL_OK;
}

int picolCommandListLength(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);

  int c = 0;
  const char **v = NULL;
  int err = SplitList(argv[1], &c, &v);
  FreeDope(c, v);
  return ResultD(c);
}

int picolCommandListRange(int argc, char **argv, void *pd)
{
  if (argc != 3 && argc != 4)
    return picolArityErr(argv[0]);
  char *list = argv[1];
  int a = atoi(argv[2]);
  // lindex is like lrange with index twice.
  int b = (argc == 3) ? a : atoi(argv[3]);

  int c = 0;
  const char **v = NULL;
  int err = SplitList(list, &c, &v);

  struct Buf result;
  BufInit(&result);
  for (int j = 0; j < c; j++) {
    if (a <= j && j <= b)
      BufAppElemS(&result, v[j]);
  }
  FreeDope(c, v);

  BufFinish(&result);
  picolMoveToResult(BufTake(&result));
  return PICOL_OK;
}

int picolCommandStringLength(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  char *s = argv[1];
  int n = strlen(s);
  return ResultD(n);
}

int picolCommandStringRange(int argc, char **argv, void *pd)
{
  if (argc != 3 && argc != 4)
    return picolArityErr(argv[0]);
  char *s = argv[1];
  int n = strlen(s);
  int a = atoi(argv[2]);
  // sindex is like srange with index twice.
  int b = (argc == 3) ? a : atoi(argv[3]);
  if (a < 0)
    a = 0;
  if (b >= n)
    b = n - 1;
  struct Buf result;
  BufInit(&result);
  for (int j = a; j <= b; j++) {
    BufAppC(&result, s[j]);
  }
  BufFinish(&result);
  picolMoveToResult(BufTake(&result));
  return PICOL_OK;
}

int picolCommandStringUpperLower(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  byte up = (Up(argv[0][1]) == 'U');
  char *s = argv[1];
  int n = strlen(s);
  char *z = malloc(n + 1);
  for (int j = 0; j <= n; j++) {
    z[j] = up ? Up(s[j]) : Down(s[j]);
  }
  picolMoveToResult(z);
  return PICOL_OK;
}

int picolCommandForEach(int argc, char **argv, void *pd)
{
  if (argc != 4)
    return picolArityErr(argv[0]);
  char *var = argv[1];
  char *list = argv[2];
  char *body = argv[3];

  int c = 0;
  const char **v = NULL;
  int err = SplitList(list, &c, &v);
  for (int j = 0; j < c; j++) {
    picolSetVar(var, v[j]);
    int e = picolEval(body, "body of foreach");
    if (e == PICOL_CONTINUE)
      continue;
    if (e == PICOL_BREAK)
      break;
    if (e != PICOL_OK)
      return e;
  }

  FreeDope(c, v);
  picolSetResult("");
  return PICOL_OK;
}

const char *FormList(int argc, char **argv)
{
  struct Buf buf;
  BufInit(&buf);
  for (int i = 0; i < argc; i++) {
    BufAppElemS(&buf, argv[i]);
  }
  BufFinish(&buf);
  return BufTake(&buf);
}

int picolCommandList(int argc, char **argv, void *pd)
{
  const char *s = FormList(argc - 1, argv + 1);
  picolSetResult(s);
  return PICOL_OK;
}

int Error(char *argv0, int err)
{
  char buf[32];
  snprintf_s(buf, 32, "%s: ERROR %d", argv0);
  char buf2[32];
  snprintf_d(buf2, 32, buf, err);
  picolSetResult(buf2);
  return PICOL_ERR;
}

int ResultD(int x)
{
  char buf[32];
  snprintf_d(buf, 32, "%d", x);
  picolSetResult(buf);
  return PICOL_OK;
}

int ResultS(const char *msg, const char *x)
{
  char buf[BUF_SIZE];
  snprintf_s(buf, 32, msg, x);
  picolSetResult(buf);
  return PICOL_OK;
}

// This currently serves as high-level "exit" and low-level "9exit".
// In the future, if there is cleanup (like flushing IO), a new "exit" will be needed.
int picolCommand9Exit(int argc, char **argv, void *pd)
{
  if (argc != 1 && argc != 2)
    return picolArityErr(argv[0]);
  exit((argc == 2) ? atoi(argv[1]) : 0);
  return PICOL_OK;
}

const char *AddCR(char *s)
{
  int n = strlen(s);
  s = realloc(s, n + 2);
  s[n] = '\r';
  return (const char *) s;
}

const char *JoinWithSpaces(int argc, char **argv)
{
  struct Buf buf;
  BufInit(&buf);
  for (int i = 0; i < argc; i++) {
    if (i)
      BufAppC(&buf, ' ');
    BufAppS(&buf, argv[i], -1);
  }
  BufFinish(&buf);
  return BufTake(&buf);
}

int picolCommand9Chain(int argc, char **argv, void *pd)
{
  if (argc < 2) {
    return picolArityErr(argv[0]);
  }
  char *program = argv[1];
  const char *params = /*FormList */ JoinWithSpaces(argc - 2, argv + 2);
  params = AddCR((char *) params);
  int e = Os9Chain(program, params, strlen(params), 0 /*lang_type */ ,
                   0 /*mem_size */ );
  // If returns, it is an error.
  return Error(argv[0], e);
}

int picolCommand9Fork(int argc, char **argv, void *pd)
{
  if (argc < 2) {
    return picolArityErr(argv[0]);
  }
  char *program = argv[1];
  const char *params = /*FormList */ JoinWithSpaces(argc - 2, argv + 2);
  params = AddCR((char *) params);
  int child_id = 0;
  int e = Os9Fork(program, params, strlen(params), 0 /*lang_type */ ,
                  0 /*mem_size */ , &child_id);
  if (e)
    return Error(argv[0], e);
  free((char *) params);
  return ResultD(child_id);
}

int picolCommand9Wait(int argc, char **argv, void *pd)
{
  if (argc != 1)
    return picolArityErr(argv[0]);
  int child_id = 0;
  int e = Os9Wait(&child_id);
  if (e)
    return Error(argv[0], e);
  return ResultD(child_id);
}

int picolCommand9Dup(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  int new_path = 0;
  int path = atoi(argv[1]);
  int e = Os9Dup(path, &new_path);
  if (e)
    return Error(argv[0], e);
  return ResultD(new_path);
}

int picolCommand9Close(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  int path = atoi(argv[1]);
  int e = Os9Close(path);
  if (e)
    return Error(argv[0], e);
  picolSetResult("");
  return PICOL_OK;
}

int picolCommand9Sleep(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  int ticks = atoi(argv[1]);
  int e = Os9Sleep(ticks);
  if (e)
    return Error(argv[0], e);
  picolSetResult("");
  return PICOL_OK;
}

char SetHiBitOfLastChar(char *s)
{
  int n = strlen(s);
  char z = s[n - 1];
  s[n - 1] |= 0x80;
  return z;
}

void RestoreLastChar(char *s, char c)
{
  int n = strlen(s);
  s[n - 1] = c;
}

int picolCommand9Create(int argc, char **argv, void *pd)
{
  if (argc != 4)
    return picolArityErr(argv[0]);
  char *path = argv[1];
  char final = SetHiBitOfLastChar(path);
  int mode = atoi(argv[2]);
  int attrs = atoi(argv[3]);
  int fd = 0;

  int e = Os9Create(path, mode, attrs, &fd);
  RestoreLastChar(path, final);
  if (e)
    return Error(argv[0], e);
  return ResultD(fd);
}

int picolCommand9Open(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  char *path = argv[1];
  char final = SetHiBitOfLastChar(path);
  int mode = atoi(argv[2]);
  int fd = 0;

  int e = Os9Open(path, mode, &fd);
  RestoreLastChar(path, final);
  if (e)
    return Error(argv[0], e);
  return ResultD(fd);
}

int picolCommand9ChgDir(int argc, char **argv, void *pd)
{
  if (argc != 3)
    return picolArityErr(argv[0]);
  char *path = argv[1];
  char final = SetHiBitOfLastChar(path);
  int mode = atoi(argv[2]);

  int e = Os9ChgDir(path, mode);
  RestoreLastChar(path, final);
  if (e)
    return Error(argv[0], e);
  picolSetResult("");
  return PICOL_OK;
}

int picolCommand9Delete(int argc, char **argv, void *pd)
{
  if (argc != 2)
    return picolArityErr(argv[0]);
  char *path = argv[1];
  char final = SetHiBitOfLastChar(path);

  int e = Os9Delete(path);
  RestoreLastChar(path, final);
  if (e)
    return Error(argv[0], e);
  picolSetResult("");
  return PICOL_OK;
}

void picolRegisterCoreCommands()
{
  const char *mathOps[] = { "+", "-", "*", "/", "%", ">", ">=", "<", "<=", "==", "!=",
    "bitand", "bitor", "bitxor", NULL
  };
  for (const char **p = mathOps; *p; p++)
    picolRegisterCommand(*p, picolCommandMath, NULL);

  const char *strOps[] = { "eq", "ne", "lt", "le", "gt", "ge", NULL };
  for (const char **p = strOps; *p; p++)
    picolRegisterCommand(*p, picolCommandString, NULL);

  picolRegisterCommand("set", picolCommandSet, NULL);
  picolRegisterCommand("puts", picolCommandPuts, NULL);
  picolRegisterCommand("if", picolCommandIf, NULL);
  picolRegisterCommand("and", picolCommandAnd, NULL);
  picolRegisterCommand("or", picolCommandOr, NULL);
  picolRegisterCommand("while", picolCommandWhile, NULL);
  picolRegisterCommand("break", picolCommandRetCodes, NULL);
  picolRegisterCommand("continue", picolCommandRetCodes, NULL);
  picolRegisterCommand("proc", picolCommandProc, NULL);
  picolRegisterCommand("return", picolCommandReturn, NULL);
  picolRegisterCommand("info", picolCommandInfo, NULL);
  picolRegisterCommand("foreach", picolCommandForEach, NULL);
  picolRegisterCommand("eval", picolCommandEval, NULL);
  picolRegisterCommand("catch", picolCommandCatch, NULL);
  picolRegisterCommand("list", picolCommandList, NULL);
  picolRegisterCommand("explode", picolCommandExplode, NULL);
  picolRegisterCommand("implode", picolCommandImplode, NULL);
  picolRegisterCommand("lappend", picolCommandListAppend, NULL);
  picolRegisterCommand("llength", picolCommandListLength, NULL);
  picolRegisterCommand("lindex", picolCommandListRange, NULL);
  picolRegisterCommand("lrange", picolCommandListRange, NULL);
  picolRegisterCommand("slength", picolCommandStringLength, NULL);
  picolRegisterCommand("sindex", picolCommandStringRange, NULL);
  picolRegisterCommand("srange", picolCommandStringRange, NULL);
  picolRegisterCommand("supper", picolCommandStringUpperLower, NULL);
  picolRegisterCommand("srange", picolCommandStringUpperLower, NULL);
  picolRegisterCommand("smatch", picolCommandStringMatch, NULL);
  picolRegisterCommand("regexp", picolCommandStringMatch, NULL);
  picolRegisterCommand("array", picolCommandArray, NULL);
  picolRegisterCommand("split", picolCommandSplit, NULL);
  picolRegisterCommand("join", picolCommandJoin, NULL);
  picolRegisterCommand("exit", picolCommand9Exit, NULL);

  // low-level os9 commands:
  picolRegisterCommand("9exit", picolCommand9Exit, NULL);
  picolRegisterCommand("9chain", picolCommand9Chain, NULL);
  picolRegisterCommand("9fork", picolCommand9Fork, NULL);
  picolRegisterCommand("9wait", picolCommand9Wait, NULL);
  picolRegisterCommand("9dup", picolCommand9Dup, NULL);
  picolRegisterCommand("9close", picolCommand9Close, NULL);
  picolRegisterCommand("9sleep", picolCommand9Sleep, NULL);
  picolRegisterCommand("9chgdir", picolCommand9ChgDir, NULL);
  picolRegisterCommand("9open", picolCommand9Open, NULL);
  picolRegisterCommand("9create", picolCommand9Create, NULL);
  picolRegisterCommand("9delete", picolCommand9Delete, NULL);
  picolRegisterCommand("9read", picolCommand9Read, NULL);
  //picolRegisterCommand("9write", picolCommand9Write, NULL);
  //picolRegisterCommand("9readln", picolCommand9ReadLn, NULL);
  //picolRegisterCommand("9writln", picolCommand9WritLn, NULL);

  // demo commands:
  picolEval("proc fib x {if {< $x 2} {return $x}; + [fib [- $x 1]] [fib [- $x 2]]}", "__init__");
  picolEval("proc tri x {if {< $x 2} {return $x}; + $x [tri [- $x 1]]}", "__init__");
  picolEval
      ("proc iota x {set z {}; set i 0; while {< $i $x} {set z \"$z $i\" ; set i [+ $i 1] }; set z}",
       "__init__");
  picolEval("proc run args {eval 9fork $args; 9wait}", "__init__");
  picolEval("proc unknown args {eval 9fork $args; 9wait}", "__init__");

  picolEval
      ("proc implode_filename x {set z {}; foreach i $x {if {< $i 0} {lappend z [+ 128 $i]; break} else {lappend z $i}}; implode $z",
       "__init__");
  picolEval
      ("proc 9dir x {set z {}; set fd [9open $x 129]; while * {if {catch {set v [9read $fd 32]}} break; if {lindex $v 0} {lappend z [implode_filename $v]}}; return $z}",
       "__init__");

}

// For lame coco keyboards:  `((` -> `[`, `(((` -> `{`, `))` -> `]`, `)))` -> `}`, `@@` -> `\`.
void ReduceBigraphs(char *s)
{
  char *z = s;                  // read from p, write to z.
  for (char *p = s; *p; p++) {
    if (p[0] == '(') {
      if (p[1] == '(') {
        if (p[2] == '(') {
          *z++ = '{';
          p++;
        } else {
          *z++ = '[';
        }
        p++;
      } else {
        *z++ = '(';
      }
    } else if (p[0] == ')') {
      if (p[1] == ')') {
        if (p[2] == ')') {
          *z++ = '}';
          p++;
        } else {
          *z++ = ']';
        }
        p++;
      } else {
        *z++ = ')';
      }
    } else if (p[0] == '@') {
      if (p[1] == '@') {
        *z++ = '\\';
        p++;
      } else {
        *z++ = '@';
      }
    } else {
      *z++ = *p;
    }
  }
  *z = '\0';
}

int main()
{
  picolInitInterp();
  picolRegisterCoreCommands();

  while (1) {
    puts(" >NCL> ");
    char line[111];
    bzero(line, sizeof line);
    int bytes_read;
    int e = Os9ReadLn(0 /*path */ , line, 111, &bytes_read);
    if (e) {
      puts(" *EOF*\r");
      break;
    }
    ReduceBigraphs(line);
    e = picolEval(line, "__repl__");
    if (e) {
      puts(" ERROR: ");
      if (e > 1) {
        printf_d("CODE=%d: ", e);
      }
      puts(Result);
      puts("\r");
    } else {
      if (Result[0] != '\0') {
        puts(Result);
        puts("\r");
      }
    }
  }
  exit(0);
  return 0;
}
