#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef char **rl_completion_func_t(const char *, int, int);
typedef char *rl_compentry_func_t(const char *, int);
typedef int rl_hook_func_t(void);
typedef int rl_command_func_t(int, int);
extern int rl_attempted_completion_over, rl_done;
extern rl_completion_func_t *rl_attempted_completion_function;
extern const char *rl_completer_word_break_characters;
extern rl_hook_func_t *rl_event_hook;
extern char **rl_completion_matches(const char *, rl_compentry_func_t *);
extern char *readline(const char *);
extern int rl_bind_key(int, rl_command_func_t *);
extern int rl_complete(int, int);
#ifdef __cplusplus
}
#endif
