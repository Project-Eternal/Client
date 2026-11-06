/* Readline for the embedded server: there is no console. The symbols AzerothCore's console loop references, doing nothing. */
#include <stddef.h>
typedef char** completion_func(const char*, int, int);
int rl_attempted_completion_over, rl_done;
completion_func* rl_attempted_completion_function;
const char* rl_completer_word_break_characters;
int (*rl_event_hook)(void);
char** rl_completion_matches(const char* text, char* (*fn)(const char*, int)) { (void) text; (void) fn; return NULL; }
char* readline(const char* prompt) { (void) prompt; return NULL; }
int rl_bind_key(int key, int (*fn)(int, int)) { (void) key; (void) fn; return 0; }
int rl_complete(int a, int b) { (void) a; (void) b; return 0; }
void add_history(const char* line) { (void) line; }
