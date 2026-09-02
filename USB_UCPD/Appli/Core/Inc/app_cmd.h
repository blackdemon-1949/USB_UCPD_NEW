/**
 * @file    app_cmd.h
 * @brief   CLI command registry for the analyzer feature layers.
 *
 * The original console is a single if/else chain in app_cli.c that must stay
 * untouched - it is part of the hardware-verified baseline.  New feature
 * layers therefore register their commands here instead, and app_cli.c falls
 * through to APP_CMD_Dispatch() only for words its own chain does not handle.
 *
 * Every public feature is expected to add an entry to the table in
 * app_cmd.c so that it shows up in `help`.
 */
#ifndef APP_CMD_H
#define APP_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/** @return 1 when the command was handled, 0 to let the caller report it. */
typedef int (*APP_CMD_Handler_t)(int argc, char *argv[]);

typedef struct
{
  const char        *name;    /* command word                              */
  const char        *usage;   /* one-line synopsis                         */
  const char        *help;    /* what it does, shown by `help`             */
  APP_CMD_Handler_t  fn;
} APP_CMD_t;

/** Dispatch a parsed command line.  @return 1 if a registered command ran. */
int APP_CMD_Dispatch(int argc, char *argv[]);
void APP_CMD_Poll(void);

/** Print every registered command, grouped under a single heading. */
void APP_CMD_PrintHelp(void);

/** Number of registered commands (used by `help` and by tests). */
unsigned APP_CMD_Count(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H */
